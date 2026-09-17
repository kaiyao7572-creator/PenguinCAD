// Sketch profile regions: the Fusion-style split of a sketch into the
// closed areas a user can point at, and the reference that survives an
// edit. No GUI, no display -- all of this is sketch-plane maths.
//
// Every expected area here is computed by hand in the comment beside it,
// deliberately not read back from the implementation.
#include "core/ProfileProvider.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchProfiles.h"

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

static SketchFeature MakeSketch()
{
    return SketchFeature(SketchFeature::PlaneXY(), 0.0);
}

// Area of the region containing a point, or 0 when nothing is there.
static double AreaAt(const std::vector<ProfileRegion>& theRegions, double theX, double theY)
{
    const int index = ProfileRegionAt(theRegions, gp_Pnt2d(theX, theY));
    return index < 0 ? 0.0 : theRegions[static_cast<std::size_t>(index)].area;
}

int main()
{
    const double kPi = 3.14159265358979323846;

    // ---- 1. a lone rectangle is one region ----
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(40.0, 30.0));
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();

        check(regions.size() == 1, "rectangle -> 1 region");
        if (regions.size() == 1) {
            checkNear(regions[0].area, 1200.0, 1.0e-6, "rectangle area is 40 x 30");
            check(regions[0].ref.boundary.size() == 4, "bounded by its four lines");
        }
    }

    // ---- 2. a rectangle with a circle inside is TWO regions ----
    //
    // This is the case the whole feature exists for: today it collapses
    // into one face with a hole, so the disc cannot be extruded on its own.
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(-20.0, -15.0), gp_Pnt2d(20.0, 15.0));
        sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();

        check(regions.size() == 2, "rect + inner circle -> 2 regions");
        if (regions.size() == 2) {
            // 40*30 - pi*25 = 1200 - 78.5398 = 1121.4602, largest first
            checkNear(regions[0].area, 1200.0 - kPi * 25.0, 1.0e-4, "ring area is rect minus disc");
            checkNear(regions[1].area, kPi * 25.0, 1.0e-4, "disc area is pi r^2");
        }

        // The point of regions: a click lands in exactly one of them.
        checkNear(AreaAt(regions, 0.0, 0.0), kPi * 25.0, 1.0e-4, "centre of the circle hits the disc");
        checkNear(AreaAt(regions, 15.0, 10.0), 1200.0 - kPi * 25.0, 1.0e-4, "a corner hits the ring");
        check(AreaAt(regions, 100.0, 100.0) == 0.0, "outside the sketch hits nothing");

        // ProfileFaces() now reports both regions rather than one holed face.
        check(sketch.ProfileFaces().size() == 2, "ProfileFaces reports both regions");
    }

    // ---- 3. two overlapping circles are THREE regions ----
    //
    // r = 10, centres 12 apart. Lens = 2 r^2 acos(d/2r) - (d/2) sqrt(4r^2 - d^2)
    //                                = 200*acos(0.6) - 6*16 = 89.4590
    // Each lune = pi r^2 - lens = 314.1593 - 89.4590 = 224.7002
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 10.0));
        sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(12.0, 0.0), 10.0));
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();

        const double lens = 200.0 * std::acos(0.6) - 96.0;
        check(regions.size() == 3, "two overlapping circles -> 3 regions");
        checkNear(AreaAt(regions, 6.0, 0.0), lens, 1.0e-3, "the overlap is the lens");
        checkNear(AreaAt(regions, -5.0, 0.0), kPi * 100.0 - lens, 1.0e-3, "left lune");
        checkNear(AreaAt(regions, 17.0, 0.0), kPi * 100.0 - lens, 1.0e-3, "right lune");
    }

    // ---- 4. a line drawn across a rectangle splits it ----
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(-20.0, -15.0), gp_Pnt2d(20.0, 15.0));
        sketch.AddEntity(SketchEntity::MakeLine(gp_Pnt2d(0.0, -15.0), gp_Pnt2d(0.0, 15.0)));
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();

        check(regions.size() == 2, "rect + divider -> 2 regions");
        checkNear(AreaAt(regions, -10.0, 0.0), 600.0, 1.0e-6, "left half is 20 x 30");
        checkNear(AreaAt(regions, 10.0, 0.0), 600.0, 1.0e-6, "right half is 20 x 30");
    }

    // ---- 5. a divider that overshoots still splits cleanly ----
    //
    // The stray tails hanging outside the rectangle must not become
    // regions of their own, and must not defeat the split either.
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(-20.0, -15.0), gp_Pnt2d(20.0, 15.0));
        sketch.AddEntity(SketchEntity::MakeLine(gp_Pnt2d(0.0, -25.0), gp_Pnt2d(0.0, 25.0)));
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();

        check(regions.size() == 2, "overshooting divider -> still 2 regions");
        checkNear(AreaAt(regions, -10.0, 0.0), 600.0, 1.0e-6, "left half unaffected by the tails");
    }

    // ---- 6. nested squares: ring and inner square ----
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(-20.0, -20.0), gp_Pnt2d(20.0, 20.0));
        sketch.AddRectangle(gp_Pnt2d(-10.0, -10.0), gp_Pnt2d(10.0, 10.0));
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();

        check(regions.size() == 2, "nested squares -> 2 regions");
        checkNear(AreaAt(regions, 15.0, 0.0), 1600.0 - 400.0, 1.0e-6, "ring is 1600 - 400");
        checkNear(AreaAt(regions, 0.0, 0.0), 400.0, 1.0e-6, "inner square is 400");
    }

    // ---- 7. two disjoint squares are two independent regions ----
    //
    // The old wire-based code called the smaller one a HOLE in the larger,
    // which is geometrically nonsense when they don't overlap.
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(10.0, 10.0));
        sketch.AddRectangle(gp_Pnt2d(50.0, 0.0), gp_Pnt2d(54.0, 4.0));
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();

        check(regions.size() == 2, "disjoint squares -> 2 regions");
        checkNear(AreaAt(regions, 5.0, 5.0), 100.0, 1.0e-6, "the big square is whole, not holed");
        checkNear(AreaAt(regions, 52.0, 2.0), 16.0, 1.0e-6, "the small square is its own region");
    }

    // ---- 8. a concave region: the centroid is OUTSIDE it ----
    //
    // A 40x40 square with a 25x20 notch cut in from the right. Area
    // 1600 - 500 = 1100, and the centroid at x = -3.409 lands in the notch,
    // so a naive centroid seed would sit outside the region it names.
    {
        SketchFeature sketch = MakeSketch();
        const gp_Pnt2d pts[] = {
            gp_Pnt2d(-20.0, -20.0), gp_Pnt2d(20.0, -20.0), gp_Pnt2d(20.0, -10.0),
            gp_Pnt2d(-5.0, -10.0),  gp_Pnt2d(-5.0, 10.0),  gp_Pnt2d(20.0, 10.0),
            gp_Pnt2d(20.0, 20.0),   gp_Pnt2d(-20.0, 20.0)};
        for (int i = 0; i < 8; ++i) {
            sketch.AddEntity(SketchEntity::MakeLine(pts[i], pts[(i + 1) % 8]));
        }
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();

        check(regions.size() == 1, "C-shape -> 1 region");
        if (regions.size() == 1) {
            checkNear(regions[0].area, 1100.0, 1.0e-6, "C-shape area is 1600 - 500");
            check(ProfileRegionAt(regions, regions[0].ref.seed) == 0,
                  "the seed point really is inside its own region");
            check(std::fabs(regions[0].ref.seed.X() + 3.409) > 0.5,
                  "the seed is NOT the centroid, which falls in the notch");
        }
    }

    // ---- 9. things that are not regions ----
    {
        SketchFeature open = MakeSketch();
        open.AddEntity(SketchEntity::MakeLine(gp_Pnt2d(-10.0, 0.0), gp_Pnt2d(10.0, 0.0)));
        open.AddEntity(SketchEntity::MakeLine(gp_Pnt2d(10.0, 0.0), gp_Pnt2d(10.0, 10.0)));
        check(open.ProfileRegions().empty(), "an open chain is not a region");

        SketchFeature empty = MakeSketch();
        check(empty.ProfileRegions().empty(), "an empty sketch has no regions");

        SketchFeature points = MakeSketch();
        points.AddEntity(SketchEntity::MakePoint(gp_Pnt2d(1.0, 1.0)));
        check(points.ProfileRegions().empty(), "bare sketch points are not regions");

        SketchFeature construction = MakeSketch();
        SketchEntity circle = SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0);
        circle.isConstruction = true;
        construction.AddEntity(circle);
        check(construction.ProfileRegions().empty(), "construction geometry encloses nothing");

        SketchFeature lone = MakeSketch();
        lone.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));
        check(lone.ProfileRegions().size() == 1, "a single circle is still one region");
    }

    // ---- 10. a reference round-trips through its string form ----
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(-20.0, -15.0), gp_Pnt2d(20.0, 15.0));
        sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();
        if (regions.size() == 2) {
            ProfileRef decoded;
            check(ProfileRef::Decode(regions[1].ref.Encode(), decoded), "a ref decodes");
            check(decoded.SameBoundary(regions[1].ref), "the boundary survives encoding");
            checkNear(decoded.seed.X(), regions[1].ref.seed.X(), 1.0e-9, "the seed x survives");
            checkNear(decoded.seed.Y(), regions[1].ref.seed.Y(), 1.0e-9, "the seed y survives");

            const std::string many = EncodeProfileRefs({regions[0].ref, regions[1].ref});
            check(DecodeProfileRefs(many).size() == 2, "a list of refs round-trips");
            check(DecodeProfileRefs("rubbish").empty(), "rubbish decodes to nothing");
        }
    }

    // ---- 11. a reference survives edits elsewhere in the sketch ----
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(-20.0, -15.0), gp_Pnt2d(20.0, 15.0));
        const int circleId = sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));

        std::vector<ProfileRegion> regions = sketch.ProfileRegions();
        ProfileRef discRef;
        for (const ProfileRegion& region : regions) {
            if (region.area < 100.0) {
                discRef = region.ref;
            }
        }
        check(!discRef.IsNull(), "found the disc to hold on to");

        TopoDS_Face face;
        check(sketch.FindProfile(discRef, face), "the disc resolves before any edit");

        // Draw an unrelated square far away: the disc is untouched.
        sketch.AddRectangle(gp_Pnt2d(100.0, 100.0), gp_Pnt2d(110.0, 110.0));
        check(sketch.FindProfile(discRef, face), "unrelated new geometry does not lose it");

        // Grow the circle: same bounding curve, different size, still the disc.
        SketchEntity bigger = SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 7.0);
        sketch.ReplaceEntity(circleId, bigger);
        check(sketch.FindProfile(discRef, face), "resizing the circle keeps the same profile");

        // Delete the circle: the disc is genuinely gone and must say so
        // rather than quietly resolving to the surrounding region.
        sketch.RemoveEntity(circleId);
        check(!sketch.FindProfile(discRef, face), "deleting the circle breaks the reference");
    }

    // ---- 12. identical boundaries are told apart by the seed ----
    //
    // All three regions of two overlapping circles are bounded by exactly
    // the same two entity ids, so the id set alone cannot name one.
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 10.0));
        sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(12.0, 0.0), 10.0));
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();

        int resolved = 0;
        for (const ProfileRegion& region : regions) {
            TopoDS_Face face;
            if (sketch.FindProfile(region.ref, face)) {
                ++resolved;
                check(ProfileRegionAt(regions, region.ref.seed) >= 0, "each seed lands in a region");
            }
        }
        check(resolved == 3, "all three same-boundary regions resolve individually");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL PROFILE TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " PROFILE TEST(S) FAILED" << std::endl;
    return 1;
}
