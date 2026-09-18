// The Origin folder and construction geometry: the planes, axes and
// points a user builds to position things no existing geometry gives them
// a handle on. All parametric, so all testable without a display.
#include "core/ConstructionGeometry.h"
#include "core/Document.h"
#include "core/Origin.h"
#include "features/ConstructionFeatures.h"
#include "features/PrimitiveFeatures.h"

#include <cmath>
#include <iostream>
#include <memory>
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

// Read a plane back off a feature that is already in a rebuilt document.
static bool PlaneOf(const FeaturePtr& theFeature, gp_Ax3& theResult)
{
    ConstructionGeometry* geometry = AsConstructionGeometry(theFeature.get());
    return geometry != nullptr && geometry->AsPlane(theResult);
}

int main()
{
    // ---- 1. the Origin folder holds Fusion's seven entities ----
    {
        const std::vector<OriginEntity>& origin = OriginEntities();
        check(origin.size() == 7, "seven origin entities");

        // Fusion draws the three planes, then the three axes, then Origin.
        check(origin[0].name == "XY" && origin[1].name == "XZ" && origin[2].name == "YZ",
              "the three planes come first, XY XZ YZ");
        check(origin[3].name == "X" && origin[4].name == "Y" && origin[5].name == "Z",
              "then the three axes");
        check(origin[6].name == "Origin" && origin[6].type == EntityType::ConstructionPoint,
              "and the origin point last");

        check(origin[0].type == EntityType::ConstructionPlane, "XY is a construction plane");
        check(origin[3].type == EntityType::ConstructionAxis, "X is a construction axis");

        // The normals are what every sketch and offset depends on.
        checkNear(origin[0].plane.Direction().Z(), 1.0, 1.0e-12, "XY's normal is +Z");
        checkNear(origin[1].plane.Direction().Y(), -1.0, 1.0e-12, "XZ's normal is -Y");
        checkNear(origin[2].plane.Direction().X(), 1.0, 1.0e-12, "YZ's normal is +X");

        // A sketch on XZ has to read as world (X, Z), or a front view is
        // mirrored.
        checkNear(origin[1].plane.XDirection().X(), 1.0, 1.0e-12, "XZ's u axis is world X");
        checkNear(origin[1].plane.YDirection().Z(), 1.0, 1.0e-12, "XZ's v axis is world Z");

        check(FindOriginEntity("YZ") != nullptr, "YZ is findable by name");
        check(FindOriginEntity("Nope") == nullptr, "a name nothing carries finds nothing");
    }

    // ---- 2. an offset plane ----
    {
        Document doc;
        auto plane = std::make_shared<ConstructionPlaneFeature>("XY", 25.0);
        plane->SetName("Plane1");
        doc.AddFeature(plane);

        check(doc.Errors().empty(), "an offset plane computes cleanly");
        gp_Ax3 built;
        check(PlaneOf(plane, built), "and reports its plane");
        checkNear(built.Location().Z(), 25.0, 1.0e-12, "it sits 25mm up the XY normal");
        checkNear(built.Direction().Z(), 1.0, 1.0e-12, "and stays parallel to XY");

        // Construction geometry makes no solid, exactly like a sketch.
        check(doc.Shape().IsNull(), "a construction plane adds no geometry to the model");
        check(doc.Bodies().empty(), "and no body");

        // Editing the offset is what makes it parametric.
        Parameter offset = Parameter::MakeDouble("Offset", -10.0);
        check(plane->SetParameter(offset), "the offset is editable");
        doc.Rebuild();
        check(PlaneOf(plane, built) && std::fabs(built.Location().Z() + 10.0) < 1.0e-12,
              "and the plane moves to -10");
    }

    // ---- 3. planes chain off each other ----
    {
        Document doc;
        auto first = std::make_shared<ConstructionPlaneFeature>("XY", 10.0);
        first->SetName("Plane1");
        doc.AddFeature(first);

        auto second = std::make_shared<ConstructionPlaneFeature>("Plane1", 5.0);
        second->SetName("Plane2");
        doc.AddFeature(second);

        gp_Ax3 built;
        check(doc.Errors().empty(), "a plane offset from another plane computes");
        check(PlaneOf(second, built), "and reports its plane");
        checkNear(built.Location().Z(), 15.0, 1.0e-12, "10 + 5 = 15 up from the origin");
    }

    // ---- 4. only upstream, and the origin folder always wins ----
    {
        Document doc;
        // A user names a construction plane "XY". Every sketch already
        // drawn on the world XY must not silently move onto it.
        auto impostor = std::make_shared<ConstructionPlaneFeature>("XY", 100.0);
        impostor->SetName("XY");
        doc.AddFeature(impostor);

        auto offset = std::make_shared<ConstructionPlaneFeature>("XY", 3.0);
        offset->SetName("Plane2");
        doc.AddFeature(offset);

        gp_Ax3 built;
        check(PlaneOf(offset, built), "the second plane computes");
        checkNear(built.Location().Z(), 3.0, 1.0e-12,
                  "'XY' still means the world XY, not the feature named XY");
    }
    {
        Document doc;
        auto downstream = std::make_shared<ConstructionPlaneFeature>("Plane2", 5.0);
        downstream->SetName("Plane1");
        doc.AddFeature(downstream);
        auto later = std::make_shared<ConstructionPlaneFeature>("XY", 1.0);
        later->SetName("Plane2");
        doc.AddFeature(later);

        check(!doc.Errors().empty(), "a plane cannot reference one BELOW it in the timeline");
        gp_Ax3 built;
        check(!PlaneOf(downstream, built), "and reports no plane at all");
    }

    // ---- 5. a plane at an angle ----
    {
        Document doc;
        auto plane = std::make_shared<ConstructionPlaneFeature>("XY", 0.0);
        plane->SetKind(PlaneKind::AtAngle);
        plane->SetAngleDegrees(90.0);
        plane->SetName("Plane1");
        doc.AddFeature(plane);

        gp_Ax3 built;
        check(PlaneOf(plane, built), "an angled plane computes");
        // XY turned 90 degrees about its own X axis points along -Y... or
        // +Y, depending on the turn direction; either is upright.
        checkNear(std::fabs(built.Direction().Y()), 1.0, 1.0e-9,
                  "XY turned 90 degrees about X is a vertical plane");
        checkNear(built.Direction().Z(), 0.0, 1.0e-9, "with nothing left pointing up");
    }

    // ---- 6. a midplane ----
    {
        Document doc;
        auto low = std::make_shared<ConstructionPlaneFeature>("XY", 0.0);
        low->SetName("Low");
        doc.AddFeature(low);
        auto high = std::make_shared<ConstructionPlaneFeature>("XY", 30.0);
        high->SetName("High");
        doc.AddFeature(high);

        auto middle = std::make_shared<ConstructionPlaneFeature>();
        middle->SetKind(PlaneKind::Midplane);
        middle->SetBasePlane("Low");
        middle->SetSecondPlane("High");
        middle->SetName("Middle");
        doc.AddFeature(middle);

        gp_Ax3 built;
        check(doc.Errors().empty(), "a midplane between two parallel planes computes");
        check(PlaneOf(middle, built), "and reports its plane");
        checkNear(built.Location().Z(), 15.0, 1.0e-12, "halfway between 0 and 30");

        // Between two planes that CROSS, "halfway" is a line, not a plane.
        auto crossing = std::make_shared<ConstructionPlaneFeature>();
        crossing->SetKind(PlaneKind::Midplane);
        crossing->SetBasePlane("XY");
        crossing->SetSecondPlane("YZ");
        crossing->SetName("Bad");
        doc.AddFeature(crossing);
        check(!doc.Errors().empty(), "a midplane between crossing planes is refused");
        check(!PlaneOf(crossing, built), "and produces no plane");
    }

    // ---- 7. a plane through three points ----
    {
        Document doc;
        auto plane = std::make_shared<ConstructionPlaneFeature>();
        plane->SetKind(PlaneKind::ThreePoints);
        plane->SetPoints(gp_Pnt(0, 0, 5), gp_Pnt(10, 0, 5), gp_Pnt(0, 10, 5));
        plane->SetName("Plane1");
        doc.AddFeature(plane);

        gp_Ax3 built;
        check(doc.Errors().empty(), "three points define a plane");
        check(PlaneOf(plane, built), "and it reports one");
        checkNear(std::fabs(built.Direction().Z()), 1.0, 1.0e-12, "all at z=5, so it is horizontal");
        checkNear(built.Location().Z(), 5.0, 1.0e-12, "and it sits at z=5");

        // Three points in a line define nothing.
        plane->SetPoints(gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0), gp_Pnt(20, 0, 0));
        doc.Rebuild();
        check(!doc.Errors().empty(), "three points in a line are refused");
        check(!PlaneOf(plane, built), "and leave no plane behind");
    }

    // ---- 8. construction axes ----
    {
        Document doc;
        auto axis = std::make_shared<ConstructionAxisFeature>();
        axis->SetPoints(gp_Pnt(1, 2, 3), gp_Pnt(1, 2, 13));
        axis->SetName("Axis1");
        doc.AddFeature(axis);

        ConstructionGeometry* geometry = AsConstructionGeometry(axis.get());
        gp_Ax1 built;
        check(geometry != nullptr && geometry->AsAxis(built), "a two-point axis computes");
        checkNear(built.Direction().Z(), 1.0, 1.0e-12, "it runs straight up");
        checkNear(built.Location().X(), 1.0, 1.0e-12, "rooted at the first point");

        gp_Pnt root;
        check(geometry->AsPoint(root), "an axis also answers as a point");
        checkNear(root.Y(), 2.0, 1.0e-12, "its root");

        // Two points in the same place define no direction.
        axis->SetPoints(gp_Pnt(4, 4, 4), gp_Pnt(4, 4, 4));
        doc.Rebuild();
        check(!doc.Errors().empty(), "a zero-length axis is refused");

        auto normal = std::make_shared<ConstructionAxisFeature>();
        normal->SetKind(AxisKind::PerpendicularToPlane);
        normal->SetBasePlane("YZ");
        normal->SetName("Axis2");
        doc.AddFeature(normal);
        ConstructionGeometry* second = AsConstructionGeometry(normal.get());
        check(second != nullptr && second->AsAxis(built), "an axis normal to YZ computes");
        checkNear(built.Direction().X(), 1.0, 1.0e-12, "and points along world X");
    }

    // ---- 9. construction points ----
    {
        Document doc;
        auto point = std::make_shared<ConstructionPointFeature>(gp_Pnt(7, 8, 9));
        point->SetName("Point1");
        doc.AddFeature(point);

        ConstructionGeometry* geometry = AsConstructionGeometry(point.get());
        gp_Pnt built;
        check(geometry != nullptr && geometry->AsPoint(built), "a typed-in point computes");
        checkNear(built.X(), 7.0, 1.0e-12, "x");
        checkNear(built.Z(), 9.0, 1.0e-12, "z");

        // Where the Z axis crosses a plane 25mm up: (0, 0, 25).
        auto raised = std::make_shared<ConstructionPlaneFeature>("XY", 25.0);
        raised->SetName("Plane1");
        doc.AddFeature(raised);

        auto crossing = std::make_shared<ConstructionPointFeature>();
        crossing->SetKind(PointKind::AxisPlaneIntersection);
        crossing->SetAxisName("Z");
        crossing->SetPlaneName("Plane1");
        crossing->SetName("Point2");
        doc.AddFeature(crossing);

        ConstructionGeometry* second = AsConstructionGeometry(crossing.get());
        check(second != nullptr && second->AsPoint(built), "an axis/plane crossing computes");
        checkNear(built.Z(), 25.0, 1.0e-9, "the Z axis meets a plane at z=25 there");
        checkNear(built.X(), 0.0, 1.0e-9, "on the axis");

        // An axis lying IN the plane never crosses it.
        crossing->SetAxisName("X");
        crossing->SetPlaneName("XY");
        doc.Rebuild();
        check(!doc.Errors().empty(), "an axis parallel to its plane is refused");
    }

    // ---- 10. construction geometry survives undo ----
    {
        Document doc;
        auto plane = std::make_shared<ConstructionPlaneFeature>("XZ", 12.0);
        plane->SetKind(PlaneKind::AtAngle);
        plane->SetAngleDegrees(30.0);
        plane->SetName("Plane1");
        doc.AddFeature(plane);

        auto box = std::make_shared<BoxFeature>();
        box->SetName("Box1");
        doc.AddFeature(box);
        check(doc.FeatureCount() == 2, "two features");

        doc.Undo();
        check(doc.FeatureCount() == 1, "undo drops the box");

        // The timeline is restored from CLONES, so the surviving plane is
        // a different object -- and must still know it is a 30-degree
        // plane off XZ, or undo silently resets construction geometry.
        FeaturePtr restored = doc.FeatureAt(0);
        auto* clone = dynamic_cast<ConstructionPlaneFeature*>(restored.get());
        check(clone != nullptr, "the survivor is still a construction plane");
        check(clone != plane.get(), "and is a clone, not the original pointer");
        if (clone != nullptr) {
            check(clone->Kind() == PlaneKind::AtAngle, "the clone kept its type");
            checkNear(clone->AngleDegrees(), 30.0, 1.0e-12, "and its angle");
            check(clone->BasePlane() == "XZ", "and its base plane");
        }
    }

    // ---- 11. the kind parameters round-trip as text ----
    {
        PlaneKind planeKind = PlaneKind::Offset;
        check(ParsePlaneKind("Midplane", planeKind) && planeKind == PlaneKind::Midplane,
              "a plane kind parses by label");
        check(ParsePlaneKind("three points", planeKind) && planeKind == PlaneKind::ThreePoints,
              "case does not matter");
        check(ParsePlaneKind("1", planeKind) && planeKind == PlaneKind::AtAngle,
              "and the bare index works too");
        check(!ParsePlaneKind("nonsense", planeKind), "nonsense is refused");
        check(!ParsePlaneKind("9", planeKind), "an out-of-range index is refused");

        AxisKind axisKind = AxisKind::TwoPoints;
        check(ParseAxisKind("Perpendicular To Plane", axisKind)
                  && axisKind == AxisKind::PerpendicularToPlane,
              "an axis kind parses");

        PointKind pointKind = PointKind::AtCoordinates;
        check(ParsePointKind("Axis And Plane", pointKind)
                  && pointKind == PointKind::AxisPlaneIntersection,
              "a point kind parses");

        check(PlaneKindNames().size() == 4, "four ways to build a plane");
        check(AxisKindNames().size() == 2, "two ways to build an axis");
        check(PointKindNames().size() == 2, "two ways to build a point");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL CONSTRUCTION TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " CONSTRUCTION TEST(S) FAILED" << std::endl;
    return 1;
}
