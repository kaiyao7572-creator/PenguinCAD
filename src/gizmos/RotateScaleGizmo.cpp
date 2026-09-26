#include "gizmos/RotateScaleGizmo.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRep_Builder.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <gp.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kRadToDeg = 180.0 / kPi;

// On-screen sizes, logical pixels. Fusion's move arrows are about a
// hundred pixels long; a ring of this radius and a handle of this length
// read as the same family at the same scale, big enough to aim at without
// hunting and small enough to leave the body in view.
constexpr double kRingRadiusPixels  = 90.0;
constexpr double kScaleLengthPixels = 100.0;
constexpr double kPickRadiusPixels  = 10.0;

// Ring thickness, as a fraction of its radius. Thin enough to read as a
// ring rather than a doughnut, fat enough to be visible at a glance.
constexpr double kRingTubeShare = 0.022;
constexpr double kMinRingTube   = 0.15;

// The scale stalk's radius, as a share of the cube on its end -- NOT of
// its length, which grows while it is dragged, and a stalk that fattened
// as it stretched would look like a different handle by the end.
constexpr double kScaleStalkShare = 0.28;

// The scale handle is only grabbable along its outer half. The inner half
// runs through the body, where a click means "pick this face" far more
// often than it means "resize".
constexpr double kScaleGrabFrom = 0.5;

// Points round a ring for the hit test. At 64 the gap between samples is
// under a tenth of the radius, so the straight segments between them are
// well inside the pick radius of the true circle.
constexpr int kRingSamples = 64;

// How close to edge-on a ring may be before a drag on it is refused.
// This is the cosine between the view ray and the ring's axis: at 0.02
// the crossing point is fifty ring-radii away and swings by whole
// degrees for one pixel of mouse travel, which is not a rotation anyone
// asked for.
constexpr double kGrazingCosine = 0.02;

// Nearer the pivot than this and the cursor is not on the ring at all,
// so there is no angle to read off it.
constexpr double kMinRadiusForAngle = 1.0e-9;

// A drag has to mean something before it becomes a timeline entry.
constexpr double kMinDragAngleDegrees = 1.0e-3;
constexpr double kMinScaleChange      = 1.0e-4;

// A factor at or below this is not a size: zero collapses the body and
// negative turns it inside out.
constexpr double kMinFactor = 1.0e-6;

} // namespace

// ---- the screen ----

GizmoScreen::GizmoScreen(const Handle(Graphic3d_Camera)& theCamera, int theWidth, int theHeight)
    : myCamera(theCamera), myWidth(theWidth), myHeight(theHeight)
{
}

bool GizmoScreen::IsValid() const
{
    return !myCamera.IsNull() && myWidth > 0 && myHeight > 0;
}

bool GizmoScreen::Project(const gp_Pnt& thePoint, double& theX, double& theY) const
{
    if (!IsValid()) {
        return false;
    }
    try {
        // V3d_View::Convert's mapping, minus its rounding to whole pixels:
        // the hit test measures distances in fractions of one.
        const gp_Pnt ndc = myCamera->Project(thePoint);
        theX = (ndc.X() + 1.0) * 0.5 * myWidth;
        theY = myHeight - 1 - (ndc.Y() + 1.0) * 0.5 * myHeight;
        return std::isfinite(theX) && std::isfinite(theY);
    } catch (const Standard_Failure&) {
        return false;
    }
}

bool GizmoScreen::RayThrough(double theX, double theY, gp_Lin& theRay) const
{
    if (!IsValid()) {
        return false;
    }
    try {
        // V3d_View::ConvertWithProj's inverse of the mapping above: two
        // depths through the same pixel give the line of sight.
        const double ndcX = 2.0 * theX / myWidth - 1.0;
        const double ndcY = 2.0 * (myHeight - 1 - theY) / myHeight - 1.0;
        const gp_Pnt nearPoint = myCamera->UnProject(gp_Pnt(ndcX, ndcY, -1.0));
        const gp_Pnt farPoint  = myCamera->UnProject(gp_Pnt(ndcX, ndcY, 1.0));
        const gp_Vec direction(nearPoint, farPoint);
        if (direction.Magnitude() <= gp::Resolution()) {
            return false;
        }
        theRay = gp_Lin(nearPoint, gp_Dir(direction));
        return true;
    } catch (const Standard_Failure&) {
        return false;  // a degenerate projection: nothing to drag along
    }
}

double GizmoScreen::ModelPerPixel(const gp_Pnt& theAnchor) const
{
    if (!IsValid()) {
        return 0.0;
    }
    try {
        const Handle(Graphic3d_TransformPers) persistence =
            new Graphic3d_TransformPers(Graphic3d_TMF_ZoomPers, theAnchor);
        const double scale = persistence->persistentScale(myCamera, myWidth, myHeight);
        return std::isfinite(scale) && scale > 0.0 ? scale : 0.0;
    } catch (const Standard_Failure&) {
        return 0.0;
    }
}

// ---- sizes ----

double RotateRingRadiusPixels()
{
    return kRingRadiusPixels;
}

double ScaleHandleLengthPixels()
{
    return kScaleLengthPixels;
}

double GizmoPickRadiusPixels()
{
    return kPickRadiusPixels;
}

// ---- rotation ----

gp_Ax1 GizmoAxis(int theIndex, const gp_Pnt& thePivot)
{
    switch (theIndex) {
    case 0:  return gp_Ax1(thePivot, gp::DX());
    case 1:  return gp_Ax1(thePivot, gp::DY());
    default: return gp_Ax1(thePivot, gp::DZ());
    }
}

gp_Ax2 RotationFrame(const gp_Ax1& theAxis)
{
    // gp_Ax2(P, N, Vx) projects Vx into the plane normal to N, so Vx only
    // has to be the world axis LEAST parallel to N for the projection to
    // be well conditioned. For the three world axes that picks out the
    // cyclic right-handed choice exactly: about +Z the angle runs from +X
    // toward +Y, about +X from +Y toward +Z, about +Y from +X toward -Z --
    // which is what a positive rotation about +Y actually does.
    const gp_Dir normal = theAxis.Direction();
    const double ax = std::fabs(normal.X());
    const double ay = std::fabs(normal.Y());
    const double az = std::fabs(normal.Z());

    gp_Dir reference = gp::DZ();
    if (ax <= ay && ax <= az) {
        reference = gp::DX();
    } else if (ay <= az) {
        reference = gp::DY();
    }
    return gp_Ax2(theAxis.Location(), normal, reference);
}

bool AngleOnPlane(const gp_Ax1& theAxis, const gp_Lin& theRay, double& theAngleRadians)
{
    const gp_Vec normal(theAxis.Direction());
    const gp_Vec direction(theRay.Direction());

    const double alongNormal = direction.Dot(normal);
    if (std::fabs(alongNormal) < kGrazingCosine) {
        return false;  // looking along the ring's edge
    }

    const gp_Vec toPlane(theRay.Location(), theAxis.Location());
    const double distance = toPlane.Dot(normal) / alongNormal;
    const gp_Pnt hit = theRay.Location().Translated(direction * distance);

    const gp_Vec spoke(theAxis.Location(), hit);
    if (spoke.Magnitude() < kMinRadiusForAngle) {
        return false;  // dead on the pivot: every angle is as true as any other
    }

    const gp_Ax2 frame = RotationFrame(theAxis);
    theAngleRadians = std::atan2(spoke.Dot(gp_Vec(frame.YDirection())),
                                 spoke.Dot(gp_Vec(frame.XDirection())));
    return true;
}

double WrapAngle(double theAngleRadians)
{
    // fmod rather than a while loop: a ray that grazed the plane can
    // hand back an angle of any size, and a loop would sit there.
    double wrapped = std::fmod(theAngleRadians + kPi, kTwoPi);
    if (wrapped < 0.0) {
        wrapped += kTwoPi;
    }
    return wrapped - kPi;
}

TopoDS_Shape MakeRotateRingShape(const gp_Ax1& theAxis, double theRadius)
{
    TopoDS_Compound ring;
    BRep_Builder builder;
    builder.MakeCompound(ring);

    if (theRadius <= 0.0) {
        return ring;
    }

    try {
        const double tube = std::max(kMinRingTube, theRadius * kRingTubeShare);
        BRepPrimAPI_MakeTorus torus(gp_Ax2(theAxis.Location(), theAxis.Direction()),
                                    theRadius, tube);
        torus.Build();
        if (!torus.IsDone()) {
            return ring;
        }
        builder.Add(ring, torus.Shape());
    } catch (const Standard_Failure&) {
        // A decoration that cannot be built is not worth failing a tool
        // over; the caller sees an empty compound and shows nothing.
    }
    return ring;
}

std::vector<gp_Pnt> SampleRing(const gp_Ax1& theAxis, double theRadius, int theCount)
{
    std::vector<gp_Pnt> points;
    if (theCount < 3 || theRadius <= 0.0) {
        return points;
    }

    // Sampled in RotationFrame's own terms, so the points march round the
    // ring in the same direction a positive angle does.
    const gp_Ax2 frame = RotationFrame(theAxis);
    const gp_Vec x(frame.XDirection());
    const gp_Vec y(frame.YDirection());

    points.reserve(static_cast<std::size_t>(theCount));
    for (int i = 0; i < theCount; ++i) {
        const double angle = kTwoPi * static_cast<double>(i) / static_cast<double>(theCount);
        points.push_back(theAxis.Location().Translated(x * (theRadius * std::cos(angle))
                                                     + y * (theRadius * std::sin(angle))));
    }
    return points;
}

int PickRotateRing(const GizmoScreen& theScreen, const gp_Pnt& thePivot, double theRadius,
                   double theX, double theY, double thePickPixels)
{
    if (!theScreen.IsValid() || !(theRadius > 0.0)) {
        return -1;
    }

    int    winner = -1;
    double best   = thePickPixels;
    for (int axis = 0; axis < 3; ++axis) {
        const std::vector<gp_Pnt> points =
            SampleRing(GizmoAxis(axis, thePivot), theRadius, kRingSamples);
        if (points.size() < 3) {
            continue;
        }

        std::vector<double> xs(points.size(), 0.0);
        std::vector<double> ys(points.size(), 0.0);
        bool projected = true;
        for (std::size_t i = 0; i < points.size() && projected; ++i) {
            projected = theScreen.Project(points[i], xs[i], ys[i]);
        }
        if (!projected) {
            continue;
        }

        for (std::size_t i = 0; i < points.size(); ++i) {
            const std::size_t next = (i + 1) % points.size();  // close the loop
            const double distance =
                DistanceToSegment2d(theX, theY, xs[i], ys[i], xs[next], ys[next]);
            if (distance < best) {
                best   = distance;
                winner = axis;
            }
        }
    }
    return winner;
}

bool RingDrag::Begin(const gp_Ax1& theAxis, const gp_Lin& thePressRay)
{
    myIsActive = false;
    double angle = 0.0;
    if (!AngleOnPlane(theAxis, thePressRay, angle)) {
        return false;
    }
    myAxis     = theAxis;
    myLast     = angle;
    myTotal    = 0.0;
    myIsActive = true;
    return true;
}

bool RingDrag::Update(const gp_Lin& theRay)
{
    if (!myIsActive) {
        return false;
    }
    double angle = 0.0;
    if (!AngleOnPlane(myAxis, theRay, angle)) {
        return false;
    }
    // Accumulated from WRAPPED differences, so a drag can pass half a
    // turn, or cross the frame's zero, without the total jumping sign.
    myTotal += WrapAngle(angle - myLast);
    myLast = angle;
    return true;
}

double RingDrag::TotalDegrees() const
{
    return myTotal * kRadToDeg;
}

// ---- uniform scale ----

gp_Dir ScaleHandleDirection()
{
    return gp_Dir(1.0, 1.0, 1.0);
}

TopoDS_Shape MakeScaleHandleShape(const gp_Ax1& theAxis, double theOffset, double theSize)
{
    TopoDS_Compound handle;
    BRep_Builder builder;
    builder.MakeCompound(handle);

    if (theOffset <= 0.0 || theSize <= 0.0) {
        return handle;
    }

    try {
        BRepPrimAPI_MakeCylinder stalk(gp_Ax2(theAxis.Location(), theAxis.Direction()),
                                       theSize * kScaleStalkShare, theOffset);
        stalk.Build();
        if (!stalk.IsDone()) {
            return handle;
        }
        builder.Add(handle, stalk.Shape());

        // The cube is centred ON the handle's tip, so the distance the
        // drag maths works in -- pivot to tip -- is the distance the user
        // sees between the pivot and the middle of the thing they grabbed.
        const gp_Pnt tip = theAxis.Location().Translated(gp_Vec(theAxis.Direction()) * theOffset);
        const double half = theSize * 0.5;
        BRepPrimAPI_MakeBox cube(gp_Pnt(tip.X() - half, tip.Y() - half, tip.Z() - half),
                                 theSize, theSize, theSize);
        cube.Build();
        if (!cube.IsDone()) {
            return handle;
        }
        builder.Add(handle, cube.Shape());
    } catch (const Standard_Failure&) {
    }
    return handle;
}

bool PickScaleHandle(const GizmoScreen& theScreen, const gp_Ax1& theAxis, double theLength,
                     double theX, double theY, double thePickPixels)
{
    if (!theScreen.IsValid() || !(theLength > 0.0)) {
        return false;
    }
    const gp_Vec along(theAxis.Direction());
    const gp_Pnt from = theAxis.Location().Translated(along * (theLength * kScaleGrabFrom));
    const gp_Pnt to   = theAxis.Location().Translated(along * theLength);

    double fromX = 0.0, fromY = 0.0, toX = 0.0, toY = 0.0;
    if (!theScreen.Project(from, fromX, fromY) || !theScreen.Project(to, toX, toY)) {
        return false;
    }
    return DistanceToSegment2d(theX, theY, fromX, fromY, toX, toY) <= thePickPixels;
}

bool ScaleFactorFromDrag(double theRestOffset, double theDragDistance, double& theFactor)
{
    if (!(theRestOffset > 0.0) || !std::isfinite(theDragDistance)) {
        return false;
    }
    const double factor = (theRestOffset + theDragDistance) / theRestOffset;
    if (!std::isfinite(factor) || factor < kMinFactor) {
        return false;
    }
    theFactor = factor;
    return true;
}

bool ScaleDrag::Begin(const gp_Ax1& theAxis, double theRestLength, const gp_Lin& thePressRay)
{
    myIsActive = false;
    if (!(theRestLength > 0.0) || !ClosestPointOnAxis(theAxis, thePressRay, myStart)) {
        return false;
    }
    myAxis       = theAxis;
    myRestLength = theRestLength;
    myFactor     = 1.0;
    myIsActive   = true;
    return true;
}

bool ScaleDrag::Update(const gp_Lin& theRay)
{
    if (!myIsActive) {
        return false;
    }
    gp_Pnt onAxis;
    if (!ClosestPointOnAxis(myAxis, theRay, onAxis)) {
        return false;
    }
    // The tip moves by exactly as much as the cursor has along the
    // handle, wherever on the handle the press landed -- so the cube
    // stays under the hand that is dragging it.
    const double travel = DragDistanceAlongAxis(myAxis, myStart, onAxis);
    double factor = 1.0;
    if (!ScaleFactorFromDrag(myRestLength, travel, factor)) {
        return false;
    }
    myFactor = factor;
    return true;
}

double MinimumDragAngleDegrees()
{
    return kMinDragAngleDegrees;
}

double MinimumScaleChange()
{
    return kMinScaleChange;
}

} // namespace lcad
