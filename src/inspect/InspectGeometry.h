#pragma once

#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <string>

namespace lcad {

// The arithmetic behind the INSPECT tools, kept free of Qt, AIS and the
// document so tests/inspect_test.cpp can prove every number the tools show
// without a window. The commands in InspectCommands.cpp only pick things
// and draw the answers.

// ---- Model Properties ----

struct ModelProperties
{
    bool        isValid = false;
    std::string error;

    double volume = 0.0;        // mm^3
    double area   = 0.0;        // mm^2
    gp_Pnt centreOfMass;

    bool   hasBounds = false;
    gp_Pnt boundsMin;
    gp_Pnt boundsMax;
};

// Never throws: an OCCT failure comes back as isValid=false with a message.
ModelProperties ComputeModelProperties(const TopoDS_Shape& theShape);

// ---- Measure ----

struct MeasureResult
{
    bool        isValid = false;
    std::string error;

    // The minimum distance between the two picks, and where it is attained
    // -- Fusion measures a face to a vertex as the shortest way between
    // them, not between two arbitrary points the user happened to click.
    double distance = 0.0;
    gp_Pnt onFirst;
    gp_Pnt onSecond;

    // Only when both picks have a direction to compare: a flat face (its
    // normal) or a straight edge. Folded into [0, 90] degrees, the angle
    // between the two as lines/planes, so parallel reads 0 whichever way
    // each face happens to be oriented.
    bool   hasAngle = false;
    double angleDegrees = 0.0;

    gp_Vec Delta() const { return gp_Vec(onFirst, onSecond); }
};

MeasureResult MeasureBetween(const TopoDS_Shape& theFirst, const TopoDS_Shape& theSecond);

// "Vertex", "Edge", "Face" or "Body": what the results panel calls a pick.
std::string KindOfPick(const TopoDS_Shape& theShape);

// The one number a single pick is asked for, with its label -- a face's
// area, an edge's length, a vertex's position, a body's volume -- in the
// document's length unit. The same question Fusion's Measure answers
// before the second pick.
std::string DescribePick(const TopoDS_Shape& theShape);

// A length in the document's unit with a FIXED three decimals. Measure
// results line up in a column and are compared with each other, so
// "20.000" beside "28.284" reads better than a trimmed "20".
std::string FormatMeasureLength(double theMillimetres);

// An area and a volume the same way, converted by the SQUARE and the CUBE
// of the length factor: a 25.4 mm square is 1.000 in², not 25.4.
std::string FormatMeasureArea(double theSquareMillimetres);
std::string FormatMeasureVolume(double theCubicMillimetres);

// ---- Section View ----

enum class SectionAxis
{
    X,   // the YZ plane, cutting across X
    Y,   // the XZ plane, cutting across Y
    Z    // the XY plane, cutting across Z
};

// Where along theAxis the middle of theShape's bounding box is -- the
// default cut, so the first thing a section shows is the inside of the
// part rather than nothing at all. 0 for an empty shape.
double SectionMiddle(const TopoDS_Shape& theShape, SectionAxis theAxis);

// The plane to hand to Graphic3d_ClipPlane for a cut at theOffset along
// theAxis that REMOVES the positive side of the axis when
// theRemovePositive is true, the negative side otherwise.
//
// OCCT keeps the half-space where the plane's equation is positive and
// discards the rest (measured on screen: a +Z plane at z=0 left a box
// standing on the XY plane completely intact), so the plane's normal
// points at what STAYS.
gp_Pln SectionClipPlane(SectionAxis theAxis, double theOffset, bool theRemovePositive);

// True when OCCT would keep thePoint under a clip plane built by
// SectionClipPlane. The same sign rule, written once, so the tests can
// ask which half survives rather than re-deriving plane equations.
bool SectionKeeps(const gp_Pln& theClipPlane, const gp_Pnt& thePoint);

// Which side a section should remove when nobody has said: the half
// nearer the eye, so the cut face looks at the viewer. theTowardEye is
// the direction from the scene to the camera (V3d_View::Proj).
bool SectionRemovesPositive(SectionAxis theAxis, const gp_Vec& theTowardEye);

// "YZ", "XZ", "XY" -- the plane's name, the way Fusion names origin planes.
std::string SectionPlaneName(SectionAxis theAxis);

// "X", "Y", "Z" -- the axis the offset is measured along.
std::string SectionAxisName(SectionAxis theAxis);

} // namespace lcad
