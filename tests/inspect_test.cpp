// The INSPECT tools' numbers: Model Properties, Measure, Section View.
//
// Every expected value is worked out by hand in the comment beside it,
// from a 20 mm cube with one corner on the origin -- the same box the
// on-screen checks use -- so nothing is read back out of the code under
// test. Picks are found by where they ARE (a face by its centre, an edge
// by its midpoint), never by an index into OCCT's exploration order.
//
// What is NOT here: turning a click into a picked sub-shape, drawing the
// result, and whether OCCT really discards the side of a clip plane that
// SectionKeeps says it does. Those need a live viewer and were checked on
// screen; the sign rule they rest on is pinned below so it cannot drift.
#include "core/Units.h"
#include "inspect/InspectGeometry.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <iostream>
#include <string>

using namespace lcad;

static int failures = 0;

static void check(bool theOk, const std::string& theWhat)
{
    std::cout << (theOk ? "  PASS  " : "  FAIL  ") << theWhat << std::endl;
    if (!theOk) {
        ++failures;
    }
}

static void checkNear(double theValue, double theExpected, double theTolerance,
                      const std::string& theWhat)
{
    const bool ok = std::fabs(theValue - theExpected) <= theTolerance;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << "  (got " << theValue
              << ", expect " << theExpected << ")" << std::endl;
    if (!ok) {
        ++failures;
    }
}

static void checkText(const std::string& theValue, const std::string& theExpected,
                      const std::string& theWhat)
{
    const bool ok = theValue == theExpected;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << "  (got \"" << theValue
              << "\", expect \"" << theExpected << "\")" << std::endl;
    if (!ok) {
        ++failures;
    }
}

static void checkPoint(const gp_Pnt& theValue, double theX, double theY, double theZ,
                       const std::string& theWhat)
{
    const bool ok = theValue.Distance(gp_Pnt(theX, theY, theZ)) <= 1.0e-6;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << "  (got " << theValue.X() << ","
              << theValue.Y() << "," << theValue.Z() << "; expect " << theX << "," << theY << ","
              << theZ << ")" << std::endl;
    if (!ok) {
        ++failures;
    }
}

static TopoDS_Shape Cube(double theX = 0.0, double theY = 0.0, double theZ = 0.0,
                         double theSize = 20.0)
{
    return BRepPrimAPI_MakeBox(gp_Pnt(theX, theY, theZ), theSize, theSize, theSize).Shape();
}

static TopoDS_Shape VertexAt(const TopoDS_Shape& theShape, double theX, double theY, double theZ)
{
    for (TopExp_Explorer it(theShape, TopAbs_VERTEX); it.More(); it.Next()) {
        if (BRep_Tool::Pnt(TopoDS::Vertex(it.Current())).Distance(gp_Pnt(theX, theY, theZ))
            < 1.0e-9) {
            return it.Current();
        }
    }
    return TopoDS_Shape();
}

// The sub-shape of theType whose centre of mass sits at the point: a
// face by its centre, an edge by its midpoint.
static TopoDS_Shape CentredAt(const TopoDS_Shape& theShape, TopAbs_ShapeEnum theType, double theX,
                              double theY, double theZ)
{
    for (TopExp_Explorer it(theShape, theType); it.More(); it.Next()) {
        GProp_GProps props;
        if (theType == TopAbs_FACE) {
            BRepGProp::SurfaceProperties(it.Current(), props);
        } else {
            BRepGProp::LinearProperties(it.Current(), props);
        }
        if (props.CentreOfMass().Distance(gp_Pnt(theX, theY, theZ)) < 1.0e-9) {
            return it.Current();
        }
    }
    return TopoDS_Shape();
}

static void testModelProperties()
{
    std::cout << "Model Properties" << std::endl;
    const ModelProperties cube = ComputeModelProperties(Cube());
    check(cube.isValid, "a 20 mm cube has properties");
    checkNear(cube.volume, 8000.0, 1.0e-6, "cube volume 20^3 = 8000 mm3");
    checkNear(cube.area, 2400.0, 1.0e-6, "cube area 6 x 20^2 = 2400 mm2");
    checkPoint(cube.centreOfMass, 10.0, 10.0, 10.0, "cube centre of mass at its middle");
    check(cube.hasBounds, "cube has bounds");
    checkPoint(cube.boundsMin, 0.0, 0.0, 0.0, "bounds start at the origin");
    checkPoint(cube.boundsMax, 20.0, 20.0, 20.0, "bounds end at the far corner");
    // The old dialog printed "-0.000": BRepBndLib::Add pads by tolerance.
    check(!std::signbit(cube.boundsMin.X()) && !std::signbit(cube.boundsMin.Y())
              && !std::signbit(cube.boundsMin.Z()),
          "bounds minimum is +0, never -0 (no \"-0.000\" in the dialog)");

    // A closed hollow: the cube with a 16 mm cube removed from its middle
    // (2 mm walls all round). Volume 8000 - 16^3 = 3904; area is both
    // skins, 2400 + 6 x 16^2 = 3936; still centred at (10,10,10).
    const TopoDS_Shape inner = Cube(2.0, 2.0, 2.0, 16.0);
    const TopoDS_Shape hollow = BRepAlgoAPI_Cut(Cube(), inner).Shape();
    const ModelProperties shell = ComputeModelProperties(hollow);
    check(shell.isValid, "a hollow cube has properties");
    checkNear(shell.volume, 3904.0, 1.0e-6, "hollow volume 8000 - 4096 = 3904 mm3");
    checkNear(shell.area, 3936.0, 1.0e-6, "hollow area 2400 + 1536 = 3936 mm2");
    checkPoint(shell.centreOfMass, 10.0, 10.0, 10.0, "hollow centre of mass unchanged");

    const ModelProperties empty = ComputeModelProperties(TopoDS_Shape());
    check(!empty.isValid && !empty.error.empty(), "an empty model says so instead of crashing");
}

static void testMeasure()
{
    std::cout << "Measure" << std::endl;
    const TopoDS_Shape cube = Cube();

    const TopoDS_Shape v000 = VertexAt(cube, 0, 0, 0);
    const TopoDS_Shape v0020 = VertexAt(cube, 0, 0, 20);
    const TopoDS_Shape v20020 = VertexAt(cube, 20, 0, 20);
    const TopoDS_Shape v202020 = VertexAt(cube, 20, 20, 20);
    const TopoDS_Shape v2000 = VertexAt(cube, 20, 0, 0);
    check(!v000.IsNull() && !v0020.IsNull() && !v20020.IsNull() && !v202020.IsNull()
              && !v2000.IsNull(),
          "found the cube's corners by position");

    // The three numbers the on-screen check reads back.
    checkNear(MeasureBetween(v0020, v20020).distance, 20.0, 1.0e-9, "vertex to vertex along an edge = 20");
    checkNear(MeasureBetween(v0020, v202020).distance, 20.0 * std::sqrt(2.0), 1.0e-9,
              "across a face diagonal = 20 sqrt2 = 28.2843");
    checkNear(MeasureBetween(v000, v202020).distance, 20.0 * std::sqrt(3.0), 1.0e-9,
              "through the body diagonal = 20 sqrt3 = 34.6410");
    checkText(FormatMeasureLength(MeasureBetween(v000, v202020).distance), "34.641 mm",
              "the body diagonal as the panel prints it");

    const MeasureResult edge = MeasureBetween(v0020, v20020);
    check(!edge.hasAngle, "two vertices have no angle between them");
    checkNear(edge.Delta().X(), 20.0, 1.0e-9, "delta X of the edge = 20");
    checkNear(edge.Delta().Y(), 0.0, 1.0e-9, "delta Y of the edge = 0");
    checkNear(edge.Delta().Z(), 0.0, 1.0e-9, "delta Z of the edge = 0");

    // Faces: the MINIMUM distance, which is what the old tool could not do
    // -- it measured to a point floating on the screen plane instead.
    const TopoDS_Shape top = CentredAt(cube, TopAbs_FACE, 10, 10, 20);
    const TopoDS_Shape bottom = CentredAt(cube, TopAbs_FACE, 10, 10, 0);
    const TopoDS_Shape right = CentredAt(cube, TopAbs_FACE, 20, 10, 10);
    check(!top.IsNull() && !bottom.IsNull() && !right.IsNull(), "found the faces by centre");

    const MeasureResult opposite = MeasureBetween(top, bottom);
    check(opposite.isValid, "opposite faces measure");
    checkNear(opposite.distance, 20.0, 1.0e-9, "top to bottom face = 20");
    check(opposite.hasAngle, "two flat faces have an angle");
    checkNear(opposite.angleDegrees, 0.0, 1.0e-9, "opposite faces are parallel: 0 deg");

    const MeasureResult adjacent = MeasureBetween(top, right);
    checkNear(adjacent.distance, 0.0, 1.0e-9, "adjacent faces touch: 0");
    checkNear(adjacent.angleDegrees, 90.0, 1.0e-9, "adjacent cube faces meet at 90 deg");

    // Top face to the bottom-front corner (20,0,0): straight down, 20,
    // landing on the face at (20,0,20).
    const MeasureResult faceVertex = MeasureBetween(top, v2000);
    checkNear(faceVertex.distance, 20.0, 1.0e-9, "top face to a bottom corner = 20");
    checkPoint(faceVertex.onFirst, 20.0, 0.0, 20.0, "closest point on the face is above the corner");
    checkPoint(faceVertex.onSecond, 20.0, 0.0, 0.0, "and the corner itself");

    // Skew edges: X-edge on the floor (y=0,z=0) and Y-edge on the roof
    // (x=0,z=20). Closest at (0,0,0)-(0,0,20): 20. At right angles.
    const TopoDS_Shape floorX = CentredAt(cube, TopAbs_EDGE, 10, 0, 0);
    const TopoDS_Shape roofY = CentredAt(cube, TopAbs_EDGE, 0, 10, 20);
    const MeasureResult skew = MeasureBetween(floorX, roofY);
    checkNear(skew.distance, 20.0, 1.0e-9, "skew box edges are 20 apart");
    check(skew.hasAngle, "two straight edges have an angle");
    checkNear(skew.angleDegrees, 90.0, 1.0e-9, "skew box edges are at 90 deg");

    // A vertical edge against the top face: they touch, and a line
    // standing straight up is at 90 deg to a flat plane (not 0, which is
    // what comparing the edge to the face's NORMAL would give).
    const TopoDS_Shape upright = CentredAt(cube, TopAbs_EDGE, 20, 0, 10);
    const MeasureResult edgeFace = MeasureBetween(upright, top);
    checkNear(edgeFace.distance, 0.0, 1.0e-9, "an upright edge touches the top face");
    checkNear(edgeFace.angleDegrees, 90.0, 1.0e-9, "upright edge to top face = 90 deg");
    // And a roof edge lies IN the top face's plane: 0 deg.
    checkNear(MeasureBetween(roofY, top).angleDegrees, 0.0, 1.0e-9, "roof edge lies in the top face: 0 deg");

    // Bodies: a second cube 5 mm beyond the first along X.
    const TopoDS_Shape other = Cube(25.0, 0.0, 0.0);
    checkNear(MeasureBetween(cube, other).distance, 5.0, 1.0e-9, "body to body gap = 5");

    check(!MeasureBetween(TopoDS_Shape(), v000).isValid, "a missing pick is refused, not measured");
}

static void testDescribe()
{
    std::cout << "Describing one pick" << std::endl;
    const TopoDS_Shape cube = Cube();
    checkText(KindOfPick(VertexAt(cube, 0, 0, 20)), "Vertex", "a vertex is called a vertex");
    checkText(KindOfPick(CentredAt(cube, TopAbs_EDGE, 10, 0, 0)), "Edge", "an edge is an edge");
    checkText(KindOfPick(CentredAt(cube, TopAbs_FACE, 10, 10, 20)), "Face", "a face is a face");
    checkText(KindOfPick(cube), "Body", "a solid is a body");

    checkText(DescribePick(CentredAt(cube, TopAbs_FACE, 10, 10, 20)), "Area 400.000 mm²",
              "a face reports its area, 20 x 20");
    checkText(DescribePick(CentredAt(cube, TopAbs_EDGE, 10, 0, 0)), "Length 20.000 mm",
              "an edge reports its length");
    checkText(DescribePick(VertexAt(cube, 0, 0, 20)), "Position 0.000 mm, 0.000 mm, 20.000 mm",
              "a vertex reports where it is");
    checkText(DescribePick(cube), "Volume 8000.000 mm³", "a body reports its volume");

    // Results follow the document's unit: a 25.4 mm edge is one inch, and
    // its 645.16 mm2 face is one square inch (the SQUARE of the factor).
    SetDefaultLengthUnit(LengthUnit::Inch);
    const TopoDS_Shape inchCube = Cube(0, 0, 0, 25.4);
    checkText(DescribePick(CentredAt(inchCube, TopAbs_EDGE, 12.7, 0, 0)), "Length 1.000 in",
              "in inches, a 25.4 mm edge is 1 in");
    checkText(DescribePick(CentredAt(inchCube, TopAbs_FACE, 12.7, 12.7, 25.4)),
              "Area 1.000 in²", "and its face is 1 square inch");
    SetDefaultLengthUnit(LengthUnit::Millimeter);
}

static void testSection()
{
    std::cout << "Section View" << std::endl;
    // Default cut through the middle of the part, per axis.
    checkNear(SectionMiddle(Cube(), SectionAxis::Z), 10.0, 1.0e-9, "middle of a 0..20 cube in Z = 10");
    checkNear(SectionMiddle(Cube(5.0, -30.0, 2.0), SectionAxis::X), 15.0, 1.0e-9,
              "middle of a 5..25 cube in X = 15");
    checkNear(SectionMiddle(Cube(5.0, -30.0, 2.0), SectionAxis::Y), -20.0, 1.0e-9,
              "middle of a -30..-10 cube in Y = -20");
    checkNear(SectionMiddle(TopoDS_Shape(), SectionAxis::Y), 0.0, 0.0, "an empty model cuts at 0");

    // Removing the +Z half at z=10 keeps (10,10,5) and discards (10,10,15).
    const gp_Pln cutTop = SectionClipPlane(SectionAxis::Z, 10.0, true);
    check(SectionKeeps(cutTop, gp_Pnt(10, 10, 5)), "removing +Z at 10 keeps a point at z=5");
    check(!SectionKeeps(cutTop, gp_Pnt(10, 10, 15)), "removing +Z at 10 discards a point at z=15");
    const gp_Pln cutBottom = SectionClipPlane(SectionAxis::Z, 10.0, false);
    check(!SectionKeeps(cutBottom, gp_Pnt(10, 10, 5)), "removing -Z at 10 discards z=5");
    check(SectionKeeps(cutBottom, gp_Pnt(10, 10, 15)), "removing -Z at 10 keeps z=15");
    const gp_Pln cutY = SectionClipPlane(SectionAxis::Y, -20.0, true);
    check(SectionKeeps(cutY, gp_Pnt(0, -25, 0)) && !SectionKeeps(cutY, gp_Pnt(0, -15, 0)),
          "the offset is along the axis, sign and all (Y = -20)");

    // The home view looks from (+X, -Y, +Z): the half to take away is the
    // one facing the camera -- +X, -Y, +Z -- so the cut face looks back.
    const gp_Vec towardEye(1.0, -1.0, 1.0);
    check(SectionRemovesPositive(SectionAxis::X, towardEye), "home view: an X cut removes +X");
    check(!SectionRemovesPositive(SectionAxis::Y, towardEye), "home view: a Y cut removes -Y");
    check(SectionRemovesPositive(SectionAxis::Z, towardEye), "home view: a Z cut removes +Z");

    // Hatch stripes a tenth of the largest dimension apart.
    checkNear(SectionHatchSpacing(Cube()), 2.0, 1.0e-9, "20 mm cube: stripes 2 mm apart");
    const TopoDS_Shape plate = BRepPrimAPI_MakeBox(gp_Pnt(-100, 0, 0), 400.0, 50.0, 2.0).Shape();
    checkNear(SectionHatchSpacing(plate), 40.0, 1.0e-9, "400 x 50 x 2 plate: 400 / 10 = 40 mm");
    checkNear(SectionHatchSpacing(TopoDS_Shape()), 1.0, 0.0, "an empty model still gets a spacing");

    checkText(SectionPlaneName(SectionAxis::Y), "XZ", "a Y cut is on the XZ plane");
    checkText(SectionAxisName(SectionAxis::X), "X", "and an X cut is measured along X");
}

int main()
{
    testModelProperties();
    testMeasure();
    testDescribe();
    testSection();

    if (failures == 0) {
        std::cout << "All inspect checks passed." << std::endl;
        return 0;
    }
    std::cout << failures << " inspect check(s) FAILED." << std::endl;
    return 1;
}
