// Identity: a stored reference either names the thing the user picked or
// it fails out loud. Never a near miss, never a best guess.
//
// ARCHITECTURE.md states this rule twice -- once for ProfileRef under
// "Identity", once for GeometryRef -- and both times as prose. Prose does
// not stop a regression, so this suite is the rule written down as
// assertions: the exact rectangle-around-a-circle case the fallback was
// removed for, the seed tiebreak that the ids alone can never make, and
// the refusal to pick a winner when two candidates match equally well.
//
// Areas and distances here are computed by hand in the comment beside
// them, deliberately not read back from the implementation.
#include "core/Body.h"
#include "core/Entity.h"
#include "core/GeometryRef.h"
#include "core/ProfileProvider.h"
#include "core/ProfileSelection.h"
#include "sketch/SketchFeature.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

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

static double FaceArea(const TopoDS_Face& theFace)
{
    if (theFace.IsNull()) {
        return 0.0;
    }
    GProp_GProps props;
    BRepGProp::SurfaceProperties(theFace, props);
    return props.Mass();
}

// The region of theRegions whose area is closest to theArea -- how a test
// says "the ring" or "the disc" without depending on the order they come
// back in.
static ProfileRef RefWithArea(const std::vector<ProfileRegion>& theRegions, double theArea)
{
    ProfileRef best;
    double     bestGap = 1.0e9;
    for (const ProfileRegion& region : theRegions) {
        const double gap = std::fabs(region.area - theArea);
        if (gap < bestGap) {
            bestGap = gap;
            best    = region.ref;
        }
    }
    return best;
}

// How many entity ids two references have in common.
static std::size_t SharedIds(const ProfileRef& theLeft, const ProfileRef& theRight)
{
    std::size_t shared = 0;
    for (const int id : theLeft.boundary) {
        if (std::find(theRight.boundary.begin(), theRight.boundary.end(), id)
            != theRight.boundary.end()) {
            ++shared;
        }
    }
    return shared;
}

static TopoDS_Shape Box(double x, double y, double z, const gp_Pnt& at)
{
    return BRepPrimAPI_MakeBox(at, x, y, z).Shape();
}

static TopoDS_Shape CompoundOf(const std::vector<TopoDS_Shape>& theShapes)
{
    TopoDS_Compound compound;
    BRep_Builder    builder;
    builder.MakeCompound(compound);
    for (const TopoDS_Shape& shape : theShapes) {
        builder.Add(compound, shape);
    }
    return compound;
}

int main()
{
    const double kPi = 3.14159265358979323846;

    // ---- 1. the RING of a rectangle-around-a-circle ----
    //
    // The case in ARCHITECTURE.md, and the reason FindProfile has no
    // fallback. A 40 x 30 rectangle with an r = 5 circle inside it is two
    // regions: the ring (1200 - 78.5398 = 1121.4602) and the disc.
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(-20.0, -15.0), gp_Pnt2d(20.0, 15.0));
        const int circleId = sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));

        const std::vector<ProfileRegion> before = sketch.ProfileRegions();
        check(before.size() == 2, "rectangle around a circle is two regions");

        const ProfileRef ringRef = RefWithArea(before, 1200.0 - kPi * 25.0);
        const ProfileRef discRef = RefWithArea(before, kPi * 25.0);
        check(ringRef.boundary.size() == 5, "the ring is bounded by four lines and the circle");
        check(discRef.boundary.size() == 1, "the disc is bounded by the circle alone");

        TopoDS_Face face;
        check(sketch.FindProfile(ringRef, face), "the ring resolves while the circle is there");
        checkNear(FaceArea(face), 1200.0 - kPi * 25.0, 1.0e-4,
                  "and resolves to the ring, not to the whole rectangle");

        // Delete the circle. The ring is gone, and saying so is the whole
        // point of this test.
        sketch.RemoveEntity(circleId);
        check(!sketch.FindProfile(ringRef, face),
              "deleting the circle FAILS the ring reference rather than resolving it");
        check(!sketch.FindProfile(discRef, face), "and fails the disc reference too");

        // Now prove the temptation was real: the rectangle left behind
        // shares four of the ring's five curves AND swallows its seed, so
        // a "shares a curve and contains the seed" fallback would have
        // resolved the ring to it and said nothing.
        const std::vector<ProfileRegion> after = sketch.ProfileRegions();
        check(after.size() == 1, "one region survives -- the plain rectangle");
        if (after.size() == 1) {
            checkNear(after[0].area, 1200.0, 1.0e-6, "and it is the full 40 x 30");
            check(SharedIds(ringRef, after[0].ref) == 4,
                  "the rectangle shares four of the ring's five bounding curves");
            check(ProfileRegionAt(after, ringRef.seed) == 0,
                  "and it contains the ring's seed point");
            check(SharedIds(discRef, after[0].ref) == 0,
                  "the disc shares nothing with it, which is why the disc is the easy case");

            // What the removed fallback would have cost, in numbers: an
            // extrude of 1121.46 mm2 quietly becoming one of 1200 mm2.
            checkNear(after[0].area - (1200.0 - kPi * 25.0), kPi * 25.0, 1.0e-4,
                      "a silent fallback would have added the disc's area to the extrude");
        }

        // Redrawing the circle does not bring the reference back: ids are
        // never reused, and a new curve is not the curve that was picked.
        sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));
        check(sketch.ProfileRegions().size() == 2, "the ring and disc exist again as regions");
        check(!sketch.FindProfile(ringRef, face),
              "but the old ring reference still fails -- the new circle is a different curve");
    }

    // ---- 2. the seed is the only thing telling three regions apart ----
    //
    // Two r = 10 circles 12 apart: lens, left lune, right lune. All three
    // are bounded by exactly the same two entity ids, so the boundary
    // alone cannot name one of them.
    //
    // lens = 2 r^2 acos(d/2r) - (d/2) sqrt(4r^2 - d^2)
    //      = 200 acos(0.6) - 6 * 16 = 89.4590
    // lune = pi r^2 - lens = 314.1593 - 89.4590 = 224.7002
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 10.0));
        sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(12.0, 0.0), 10.0));

        const std::vector<ProfileRegion> regions = sketch.ProfileRegions();
        check(regions.size() == 3, "two overlapping circles are three regions");

        const double lens = 200.0 * std::acos(0.6) - 96.0;
        const double lune = kPi * 100.0 - lens;

        // Same ids for all three -- stated as an assertion because the
        // tiebreak below is only meaningful while it holds.
        bool sameBoundary = true;
        for (const ProfileRegion& region : regions) {
            sameBoundary = sameBoundary && region.ref.SameBoundary(regions.front().ref);
        }
        check(sameBoundary, "all three carry the same two bounding ids");

        // Each ref resolves to ITS region, not merely to some region.
        for (const ProfileRegion& region : regions) {
            TopoDS_Face face;
            const bool  found = sketch.FindProfile(region.ref, face);
            check(found, "a region resolves");
            if (found) {
                checkNear(FaceArea(face), region.area, 1.0e-3,
                          "and resolves to the region with its own area");
            }
        }

        // Swap one ref's seed for another region's and it resolves to that
        // other region: the seed really is what is doing the work.
        const ProfileRef lensRef = RefWithArea(regions, lens);
        ProfileRef       moved   = lensRef;
        moved.seed               = gp_Pnt2d(-5.0, 0.0);  // inside the left lune

        TopoDS_Face face;
        check(sketch.FindProfile(lensRef, face), "the lens reference resolves");
        checkNear(FaceArea(face), lens, 1.0e-3, "to the lens");
        check(sketch.FindProfile(moved, face), "the same ids with a lune's seed resolve");
        checkNear(FaceArea(face), lune, 1.0e-3, "to the LUNE -- the seed chose, not the ids");

        // A seed inside none of them names nothing, rather than falling
        // back to the first candidate that shares the boundary.
        ProfileRef stray = lensRef;
        stray.seed       = gp_Pnt2d(500.0, 500.0);
        check(!sketch.FindProfile(stray, face),
              "the right ids with a seed outside every region resolve to nothing");
    }

    // ---- 3. a reference that names nothing is refused ----
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(10.0, 10.0));

        TopoDS_Face face;
        check(!sketch.FindProfile(ProfileRef(), face), "a null reference resolves to nothing");

        ProfileRef invented;
        invented.boundary = {9001, 9002};
        invented.seed     = gp_Pnt2d(5.0, 5.0);
        check(!sketch.FindProfile(invented, face),
              "ids that never existed are refused, even with a seed inside a real region");

        SketchFeature empty = MakeSketch();
        check(!empty.FindProfile(RefWithArea(sketch.ProfileRegions(), 100.0), face),
              "a sketch with no regions resolves nothing");
    }

    // ---- 4. the selection obeys the same rule as the reference ----
    //
    // ProfileSelection::Prune re-points what the user picked at the
    // regions as they are now. It must drop the ring rather than let the
    // rectangle inherit the highlight -- the same refusal FindProfile
    // makes, in the place the user would actually see it.
    {
        SketchFeature sketch = MakeSketch();
        sketch.AddRectangle(gp_Pnt2d(-20.0, -15.0), gp_Pnt2d(20.0, 15.0));
        const int circleId = sketch.AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));

        const ProfileRef ringRef = RefWithArea(sketch.ProfileRegions(), 1200.0 - kPi * 25.0);

        ProfileSelection& selection = ProfileSelection::Instance();
        selection.SetSketchName("Sketch1");
        selection.Clear();
        selection.Toggle(ringRef, false);
        check(selection.Count() == 1, "the ring is picked");
        check(selection.Contains(ringRef), "and the selection knows it holds that region");

        selection.Prune(sketch.ProfileRegions());
        check(selection.Count() == 1, "pruning against an unchanged sketch keeps it");

        sketch.RemoveEntity(circleId);
        selection.Prune(sketch.ProfileRegions());
        check(selection.IsEmpty(),
              "once the circle is gone the ring is dropped, not re-pointed at the rectangle");

        selection.Clear();
    }

    // ---- 5. GeometryRef refuses when two candidates match equally well ----
    //
    // A 10 x 10 x 0.1 plate. Its top and bottom faces have the same area
    // (100) and their centres are 0.1 apart, well inside the position
    // tolerance -- which is exactly the shape of a reference that does not
    // single anything out.
    {
        Body plate(Box(10.0, 10.0, 0.1, gp_Pnt(0, 0, 0)), "Body1");
        const std::vector<GeometryRef> faces = CollectGeometryRefs(plate, EntityType::BRepFace);
        check(faces.size() == 6, "a plate still has six faces");

        GeometryRef top;
        for (const GeometryRef& ref : faces) {
            // The two big faces are the 10 x 10 ones; the walls are 10 x 0.1.
            if (std::fabs(ref.measure - 100.0) < 1.0e-6 && ref.point.Z() > 0.05) {
                top = ref;
            }
        }
        check(!top.IsNull(), "found the top face");

        TopoDS_Shape found;
        check(ResolveGeometryRef(plate, top, found), "a reference ON the top face resolves");
        if (!found.IsNull() && found.ShapeType() == TopAbs_FACE) {
            GProp_GProps props;
            BRepGProp::SurfaceProperties(found, props);
            checkNear(props.CentreOfMass().Z(), 0.1, 1.0e-9,
                      "and resolves to the top face, not the bottom one 0.1 below it");
        }

        // Halfway between them: both match the area, both are in range,
        // and neither is meaningfully closer.
        GeometryRef between = top;
        between.point       = gp_Pnt(5.0, 5.0, 0.05);
        check(!ResolveGeometryRef(plate, between, found),
              "a reference between two equally good faces is REFUSED, not given to one");

        // The other way a reference can name nothing: a size no face has.
        GeometryRef wrongSize = top;
        wrongSize.measure     = 37.0;
        check(!ResolveGeometryRef(plate, wrongSize, found),
              "a reference whose size matches no face resolves to nothing");
    }

    // ---- 6. two coincident bodies leave every reference ambiguous ----
    //
    // Pasting a body on top of itself duplicates every face exactly. There
    // is no right answer to "which one did you mean", so there must be no
    // answer at all.
    {
        const TopoDS_Shape single = Box(10.0, 20.0, 30.0, gp_Pnt(0, 0, 0));
        Body               body(single, "Body1");
        const std::vector<GeometryRef> faces = CollectGeometryRefs(body, EntityType::BRepFace);
        check(faces.size() == 6, "six faces to reference");

        TopoDS_Shape found;
        check(ResolveGeometryRefInShape(single, faces[0], found),
              "the reference resolves against the single body");

        const TopoDS_Shape doubled =
            CompoundOf({single, Box(10.0, 20.0, 30.0, gp_Pnt(0, 0, 0))});
        int resolvedAnyway = 0;
        for (const GeometryRef& ref : faces) {
            if (ResolveGeometryRefInShape(doubled, ref, found)) {
                ++resolvedAnyway;
            }
        }
        check(resolvedAnyway == 0,
              "against two coincident copies NO face reference resolves -- all six refuse");
    }

    // ---- 7. a vertex is named by its position alone ----
    //
    // Mass properties give every vertex a zero mass at the origin, so
    // position is the only signal there is. If it were not read off the
    // vertex itself, all eight corners of a box would look identical and
    // every one of them would be refused as ambiguous.
    {
        Body body(Box(10.0, 20.0, 30.0, gp_Pnt(0, 0, 0)), "Body1");
        const std::vector<GeometryRef> corners = CollectGeometryRefs(body, EntityType::BRepVertex);
        check(corners.size() == 8, "eight corner references");

        std::vector<TopoDS_Shape> resolved;
        for (const GeometryRef& ref : corners) {
            checkNear(ref.measure, 0.0, 1.0e-12, "a vertex reference carries no size");
            TopoDS_Shape found;
            if (!ResolveGeometryRef(body, ref, found) || found.ShapeType() != TopAbs_VERTEX) {
                continue;
            }
            checkNear(BRep_Tool::Pnt(TopoDS::Vertex(found)).Distance(ref.point), 0.0, 1.0e-9,
                      "and resolves to the corner standing at its point");
            resolved.push_back(found);
        }
        check(resolved.size() == 8, "all eight resolve");

        // Eight distinct corners, not the same one eight times.
        std::size_t distinct = 0;
        for (std::size_t i = 0; i < resolved.size(); ++i) {
            bool seenBefore = false;
            for (std::size_t j = 0; j < i; ++j) {
                seenBefore = seenBefore || resolved[i].IsSame(resolved[j]);
            }
            if (!seenBefore) {
                ++distinct;
            }
        }
        check(distinct == 8, "to eight DIFFERENT corners");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL IDENTITY TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " IDENTITY TEST(S) FAILED" << std::endl;
    return 1;
}
