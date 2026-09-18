// The sketch entity taxonomy: every curve type Fusion has that this app
// can draw, classified the way Fusion classifies it, plus the geometry of
// the two that were missing -- control point splines and conics.
#include "core/Entity.h"
#include "sketch/SketchEntity.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchGeometry.h"

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

static void checkPoint(const gp_Pnt2d& theValue, double theX, double theY,
                       const std::string& theWhat)
{
    const bool ok = std::fabs(theValue.X() - theX) < 1.0e-6
                 && std::fabs(theValue.Y() - theY) < 1.0e-6;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << "  (got " << theValue.X() << ", "
              << theValue.Y() << "; expect " << theX << ", " << theY << ")" << std::endl;
    if (!ok) {
        ++failures;
    }
}

int main()
{
    const double kTwoPi = 2.0 * 3.14159265358979323846;

    // ---- 1. every entity reports the Fusion type it would report ----
    {
        check(SketchEntityType(SketchEntity::MakeLine(gp_Pnt2d(0, 0), gp_Pnt2d(1, 1)))
                  == EntityType::SketchLine, "a line is a SketchLine");
        check(SketchEntityType(SketchEntity::MakeCircle(gp_Pnt2d(0, 0), 5.0))
                  == EntityType::SketchCircle, "a circle is a SketchCircle");
        check(SketchEntityType(SketchEntity::MakeArc(gp_Pnt2d(0, 0), 5.0, 0.0, 1.0))
                  == EntityType::SketchArc, "an arc is a SketchArc");
        check(SketchEntityType(SketchEntity::MakePoint(gp_Pnt2d(1, 2)))
                  == EntityType::SketchPoint, "a point is a SketchPoint");
        check(SketchEntityType(SketchEntity::MakeSpline({gp_Pnt2d(0, 0), gp_Pnt2d(5, 5),
                                                         gp_Pnt2d(10, 0)}))
                  == EntityType::SketchFittedSpline, "a spline through points is a fitted spline");
        check(SketchEntityType(SketchEntity::MakeControlPointSpline(
                  {gp_Pnt2d(0, 0), gp_Pnt2d(5, 5), gp_Pnt2d(10, 0)}))
                  == EntityType::SketchControlPointSpline, "poles make a control point spline");
        check(SketchEntityType(SketchEntity::MakeConic(gp_Pnt2d(0, 0), gp_Pnt2d(10, 10),
                                                       gp_Pnt2d(20, 0), 0.5))
                  == EntityType::SketchConicCurve, "three points and a rho make a conic");

        // The one classification that is not one-to-one with a Kind: an
        // ellipse and an elliptical arc are the same geometry, and only
        // the sweep tells them apart.
        const SketchEntity whole =
            SketchEntity::MakeEllipse(gp_Pnt2d(0, 0), 10.0, 5.0, 0.0);
        check(SketchEntityType(whole) == EntityType::SketchEllipse,
              "a whole ellipse is a SketchEllipse");
        const SketchEntity part =
            SketchEntity::MakeEllipse(gp_Pnt2d(0, 0), 10.0, 5.0, 0.0, 0.0, 1.0);
        check(SketchEntityType(part) == EntityType::SketchEllipticalArc,
              "a partial one is a SketchEllipticalArc");
        check(whole.IsSelfClosed() && !part.IsSelfClosed(),
              "which is exactly the sweep difference");

        // Everything but points and text is a SketchCurve in Fusion.
        check(IsSketchCurve(EntityType::SketchControlPointSpline)
                  && IsSketchCurve(EntityType::SketchConicCurve)
                  && IsSketchCurve(EntityType::SketchEllipticalArc),
              "the new types are all SketchCurves");
    }

    // ---- 2. a fitted spline goes THROUGH its points ----
    {
        const SketchEntity fitted = SketchEntity::MakeSpline(
            {gp_Pnt2d(0, 0), gp_Pnt2d(10, 8), gp_Pnt2d(20, 0)});
        checkPoint(fitted.StartPoint(), 0.0, 0.0, "a fitted spline starts at its first point");
        checkPoint(fitted.EndPoint(), 20.0, 0.0, "and ends at its last");

        // The middle point is ON the curve -- that is what "fitted" means.
        double first = 0.0, last = 0.0;
        check(SketchGeometry::ParamRange(fitted, first, last), "it has a parameter range");
        double param = 0.0;
        check(SketchGeometry::NearestParam(fitted, gp_Pnt2d(10, 8), param, true),
              "the middle fit point is findable on the curve");
        checkNear(SketchGeometry::PointAt(fitted, param).Distance(gp_Pnt2d(10, 8)), 0.0, 1.0e-6,
                  "and the curve really passes through it");
    }

    // ---- 3. a control point spline does NOT ----
    {
        const SketchEntity poles = SketchEntity::MakeControlPointSpline(
            {gp_Pnt2d(0, 0), gp_Pnt2d(10, 8), gp_Pnt2d(20, 0)});

        // A clamped B-spline starts and ends at its outer poles, which is
        // what keeps its endpoints snappable like every other curve's.
        checkPoint(poles.StartPoint(), 0.0, 0.0, "it still starts at the first pole");
        checkPoint(poles.EndPoint(), 20.0, 0.0, "and ends at the last");

        // But it is pulled TOWARDS the middle pole, not through it. For a
        // quadratic Bezier the apex of the curve is halfway between the
        // middle pole and the chord: (10, 4), not (10, 8).
        double param = 0.0;
        check(SketchGeometry::NearestParam(poles, gp_Pnt2d(10, 8), param, true),
              "the nearest point to the middle pole is findable");
        const double gap = SketchGeometry::PointAt(poles, param).Distance(gp_Pnt2d(10, 8));
        check(gap > 1.0,
              "and the curve does NOT pass through it -- that is the whole difference");
        checkPoint(SketchGeometry::PointAt(poles, 0.5), 10.0, 4.0,
                   "it reaches halfway to the middle pole");

        check(!poles.IsDegenerate(), "three poles is a real curve");
        check(SketchEntity::MakeControlPointSpline({gp_Pnt2d(0, 0)}).IsDegenerate(),
              "one pole is not");

        // Two poles is a straight line, and must still build rather than
        // being rejected for being too low a degree.
        const SketchEntity straight =
            SketchEntity::MakeControlPointSpline({gp_Pnt2d(0, 0), gp_Pnt2d(10, 0)});
        check(!straight.IsDegenerate(), "two poles is still a curve");
        check(!SketchGeometry::Curve2dOf(straight).IsNull(), "and it builds");
        checkPoint(SketchGeometry::PointAt(straight, 0.5), 5.0, 0.0, "as a straight line");
    }

    // ---- 4. a conic, and what rho actually does ----
    {
        // The defining property: the point at the middle of the curve sits
        // at (1 - rho) of the way from the chord midpoint to the apex.
        // P0 = (0,0), apex = (10,10), P2 = (20,0), chord midpoint = (10,0).
        //   rho = 0.5  ->  0.5*(10,0) + 0.5*(10,10)  = (10, 5)
        //   rho = 0.25 ->  0.75*(10,0) + 0.25*(10,10) = (10, 2.5)
        //   rho = 0.75 ->  0.25*(10,0) + 0.75*(10,10) = (10, 7.5)
        const gp_Pnt2d start(0, 0);
        const gp_Pnt2d apex(10, 10);
        const gp_Pnt2d end(20, 0);

        const SketchEntity parabola = SketchEntity::MakeConic(start, apex, end, 0.5);
        checkPoint(parabola.StartPoint(), 0.0, 0.0, "a conic starts at its first point");
        checkPoint(parabola.EndPoint(), 20.0, 0.0, "and ends at its third");
        checkPoint(SketchGeometry::PointAt(parabola, 0.5), 10.0, 5.0,
                   "rho 0.5 puts the shoulder halfway to the apex -- a parabola");

        const SketchEntity ellipse = SketchEntity::MakeConic(start, apex, end, 0.25);
        checkPoint(SketchGeometry::PointAt(ellipse, 0.5), 10.0, 2.5,
                   "rho 0.25 pulls it flat -- an ellipse arc");

        const SketchEntity hyperbola = SketchEntity::MakeConic(start, apex, end, 0.75);
        checkPoint(SketchGeometry::PointAt(hyperbola, 0.5), 10.0, 7.5,
                   "rho 0.75 pulls it towards the apex -- a hyperbola arc");

        // The apex is NOT on the curve, which is what distinguishes a
        // conic from an arc through three points.
        double param = 0.0;
        check(SketchGeometry::NearestParam(parabola, apex, param, true), "the apex has a nearest point");
        check(SketchGeometry::PointAt(parabola, param).Distance(apex) > 1.0,
              "but the curve does not pass through the apex");

        check(!parabola.IsSelfClosed(), "a conic never closes on itself");
        check(parabola.IsCurve(), "and it is a curve");

        // Out-of-range rho is clamped rather than producing a broken curve.
        const SketchEntity clamped = SketchEntity::MakeConic(start, apex, end, 5.0);
        check(clamped.rho < 1.0 && clamped.rho > 0.0, "rho is clamped into (0, 1)");
        check(!SketchGeometry::Curve2dOf(clamped).IsNull(), "and the curve still builds");

        // Degenerate: the two ends in the same place.
        check(SketchEntity::MakeConic(start, apex, start, 0.5).IsDegenerate(),
              "a conic from a point back to itself is degenerate");
    }

    // ---- 5. both new kinds build real 3D edges ----
    {
        SketchFeature sketch(SketchFeature::PlaneXY(), 0.0);
        const int poles = sketch.AddEntity(SketchEntity::MakeControlPointSpline(
            {gp_Pnt2d(0, 0), gp_Pnt2d(10, 8), gp_Pnt2d(20, 0)}));
        const int conic = sketch.AddEntity(SketchEntity::MakeConic(
            gp_Pnt2d(0, 0), gp_Pnt2d(-10, 10), gp_Pnt2d(-20, 0), 0.5));

        const SketchEntity* first = sketch.FindEntity(poles);
        const SketchEntity* second = sketch.FindEntity(conic);
        check(first != nullptr && second != nullptr, "both landed in the sketch");
        check(!sketch.BuildEdge(*first).IsNull(), "a control point spline builds a 3D edge");
        check(!sketch.BuildEdge(*second).IsNull(), "and so does a conic");
        check(!sketch.EdgeCompound().IsNull(), "and both reach the display compound");
    }

    // ---- 6. a conic can close a profile region ----
    {
        // A conic bulging up from (0,0) to (20,0), closed by a straight
        // line back along the bottom. The area is bounded by both.
        SketchFeature sketch(SketchFeature::PlaneXY(), 0.0);
        sketch.AddEntity(SketchEntity::MakeConic(gp_Pnt2d(0, 0), gp_Pnt2d(10, 10),
                                                 gp_Pnt2d(20, 0), 0.5));
        sketch.AddEntity(SketchEntity::MakeLine(gp_Pnt2d(20, 0), gp_Pnt2d(0, 0)));

        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();
        check(regions.size() == 1, "a conic and a line enclose one region");
        if (regions.size() == 1) {
            // A parabolic segment encloses two thirds of its bounding box:
            // the shoulder reaches y = 5, so 20 wide x 5 high x 2/3 = 66.67.
            checkNear(regions[0].area, 200.0 / 3.0, 0.5,
                      "and it is the area under a parabola, 2/3 of 20 x 5");
        }
    }

    // ---- 7. the new kinds survive the transforms ----
    {
        const SketchEntity conic = SketchEntity::MakeConic(
            gp_Pnt2d(0, 0), gp_Pnt2d(10, 10), gp_Pnt2d(20, 0), 0.3);

        const SketchEntity mirrored =
            SketchGeometry::Mirrored(conic, gp_Pnt2d(0, 0), gp_Pnt2d(0, 1));
        check(mirrored.kind == SketchEntity::Kind::Conic, "a mirrored conic is still a conic");
        checkNear(mirrored.rho, 0.3, 1.0e-12, "and keeps its rho -- a ratio survives reflection");
        checkPoint(mirrored.points[2], -20.0, 0.0, "its far end is mirrored across x = 0");

        const SketchEntity turned =
            SketchGeometry::Rotated(conic, gp_Pnt2d(0, 0), kTwoPi * 0.25);
        check(turned.kind == SketchEntity::Kind::Conic, "a rotated conic is still a conic");
        checkPoint(turned.points[2], 0.0, 20.0, "a quarter turn puts its end on the Y axis");

        const SketchEntity poles = SketchEntity::MakeControlPointSpline(
            {gp_Pnt2d(0, 0), gp_Pnt2d(10, 8), gp_Pnt2d(20, 0)});
        const SketchEntity movedPoles =
            SketchGeometry::Mirrored(poles, gp_Pnt2d(0, 0), gp_Pnt2d(0, 1));
        check(movedPoles.kind == SketchEntity::Kind::ControlPointSpline,
              "a mirrored control point spline keeps its kind");
        checkPoint(movedPoles.points[1], -10.0, 8.0, "and its poles are reflected");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL SKETCH TYPE TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " SKETCH TYPE TEST(S) FAILED" << std::endl;
    return 1;
}
