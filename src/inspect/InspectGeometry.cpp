#include "inspect/InspectGeometry.h"

#include "core/Units.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS.hxx>
#include <gp_Dir.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lcad {

namespace {

// Bounding boxes and distances come back with noise in the last few bits;
// "-0.000" in a results panel reads as a bug even though it is not one.
double Clean(double theValue)
{
    return std::fabs(theValue) < 5.0e-7 ? 0.0 : theValue;
}

std::string FailureText(const Standard_Failure& theFailure)
{
    const char* message = theFailure.GetMessageString();
    return (message != nullptr && *message != '\0') ? message : "OCCT error";
}

// What a pick is "about" for the purpose of an angle: a flat face stands
// for its normal, a straight edge for its direction. isPlane says which,
// because the angle between a line and a plane is measured against the
// plane itself, not its normal.
struct Direction
{
    bool   has = false;
    bool   isPlane = false;
    gp_Dir dir;
};

Direction DirectionOf(const TopoDS_Shape& theShape)
{
    Direction result;
    if (theShape.IsNull()) {
        return result;
    }
    try {
        if (theShape.ShapeType() == TopAbs_FACE) {
            BRepAdaptor_Surface surface(TopoDS::Face(theShape));
            if (surface.GetType() == GeomAbs_Plane) {
                result.has = true;
                result.isPlane = true;
                result.dir = surface.Plane().Axis().Direction();
            }
        } else if (theShape.ShapeType() == TopAbs_EDGE) {
            BRepAdaptor_Curve curve(TopoDS::Edge(theShape));
            if (curve.GetType() == GeomAbs_Line) {
                result.has = true;
                result.dir = curve.Line().Direction();
            }
        }
    } catch (const Standard_Failure&) {
        result.has = false;
    }
    return result;
}

double Degrees(double theRadians)
{
    return theRadians * 180.0 / M_PI;
}

gp_Dir AxisDirection(SectionAxis theAxis)
{
    switch (theAxis) {
        case SectionAxis::X: return gp_Dir(1.0, 0.0, 0.0);
        case SectionAxis::Y: return gp_Dir(0.0, 1.0, 0.0);
        case SectionAxis::Z: break;
    }
    return gp_Dir(0.0, 0.0, 1.0);
}

} // namespace

ModelProperties ComputeModelProperties(const TopoDS_Shape& theShape)
{
    ModelProperties result;
    if (theShape.IsNull()) {
        result.error = "The model is empty.";
        return result;
    }

    try {
        GProp_GProps volumeProps;
        GProp_GProps surfaceProps;
        // OnlyClosed=false: an open or imperfect shape still gets a (less
        // meaningful but non-crashing) number rather than nothing.
        BRepGProp::VolumeProperties(theShape, volumeProps);
        BRepGProp::SurfaceProperties(theShape, surfaceProps);
        result.volume = volumeProps.Mass();
        result.area = surfaceProps.Mass();
        const gp_Pnt centre = volumeProps.CentreOfMass();
        result.centreOfMass = gp_Pnt(Clean(centre.X()), Clean(centre.Y()), Clean(centre.Z()));

        // AddOptimal without triangulation or tolerance, rather than Add:
        // Add pads the box by every sub-shape's tolerance, so a box built
        // from the origin reported its corner at -0.0000001 and the dialog
        // showed "-0.000".
        Bnd_Box box;
        BRepBndLib::AddOptimal(theShape, box, Standard_False, Standard_False);
        if (!box.IsVoid()) {
            Standard_Real xmin = 0.0, ymin = 0.0, zmin = 0.0, xmax = 0.0, ymax = 0.0, zmax = 0.0;
            box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
            result.hasBounds = true;
            result.boundsMin = gp_Pnt(Clean(xmin), Clean(ymin), Clean(zmin));
            result.boundsMax = gp_Pnt(Clean(xmax), Clean(ymax), Clean(zmax));
        }
    } catch (const Standard_Failure& e) {
        result.error = FailureText(e);
        return result;
    }

    result.isValid = true;
    return result;
}

MeasureResult MeasureBetween(const TopoDS_Shape& theFirst, const TopoDS_Shape& theSecond)
{
    MeasureResult result;
    if (theFirst.IsNull() || theSecond.IsNull()) {
        result.error = "Pick two things to measure between.";
        return result;
    }

    try {
        BRepExtrema_DistShapeShape extrema(theFirst, theSecond);
        extrema.Perform();
        if (!extrema.IsDone() || extrema.NbSolution() < 1) {
            result.error = "Could not find the distance between these.";
            return result;
        }
        result.distance = Clean(extrema.Value());
        result.onFirst = extrema.PointOnShape1(1);
        result.onSecond = extrema.PointOnShape2(1);
    } catch (const Standard_Failure& e) {
        result.error = FailureText(e);
        return result;
    }

    const Direction first = DirectionOf(theFirst);
    const Direction second = DirectionOf(theSecond);
    if (first.has && second.has) {
        // |cos| folds 180 onto 0: two faces of a box that face away from
        // each other are still parallel, and that is what the user asks.
        const double cosine = std::min(1.0, std::fabs(first.dir.Dot(second.dir)));
        double angle = Degrees(std::acos(cosine));
        if (first.isPlane != second.isPlane) {
            // A line against a plane: the angle to the plane is the
            // complement of the angle to its normal.
            angle = 90.0 - angle;
        }
        result.hasAngle = true;
        result.angleDegrees = Clean(angle);
    }

    result.isValid = true;
    return result;
}

std::string KindOfPick(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return std::string();
    }
    switch (theShape.ShapeType()) {
        case TopAbs_VERTEX: return "Vertex";
        case TopAbs_EDGE:   return "Edge";
        case TopAbs_FACE:   return "Face";
        default:            return "Body";
    }
}

std::string DescribePick(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return std::string();
    }
    try {
        switch (theShape.ShapeType()) {
            case TopAbs_VERTEX: {
                const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(theShape));
                return "Position " + FormatMeasureLength(p.X()) + ", "
                     + FormatMeasureLength(p.Y()) + ", " + FormatMeasureLength(p.Z());
            }
            case TopAbs_EDGE: {
                GProp_GProps props;
                BRepGProp::LinearProperties(theShape, props);
                return "Length " + FormatMeasureLength(props.Mass());
            }
            case TopAbs_FACE: {
                GProp_GProps props;
                BRepGProp::SurfaceProperties(theShape, props);
                return "Area " + FormatMeasureArea(props.Mass());
            }
            default: {
                GProp_GProps props;
                BRepGProp::VolumeProperties(theShape, props);
                return "Volume " + FormatMeasureVolume(props.Mass());
            }
        }
    } catch (const Standard_Failure& e) {
        return FailureText(e);
    }
}

std::string FormatMeasureLength(double theMillimetres)
{
    const LengthUnit unit = DefaultLengthUnit();
    char text[64];
    std::snprintf(text, sizeof(text), "%.3f", Clean(FromMillimeters(theMillimetres, unit)));
    return std::string(text) + " " + SymbolOf(unit);
}

std::string FormatMeasureArea(double theSquareMillimetres)
{
    const LengthUnit unit = DefaultLengthUnit();
    const double     per  = MillimetersPer(unit);
    const double value = per > 0.0 ? theSquareMillimetres / (per * per) : theSquareMillimetres;
    char text[64];
    std::snprintf(text, sizeof(text), "%.3f", Clean(value));
    return std::string(text) + " " + SymbolOf(unit) + "²";
}

std::string FormatMeasureVolume(double theCubicMillimetres)
{
    const LengthUnit unit = DefaultLengthUnit();
    const double     per  = MillimetersPer(unit);
    const double value =
        per > 0.0 ? theCubicMillimetres / (per * per * per) : theCubicMillimetres;
    char text[64];
    std::snprintf(text, sizeof(text), "%.3f", Clean(value));
    return std::string(text) + " " + SymbolOf(unit) + "³";
}

double SectionMiddle(const TopoDS_Shape& theShape, SectionAxis theAxis)
{
    if (theShape.IsNull()) {
        return 0.0;
    }
    try {
        Bnd_Box box;
        BRepBndLib::AddOptimal(theShape, box, Standard_False, Standard_False);
        if (box.IsVoid()) {
            return 0.0;
        }
        Standard_Real xmin = 0.0, ymin = 0.0, zmin = 0.0, xmax = 0.0, ymax = 0.0, zmax = 0.0;
        box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        switch (theAxis) {
            case SectionAxis::X: return Clean(0.5 * (xmin + xmax));
            case SectionAxis::Y: return Clean(0.5 * (ymin + ymax));
            case SectionAxis::Z: return Clean(0.5 * (zmin + zmax));
        }
    } catch (const Standard_Failure&) {
        return 0.0;
    }
    return 0.0;
}

gp_Pln SectionClipPlane(SectionAxis theAxis, double theOffset, bool theRemovePositive)
{
    const gp_Dir axis = AxisDirection(theAxis);
    const gp_Pnt origin(axis.X() * theOffset, axis.Y() * theOffset, axis.Z() * theOffset);
    // The normal points at what is KEPT, so removing the positive side
    // means a normal pointing down the axis.
    return gp_Pln(origin, theRemovePositive ? axis.Reversed() : axis);
}

bool SectionKeeps(const gp_Pln& theClipPlane, const gp_Pnt& thePoint)
{
    Standard_Real a = 0.0, b = 0.0, c = 0.0, d = 0.0;
    theClipPlane.Coefficients(a, b, c, d);
    return a * thePoint.X() + b * thePoint.Y() + c * thePoint.Z() + d >= 0.0;
}

bool SectionRemovesPositive(SectionAxis theAxis, const gp_Vec& theTowardEye)
{
    const gp_Dir axis = AxisDirection(theAxis);
    const double along = theTowardEye.X() * axis.X() + theTowardEye.Y() * axis.Y()
                       + theTowardEye.Z() * axis.Z();
    // Looking straight along the plane (along == 0) either half is as
    // good; positive keeps the answer stable instead of flickering.
    return along >= 0.0;
}

std::string SectionPlaneName(SectionAxis theAxis)
{
    switch (theAxis) {
        case SectionAxis::X: return "YZ";
        case SectionAxis::Y: return "XZ";
        case SectionAxis::Z: break;
    }
    return "XY";
}

std::string SectionAxisName(SectionAxis theAxis)
{
    switch (theAxis) {
        case SectionAxis::X: return "X";
        case SectionAxis::Y: return "Y";
        case SectionAxis::Z: break;
    }
    return "Z";
}

} // namespace lcad
