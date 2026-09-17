// Axis inference and snap candidates -- the geometry behind "drawn lines
// come out straight". Pure logic, so this runs with no display.
#include "sketch/SketchGeometry.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchEntity.h"

#include <cmath>
#include <iostream>

using namespace lcad;

static int failures = 0;
static void check(bool ok, const std::string& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) ++failures;
}

static bool Near(const gp_Pnt2d& a, double x, double y)
{
    return std::fabs(a.X() - x) < 1e-9 && std::fabs(a.Y() - y) < 1e-9;
}

int main()
{
    const gp_Pnt2d anchor(0.0, 0.0);

    std::cout << "-- a near-horizontal drag locks flat --" << std::endl;
    // 100mm across, 1mm up = 0.57 degrees: well inside tolerance.
    check(Near(SketchGeometry::AxisInferred(anchor, gp_Pnt2d(100.0, 1.0)), 100.0, 0.0),
          "0.57 deg off horizontal snaps to y = anchor");

    std::cout << "-- a near-vertical drag locks upright --" << std::endl;
    check(Near(SketchGeometry::AxisInferred(anchor, gp_Pnt2d(1.0, 100.0)), 0.0, 100.0),
          "0.57 deg off vertical snaps to x = anchor");

    std::cout << "-- a deliberate angle is left alone --" << std::endl;
    check(Near(SketchGeometry::AxisInferred(anchor, gp_Pnt2d(100.0, 100.0)), 100.0, 100.0),
          "45 deg is untouched");
    check(Near(SketchGeometry::AxisInferred(anchor, gp_Pnt2d(100.0, 30.0)), 100.0, 30.0),
          "17 deg is untouched");

    std::cout << "-- the boundary behaves --" << std::endl;
    // 3 degrees is the tolerance; 5 degrees must not lock.
    const double fiveDeg = 100.0 * std::tan(5.0 * M_PI / 180.0);
    check(Near(SketchGeometry::AxisInferred(anchor, gp_Pnt2d(100.0, fiveDeg)), 100.0, fiveDeg),
          "5 deg off horizontal does NOT lock");

    std::cout << "-- degenerate input is safe --" << std::endl;
    check(Near(SketchGeometry::AxisInferred(anchor, gp_Pnt2d(0.0, 0.0)), 0.0, 0.0),
          "zero-length segment returns unchanged");

    std::cout << "-- negative directions lock too --" << std::endl;
    check(Near(SketchGeometry::AxisInferred(anchor, gp_Pnt2d(-100.0, -1.0)), -100.0, 0.0),
          "leftwards near-horizontal locks");

    std::cout << "-- snap candidates include origin and midpoints --" << std::endl;
    SketchFeature sketch(SketchFeature::PlaneXY(), 0.0);
    sketch.AddEntity(SketchEntity::MakeLine(gp_Pnt2d(10.0, 20.0), gp_Pnt2d(30.0, 20.0)));
    const std::vector<gp_Pnt2d> snaps = sketch.SnapPoints();

    bool hasOrigin = false, hasMid = false, hasEnd = false;
    for (const gp_Pnt2d& p : snaps) {
        if (Near(p, 0.0, 0.0))   hasOrigin = true;
        if (Near(p, 20.0, 20.0)) hasMid = true;
        if (Near(p, 30.0, 20.0)) hasEnd = true;
    }
    check(hasOrigin, "sketch origin is snappable");
    check(hasMid,    "line midpoint is snappable");
    check(hasEnd,    "line endpoint is still snappable");

    std::cout << (failures == 0 ? "\nALL INFERENCE TESTS PASSED\n" : "\nFAILURES\n");
    return failures == 0 ? 0 : 1;
}
