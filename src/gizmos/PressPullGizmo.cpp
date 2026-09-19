#include "gizmos/PressPullGizmo.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>

namespace lcad {

namespace {

// Half the face's representative size reads as an arrow rather than a
// needle on a small face and stays out of the way on a large one.
constexpr double kArrowShare  = 0.5;
constexpr double kMinLength   = 2.0;

// Proportions of the arrow itself, as fractions of its total length.
constexpr double kHeadShare        = 0.28;
constexpr double kShaftRadiusShare = 0.035;
constexpr double kHeadRadiusShare  = 0.10;

// Two lines whose directions are this close to parallel have no usable
// closest point: the answer shoots off to infinity long before they are
// exactly parallel.
constexpr double kParallelTolerance = 1.0e-6;

std::string OcctMessage(const Standard_Failure& theFailure, const std::string& theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message) : theFallback;
}

} // namespace

bool ComputePressPullArrow(const TopoDS_Face& theFace,
                           PressPullArrow&    theArrow,
                           std::string&       theError)
{
    if (theFace.IsNull()) {
        theError = "no face selected";
        return false;
    }

    try {
        BRepAdaptor_Surface surface(theFace);
        if (surface.GetType() != GeomAbs_Plane) {
            theError = "the press/pull arrow needs a flat face";
            return false;
        }

        gp_Dir normal = surface.Plane().Axis().Direction();
        if (theFace.Orientation() == TopAbs_REVERSED) {
            normal.Reverse();
        }

        GProp_GProps props;
        BRepGProp::SurfaceProperties(theFace, props);
        const double area = props.Mass();
        if (area <= 0.0) {
            theError = "that face has no area";
            return false;
        }

        theArrow.axis   = gp_Ax1(props.CentreOfMass(), normal);
        theArrow.length = PressPullArrowLength(std::sqrt(area));
        return true;
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "could not read the face");
        return false;
    }
}

double PressPullArrowLength(double theFaceSize)
{
    return std::max(kMinLength, kArrowShare * theFaceSize);
}

TopoDS_Shape MakePressPullArrowShape(const gp_Ax1& theAxis, double theLength)
{
    TopoDS_Compound arrow;
    BRep_Builder builder;
    builder.MakeCompound(arrow);

    if (theLength <= 0.0) {
        return arrow;
    }

    try {
        const double headLength  = theLength * kHeadShare;
        const double shaftLength = theLength - headLength;
        const gp_Pnt tail = theAxis.Location();
        const gp_Pnt neck = tail.Translated(gp_Vec(theAxis.Direction()) * shaftLength);

        BRepPrimAPI_MakeCylinder shaft(gp_Ax2(tail, theAxis.Direction()),
                                       theLength * kShaftRadiusShare, shaftLength);
        BRepPrimAPI_MakeCone head(gp_Ax2(neck, theAxis.Direction()),
                                  theLength * kHeadRadiusShare, 0.0, headLength);
        shaft.Build();
        head.Build();
        if (!shaft.IsDone() || !head.IsDone()) {
            return arrow;
        }
        builder.Add(arrow, shaft.Shape());
        builder.Add(arrow, head.Shape());
    } catch (const Standard_Failure&) {
        // A decoration that cannot be built is not worth failing a drag
        // over; the caller sees an empty compound and shows nothing.
    }
    return arrow;
}

double DragDistanceAlongAxis(const gp_Ax1& theAxis,
                             const gp_Pnt& theStart,
                             const gp_Pnt& theCurrent)
{
    const gp_Vec travel(theStart, theCurrent);
    return travel.Dot(gp_Vec(theAxis.Direction()));
}

bool ClosestPointOnAxis(const gp_Ax1& theAxis,
                        const gp_Lin& theRay,
                        gp_Pnt&       theResult)
{
    // Both directions are unit vectors, which collapses the usual
    // line-line closest-approach system to a single division.
    const gp_Vec u(theAxis.Direction());
    const gp_Vec v(theRay.Direction());
    const double b = u.Dot(v);
    const double denominator = 1.0 - b * b;
    if (std::fabs(denominator) < kParallelTolerance) {
        return false;
    }

    const gp_Vec w(theRay.Location(), theAxis.Location());
    const double d = u.Dot(w);
    const double e = v.Dot(w);

    const double alongAxis = (b * e - d) / denominator;
    theResult = theAxis.Location().Translated(u * alongAxis);
    return true;
}

double DistanceToSegment2d(double thePointX, double thePointY,
                           double theAx, double theAy,
                           double theBx, double theBy)
{
    const double abx = theBx - theAx;
    const double aby = theBy - theAy;
    const double apx = thePointX - theAx;
    const double apy = thePointY - theAy;

    const double lengthSquared = abx * abx + aby * aby;
    double parameter = 0.0;
    if (lengthSquared > 0.0) {
        parameter = (apx * abx + apy * aby) / lengthSquared;
        parameter = std::max(0.0, std::min(1.0, parameter));
    }

    const double dx = apx - parameter * abx;
    const double dy = apy - parameter * aby;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace lcad
