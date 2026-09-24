#include "gizmos/RotateScaleGizmo.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRep_Builder.hxx>
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

// A ring this much of the body's corner-to-corner size clears the body
// itself (whose half-diagonal is 0.5 of it) with room to grab, without
// flying off to where the user has to zoom out to find it.
constexpr double kRingShare = 0.62;
constexpr double kMinRing   = 4.0;   // mm, for a body too small to size one off

// Ring thickness, as a fraction of its radius. Thin enough to read as a
// ring rather than a doughnut, fat enough to be visible at a glance.
constexpr double kRingTubeShare = 0.022;
constexpr double kMinRingTube   = 0.15;

// The scale handle stands just outside where a ring would be, so the two
// gizmos read as the same family at the same scale.
constexpr double kScaleOffsetShare = 0.75;
constexpr double kMinScaleOffset   = 5.0;
constexpr double kScaleStalkShare  = 0.035;  // of the offset

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

double RotateRingRadius(double theBodySize)
{
    return std::max(kMinRing, kRingShare * theBodySize);
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

gp_Dir ScaleHandleDirection()
{
    return gp_Dir(1.0, 1.0, 1.0);
}

double ScaleHandleOffset(double theBodySize)
{
    return std::max(kMinScaleOffset, kScaleOffsetShare * theBodySize);
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
                                       std::max(0.05, theOffset * kScaleStalkShare),
                                       theOffset);
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

double MinimumDragAngleDegrees()
{
    return kMinDragAngleDegrees;
}

double MinimumScaleChange()
{
    return kMinScaleChange;
}

} // namespace lcad
