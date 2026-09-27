// Picking a finished sketch's regions from the model view: which region a
// ray from the cursor lands on, and whether a body in the way takes the
// click instead. No window: the rays are built by hand here, the way
// V3d_View::ConvertWithProj builds them in the app (a point on the near
// clipping plane and a direction into the scene).
//
// Every expected depth and area is worked out in the comment beside it,
// not read back from the implementation.
#include "core/ProfileProvider.h"
#include "core/ProfileSelection.h"
#include "sketch/ProfilePicking.h"
#include "sketch/SketchFeature.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <iostream>
#include <limits>
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

static const double kPi = 3.14159265358979323846;
static const double kNone = std::numeric_limits<double>::infinity();

// Sized the way the app sizes it: about one screen pixel at an ordinary
// zoom, far above float noise and far below any gap a user could see.
static const double kTolerance = 0.05;

// Straight down onto XY from above, through (x, y).
static gp_Lin FromAbove(double theX, double theY, double theHeight = 100.0)
{
    return gp_Lin(gp_Pnt(theX, theY, theHeight), gp_Dir(0.0, 0.0, -1.0));
}

// Straight up at XY from underneath.
static gp_Lin FromBelow(double theX, double theY, double theDepth = 100.0)
{
    return gp_Lin(gp_Pnt(theX, theY, -theDepth), gp_Dir(0.0, 0.0, 1.0));
}

// Rectangle -20..20 x -15..15 around a circle of radius 5: the ring and the
// disc, the sketch the whole feature is demonstrated with.
static SketchFeature RingAndDisc(double theOffset = 0.0)
{
    SketchFeature sketch(SketchFeature::PlaneXY(), theOffset);
    sketch.AddRectangle(gp_Pnt2d(-20.0, -15.0), gp_Pnt2d(20.0, 15.0));
    sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));
    return sketch;
}

static double AreaOf(const ProfileHit& theHit, const std::vector<ProfilePickTarget>& theTargets)
{
    if (!theHit.IsHit()) {
        return 0.0;
    }
    const std::vector<ProfileRegion>& regions =
        *theTargets[static_cast<std::size_t>(theHit.target)].regions;
    return regions[static_cast<std::size_t>(theHit.region)].area;
}

int main()
{
    // 40*30 - pi*5^2 = 1200 - 78.5398 = 1121.4602
    const double kRing = 1200.0 - kPi * 25.0;
    const double kDisc = kPi * 25.0;

    // ---- 1. where a ray meets a sketch plane ----
    {
        double   depth = 0.0;
        gp_Pnt2d point;

        const bool down = RayMeetsPlane(FromAbove(5.0, 7.0), SketchFeature::PlaneXY(), depth, point);
        check(down, "a ray straight down meets XY");
        checkNear(depth, 100.0, 1.0e-9, "  100 along it, from z = 100");
        checkNear(point.X(), 5.0, 1.0e-9, "  at sketch x = 5");
        checkNear(point.Y(), 7.0, 1.0e-9, "  at sketch y = 7");

        const gp_Lin along(gp_Pnt(0.0, 0.0, 10.0), gp_Dir(1.0, 0.0, 0.0));
        check(!RayMeetsPlane(along, SketchFeature::PlaneXY(), depth, point),
              "a ray parallel to the plane never meets it");

        const gp_Lin away(gp_Pnt(0.0, 0.0, 10.0), gp_Dir(0.0, 0.0, 1.0));
        check(!RayMeetsPlane(away, SketchFeature::PlaneXY(), depth, point),
              "a plane behind where the ray starts is not on screen");

        // An offset sketch lives on its offset plane: z = 20, so 80 down.
        const SketchFeature raised(SketchFeature::PlaneXY(), 20.0);
        RayMeetsPlane(FromAbove(5.0, 7.0), raised.Position(), depth, point);
        checkNear(depth, 80.0, 1.0e-9, "an offset of 20 meets the ray 80 down");

        // 45 degrees down from (0,0,100) along +X reaches z = 0 at x = 100,
        // having travelled 100 * sqrt(2) = 141.4214.
        const gp_Lin oblique(gp_Pnt(0.0, 0.0, 100.0), gp_Dir(1.0, 0.0, -1.0));
        RayMeetsPlane(oblique, SketchFeature::PlaneXY(), depth, point);
        checkNear(depth, 100.0 * std::sqrt(2.0), 1.0e-9, "an oblique ray travels 100 * sqrt 2");
        checkNear(point.X(), 100.0, 1.0e-9, "  and lands at x = 100");

        // The 2D point has to be in the SAME coordinates the regions are
        // classified in, on a plane whose axes are not the world's.
        const SketchFeature side(SketchFeature::PlaneYZ(), 0.0);
        const gp_Lin across(gp_Pnt(100.0, 3.0, 4.0), gp_Dir(-1.0, 0.0, 0.0));
        RayMeetsPlane(across, side.Position(), depth, point);
        const gp_Pnt2d expected = side.To2d(gp_Pnt(0.0, 3.0, 4.0));
        check(point.Distance(expected) < 1.0e-9, "on YZ the point matches SketchFeature::To2d");
    }

    // ---- 2. no bodies: the region under the cursor, or nothing ----
    {
        const SketchFeature sketch = RingAndDisc();
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();
        const std::vector<ProfilePickTarget> targets{{sketch.Position(), &regions}};

        const ProfileHit disc = ProfileUnderRay(FromAbove(0.0, 0.0), targets, kNone, kTolerance);
        check(disc.IsHit(), "over the circle: a hit");
        checkNear(AreaOf(disc, targets), kDisc, 1.0e-4, "  the disc, not the rectangle around it");

        const ProfileHit ring = ProfileUnderRay(FromAbove(15.0, 0.0), targets, kNone, kTolerance);
        checkNear(AreaOf(ring, targets), kRing, 1.0e-4, "between circle and rectangle: the ring");

        const ProfileHit outside = ProfileUnderRay(FromAbove(30.0, 0.0), targets, kNone, kTolerance);
        check(!outside.IsHit(), "outside the rectangle: nothing");

        const std::vector<ProfilePickTarget> empty;
        check(!ProfileUnderRay(FromAbove(0.0, 0.0), empty, kNone, kTolerance).IsHit(),
              "no finished sketches: nothing");
    }

    // ---- 3. a body in the way ----
    //
    // A 10 x 10 x 20 box standing on XY over the circle: x,y in -5..5,
    // z in 0..20.
    {
        const TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(-5.0, -5.0, 0.0), 10.0, 10.0, 20.0).Shape();
        BodyRayCaster bodies;
        bodies.SetBodies({box});

        checkNear(bodies.NearestDepth(FromAbove(0.0, 0.0)), 80.0, 1.0e-6,
                  "from z = 100 the box's top (z = 20) is 80 away");
        check(std::isinf(bodies.NearestDepth(FromAbove(15.0, 0.0))),
              "beside the box the ray hits nothing");

        // Sketch on XY, under the box.
        {
            const SketchFeature sketch = RingAndDisc();
            const std::vector<ProfileRegion> regions = sketch.ProfileRegions();
            const std::vector<ProfilePickTarget> targets{{sketch.Position(), &regions}};

            const gp_Lin overBox = FromAbove(0.0, 0.0);
            check(!ProfileUnderRay(overBox, targets, bodies.NearestDepth(overBox), kTolerance).IsHit(),
                  "box in FRONT of the sketch (80 < 100): the body takes the click");

            const gp_Lin beside = FromAbove(15.0, 0.0);
            const ProfileHit ring =
                ProfileUnderRay(beside, targets, bodies.NearestDepth(beside), kTolerance);
            checkNear(AreaOf(ring, targets), kRing, 1.0e-4,
                      "beside the box the ring is still pickable");

            // From below, the box's bottom face lies ON the sketch plane:
            // both 100 away. The region must win inside the region.
            const gp_Lin underBox = FromBelow(0.0, 0.0);
            checkNear(bodies.NearestDepth(underBox), 100.0, 1.0e-6,
                      "from below the box's bottom face is 100 away, same as the plane");
            const ProfileHit coplanar =
                ProfileUnderRay(underBox, targets, bodies.NearestDepth(underBox), kTolerance);
            checkNear(AreaOf(coplanar, targets), kDisc, 1.0e-4,
                      "coplanar face: the region wins (the disc)");
        }

        // Sketch drawn ON the box's top face -- "sketch on a face, click
        // the region, E".
        {
            const SketchFeature sketch = RingAndDisc(20.0);
            const std::vector<ProfileRegion> regions = sketch.ProfileRegions();
            const std::vector<ProfilePickTarget> targets{{sketch.Position(), &regions}};

            const gp_Lin overBox = FromAbove(0.0, 0.0);
            const ProfileHit onFace =
                ProfileUnderRay(overBox, targets, bodies.NearestDepth(overBox), kTolerance);
            check(onFace.IsHit(), "sketch on the top face: the region under the cursor wins");
            checkNear(onFace.depth, 80.0, 1.0e-9, "  met 80 down, on the face");
            checkNear(AreaOf(onFace, targets), kDisc, 1.0e-4, "  and it is the disc");

            // From underneath, the whole box stands between the cursor and
            // the sketch: bottom at 100, sketch at 120.
            const gp_Lin underBox = FromBelow(0.0, 0.0);
            check(!ProfileUnderRay(underBox, targets, bodies.NearestDepth(underBox), kTolerance)
                       .IsHit(),
                  "same sketch seen from below: the box hides it");
        }

        // Sketch floating above the box: the body is BEHIND the plane.
        {
            const SketchFeature sketch = RingAndDisc(50.0);
            const std::vector<ProfileRegion> regions = sketch.ProfileRegions();
            const std::vector<ProfilePickTarget> targets{{sketch.Position(), &regions}};

            const gp_Lin overBox = FromAbove(0.0, 0.0);
            const ProfileHit above =
                ProfileUnderRay(overBox, targets, bodies.NearestDepth(overBox), kTolerance);
            checkNear(AreaOf(above, targets), kDisc, 1.0e-4,
                      "body behind the plane (80 > 50): the region wins");
        }
    }

    // ---- 4. how close counts as "on the plane" ----
    {
        const SketchFeature sketch = RingAndDisc();
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();
        const std::vector<ProfilePickTarget> targets{{sketch.Position(), &regions}};
        const gp_Lin ray = FromAbove(0.0, 0.0);   // plane 100 away

        check(ProfileUnderRay(ray, targets, 100.0 - 1.0e-6, kTolerance).IsHit(),
              "a face 1e-6 in front is float noise: coplanar, region wins");
        check(ProfileUnderRay(ray, targets, 100.0 - 0.9 * kTolerance, kTolerance).IsHit(),
              "just inside the tolerance: still coplanar");
        check(!ProfileUnderRay(ray, targets, 100.0 - 1.1 * kTolerance, kTolerance).IsHit(),
              "just outside it: the body is in front");
        check(!ProfileUnderRay(ray, targets, 99.5, kTolerance).IsHit(),
              "half a millimetre in front: the body is in front");
        check(ProfileUnderRay(ray, targets, 100.5, kTolerance).IsHit(),
              "half a millimetre behind: the region wins");
    }

    // ---- 5. several finished sketches under one cursor ----
    {
        // Two 20 x 20 squares, one on XY and one lifted to z = 10.
        SketchFeature low(SketchFeature::PlaneXY(), 0.0);
        low.AddRectangle(gp_Pnt2d(-10.0, -10.0), gp_Pnt2d(10.0, 10.0));
        SketchFeature high(SketchFeature::PlaneXY(), 10.0);
        high.AddRectangle(gp_Pnt2d(-10.0, -10.0), gp_Pnt2d(10.0, 10.0));
        const std::vector<ProfileRegion> lowRegions = low.ProfileRegions();
        const std::vector<ProfileRegion> highRegions = high.ProfileRegions();
        const std::vector<ProfilePickTarget> targets{{low.Position(), &lowRegions},
                                                     {high.Position(), &highRegions}};

        const ProfileHit fromAbove = ProfileUnderRay(FromAbove(0.0, 0.0), targets, kNone, kTolerance);
        check(fromAbove.target == 1, "from above, the higher sketch is nearer and wins");
        checkNear(fromAbove.depth, 90.0, 1.0e-9, "  90 down");

        const ProfileHit fromBelow = ProfileUnderRay(FromBelow(0.0, 0.0), targets, kNone, kTolerance);
        check(fromBelow.target == 0, "from below, the one on XY wins");

        // Where the nearer sketch has no region, the one behind it shows
        // through: an empty stretch of plane hides nothing.
        SketchFeature small(SketchFeature::PlaneXY(), 10.0);
        small.AddRectangle(gp_Pnt2d(-2.0, -2.0), gp_Pnt2d(2.0, 2.0));
        const std::vector<ProfileRegion> smallRegions = small.ProfileRegions();
        const std::vector<ProfilePickTarget> seeThrough{{low.Position(), &lowRegions},
                                                        {small.Position(), &smallRegions}};
        check(ProfileUnderRay(FromAbove(6.0, 6.0), seeThrough, kNone, kTolerance).target == 0,
              "outside the nearer sketch's regions, the farther sketch is picked");

        // Coplanar sketches: the smaller region, whichever comes first.
        SketchFeature big(SketchFeature::PlaneXY(), 0.0);
        big.AddRectangle(gp_Pnt2d(-30.0, -30.0), gp_Pnt2d(30.0, 30.0));
        const std::vector<ProfileRegion> bigRegions = big.ProfileRegions();
        const std::vector<ProfilePickTarget> bigFirst{{big.Position(), &bigRegions},
                                                      {low.Position(), &lowRegions}};
        const std::vector<ProfilePickTarget> bigLast{{low.Position(), &lowRegions},
                                                     {big.Position(), &bigRegions}};
        check(ProfileUnderRay(FromAbove(0.0, 0.0), bigFirst, kNone, kTolerance).target == 1,
              "coplanar sketches: the smaller region wins (listed second)");
        check(ProfileUnderRay(FromAbove(0.0, 0.0), bigLast, kNone, kTolerance).target == 0,
              "coplanar sketches: the smaller region wins (listed first)");
        // 60 x 60 = 3600 where only the big one reaches.
        check(ProfileUnderRay(FromAbove(20.0, 20.0), bigFirst, kNone, kTolerance).target == 0,
              "  and the big one where the small one is absent");
    }

    // ---- 6. the body caster reloads when the bodies change ----
    {
        BodyRayCaster bodies;
        check(std::isinf(bodies.NearestDepth(FromAbove(0.0, 0.0))), "no bodies: nothing in the way");

        const TopoDS_Shape low = BRepPrimAPI_MakeBox(gp_Pnt(-5.0, -5.0, 0.0), 10.0, 10.0, 20.0).Shape();
        const TopoDS_Shape tall = BRepPrimAPI_MakeBox(gp_Pnt(-5.0, -5.0, 0.0), 10.0, 10.0, 60.0).Shape();
        bodies.SetBodies({low});
        checkNear(bodies.NearestDepth(FromAbove(0.0, 0.0)), 80.0, 1.0e-6, "20 tall: 80 away");
        bodies.SetBodies({tall});
        checkNear(bodies.NearestDepth(FromAbove(0.0, 0.0)), 40.0, 1.0e-6,
                  "replaced by one 60 tall: 40 away, not the stale 80");
        bodies.SetBodies({low, tall});
        checkNear(bodies.NearestDepth(FromAbove(0.0, 0.0)), 40.0, 1.0e-6,
                  "both: the nearer surface counts");
        bodies.SetBodies({});
        check(std::isinf(bodies.NearestDepth(FromAbove(0.0, 0.0))),
              "a hidden body stops occluding once it is left out");
    }

    // ---- 7. the status-bar line ----
    {
        check(DescribeProfiles({}).empty(), "nothing picked: no line");
        check(DescribeProfiles({1200.0}) == "Profile   Area 1200 mm²",
              "one profile: 'Profile   Area 1200 mm2', worded like a face");
        // pi * 25 = 78.5398 -> 78.54
        check(DescribeProfiles({kDisc}) == "Profile   Area 78.54 mm²", "the disc: 78.54 mm2");
        check(DescribeProfiles({kRing, kDisc}) == "2 profiles   Total area 1200 mm²",
              "ring + disc: 2 profiles totalling the whole rectangle");
    }

    // ---- 8. the selection tells its display when it changes ----
    //
    // Extrude clears the picks it consumed after the rebuild has already
    // redrawn them; without the notification the highlight outlived them.
    {
        ProfileSelection& selection = ProfileSelection::Instance();
        selection.SetSketchName("SketchA");
        selection.Clear();

        int calls = 0;
        selection.SetChangeHandler([&calls]() { ++calls; });

        const SketchFeature sketch = RingAndDisc();
        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();
        check(regions.size() == 2, "ring and disc are two regions");
        if (regions.size() == 2) {
            selection.Toggle(regions[1].ref, false);
            check(calls == 1, "a pick notifies");
            selection.Toggle(regions[0].ref, true);
            check(calls == 2 && selection.Count() == 2, "an additive pick notifies");
            selection.Toggle(regions[0].ref, true);
            check(calls == 3 && selection.Count() == 1, "taking one back out notifies");
            selection.Prune(regions);
            check(calls == 3, "a prune that loses nothing is silent");
            selection.Prune({});
            check(calls == 4 && selection.IsEmpty(), "a prune that drops a pick notifies");

            selection.Toggle(regions[1].ref, false);
            selection.Clear();
            check(calls == 6, "clearing picks notifies");
            selection.Clear();
            check(calls == 6, "clearing nothing is silent");
            selection.Toggle(ProfileRef(), false);
            check(calls == 6, "a click on empty space with nothing picked is silent");

            selection.Toggle(regions[1].ref, false);
            selection.SetSketchName("SketchB");
            check(calls == 8 && selection.IsEmpty(), "switching sketch drops the picks, and says so");
        }
        selection.SetChangeHandler(nullptr);
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL PROFILE PICKING TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " PROFILE PICKING TEST(S) FAILED" << std::endl;
    return 1;
}
