// End-to-end check of the real Document/Feature pipeline, no GUI:
// sketch a closed rectangle -> extrude it -> edit it -> undo -> suppress,
// verifying actual volumes at each step, then the same pipeline driven by
// a CHOSEN subset of a sketch's profiles rather than the whole sketch.
//
// Every expected volume is computed by hand in the comment beside it.
#include "core/Document.h"
#include "core/ProfileProvider.h"
#include "features/PrimitiveFeatures.h"
#include "features/ProfileFeatures.h"
#include "sketch/SketchFeature.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cmath>
#include <iostream>

using namespace lcad;

static int failures = 0;

static double VolumeOf(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return 0.0;
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(theShape, props);
    return props.Mass();
}

static void check(bool theOk, const std::string& theWhat)
{
    std::cout << (theOk ? "  PASS  " : "  FAIL  ") << theWhat << std::endl;
    if (!theOk) {
        ++failures;
    }
}

int main()
{
    // ---- 1. sketch a closed 40 x 30 rectangle on XY ----
    Document doc;
    auto sketch = std::make_shared<SketchFeature>(SketchFeature::PlaneXY(), 0.0);
    sketch->SetName("Sketch1");
    sketch->AddRectangle(gp_Pnt2d(0.0, 0.0), gp_Pnt2d(40.0, 30.0));
    doc.AddFeature(sketch);

    check(sketch->ProfileWires().size() == 1, "rectangle yields exactly one closed wire");
    check(!sketch->ProfileFaces().empty(), "rectangle yields a usable face");
    check(doc.Errors().empty(), "sketch computes without error");

    // ---- 2. extrude it 10mm -> expect 40*30*10 = 12000 ----
    auto extrude = std::make_shared<ExtrudeFeature>();
    extrude->SetName("Extrude1");
    extrude->SetSketchName("Sketch1");
    extrude->SetDistance(10.0);
    doc.AddFeature(extrude);

    const double volume = VolumeOf(doc.Shape());
    std::cout << "  extruded volume = " << volume << " (expect 12000)" << std::endl;
    check(std::fabs(volume - 12000.0) < 1.0, "extrude volume matches 40 x 30 x 10");

    // ---- 3. the whole point of a parametric model: edit and rebuild ----
    extrude->SetDistance(20.0);
    doc.Rebuild();
    std::cout << "  after distance=20 -> " << VolumeOf(doc.Shape()) << " (expect 24000)"
              << std::endl;
    check(std::fabs(VolumeOf(doc.Shape()) - 24000.0) < 1.0,
          "editing extrude distance rebuilds the solid");

    // ---- 4. undo ----
    const std::size_t before = doc.FeatureCount();
    doc.AddFeature(std::make_shared<BoxFeature>(10.0, 10.0, 10.0));
    check(doc.FeatureCount() == before + 1, "adding a box grows the timeline");
    doc.Undo();
    check(doc.FeatureCount() == before, "undo removes it again");

    // Undo restores the timeline from CLONES, so any handle taken before
    // the undo is now stale. Features reference each other by name for
    // exactly this reason -- re-resolve rather than reusing `extrude`.
    check(doc.FeatureAt(1).get() != extrude.get(),
          "undo replaced the timeline with clones (pointers go stale)");

    FeaturePtr liveExtrude;
    for (const FeaturePtr& feature : doc.Features()) {
        if (feature && feature->Name() == "Extrude1") {
            liveExtrude = feature;
        }
    }
    check(liveExtrude != nullptr, "extrude is still findable by name after undo");

    // ---- 5. suppression skips a feature without deleting it ----
    liveExtrude->SetSuppressed(true);
    doc.Rebuild();
    check(VolumeOf(doc.Shape()) < 1.0, "suppressing the extrude drops the solid");
    liveExtrude->SetSuppressed(false);
    doc.Rebuild();
    check(std::fabs(VolumeOf(doc.Shape()) - 24000.0) < 1.0, "unsuppressing restores it");

    // ---- 6. a bad reference fails loudly, not silently ----
    auto orphan = std::make_shared<ExtrudeFeature>();
    orphan->SetName("Orphan");
    orphan->SetSketchName("NoSuchSketch");
    orphan->SetDistance(5.0);
    doc.AddFeature(orphan);
    check(!orphan->LastError().empty(), "extrude on a missing sketch reports an error");
    std::cout << "    reported: " << orphan->LastError() << std::endl;
    check(std::fabs(VolumeOf(doc.Shape()) - 24000.0) < 1.0,
          "a failed feature leaves the upstream model intact");

    // ---- 7. extruding a CHOSEN subset of a sketch's profiles ----
    //
    // A 40 x 30 rectangle centred on the origin with a circle of r = 5 at
    // its centre. That is two minimal regions, the ring and the disc, so
    // three different 10mm extrudes are possible:
    //
    //   whole sketch (both regions) : 40 * 30 * 10                 = 12000
    //   the disc alone             : pi * 5^2 * 10 = 78.53982 * 10 = 785.398
    //   the ring alone             : (1200 - 78.53982) * 10        = 11214.602
    //
    // The two parts must add back up to the whole -- the regions are
    // disjoint, which is the property that makes them pickable at all.
    const double kPi = 3.14159265358979323846;
    const double kDiscVolume  = kPi * 25.0 * 10.0;           // 785.39816
    const double kRingVolume  = (1200.0 - kPi * 25.0) * 10.0; // 11214.60184

    Document pdoc;
    auto region_sketch = std::make_shared<SketchFeature>(SketchFeature::PlaneXY(), 0.0);
    region_sketch->SetName("Sketch1");
    region_sketch->AddRectangle(gp_Pnt2d(-20.0, -15.0), gp_Pnt2d(20.0, 15.0));
    const int circleId = region_sketch->AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));
    pdoc.AddFeature(region_sketch);

    const std::vector<ProfileRegion> regions = region_sketch->ProfileRegions();
    check(regions.size() == 2, "rect + inner circle is two selectable profiles");

    ProfileRef discRef;
    ProfileRef ringRef;
    for (const ProfileRegion& region : regions) {
        // The disc is the small one; no need to reproduce the area maths
        // here, profile_test already pins both numbers down.
        if (region.area < 100.0) {
            discRef = region.ref;
        } else {
            ringRef = region.ref;
        }
    }
    check(!discRef.IsNull() && !ringRef.IsNull(), "both profiles have usable references");

    auto regionExtrude = std::make_shared<ExtrudeFeature>();
    regionExtrude->SetName("Extrude1");
    regionExtrude->SetSketchName("Sketch1");
    regionExtrude->SetDistance(10.0);
    pdoc.AddFeature(regionExtrude);

    check(regionExtrude->Profiles().empty(), "a fresh extrude selects no profile");
    std::cout << "  no selection -> " << VolumeOf(pdoc.Shape()) << " (expect 12000)" << std::endl;
    check(std::fabs(VolumeOf(pdoc.Shape()) - 12000.0) < 0.01,
          "no profile selected extrudes the whole sketch (ring + disc)");

    regionExtrude->SetProfiles({discRef});
    pdoc.Rebuild();
    std::cout << "  disc only -> " << VolumeOf(pdoc.Shape()) << " (expect 785.398)" << std::endl;
    check(std::fabs(VolumeOf(pdoc.Shape()) - kDiscVolume) < 0.01,
          "selecting only the disc extrudes pi * 25 * 10");

    regionExtrude->SetProfiles({ringRef});
    pdoc.Rebuild();
    std::cout << "  ring only -> " << VolumeOf(pdoc.Shape()) << " (expect 11214.602)" << std::endl;
    check(std::fabs(VolumeOf(pdoc.Shape()) - kRingVolume) < 0.01,
          "selecting only the ring extrudes (1200 - pi * 25) * 10");

    // ---- 8. the profile choice survives undo ----
    //
    // Undo restores the timeline from Clone()s, so this is really a test
    // that ExtrudeFeature's clone carries the selection: without it the
    // feature quietly reverts to the whole sketch and the solid grows by
    // 15x behind the user's back.
    regionExtrude->SetProfiles({discRef});
    pdoc.Rebuild();
    pdoc.PushUndoSnapshot();
    regionExtrude->SetDistance(20.0);
    pdoc.Rebuild();
    check(std::fabs(VolumeOf(pdoc.Shape()) - 2.0 * kDiscVolume) < 0.02,
          "editing the distance of a single-profile extrude rebuilds it");

    pdoc.Undo();
    std::cout << "  after undo -> " << VolumeOf(pdoc.Shape()) << " (expect 785.398)" << std::endl;
    check(std::fabs(VolumeOf(pdoc.Shape()) - kDiscVolume) < 0.01,
          "undo keeps the chosen profile, not just the distance");

    // The pre-undo handle is stale now, exactly as in step 4.
    ProfileFeature* liveRegionExtrude = nullptr;
    SketchFeature*  liveSketch = nullptr;
    for (const FeaturePtr& feature : pdoc.Features()) {
        if (!feature) {
            continue;
        }
        if (feature->Name() == "Extrude1") {
            liveRegionExtrude = dynamic_cast<ProfileFeature*>(feature.get());
        }
        if (feature->Name() == "Sketch1") {
            liveSketch = dynamic_cast<SketchFeature*>(feature.get());
        }
    }
    check(liveRegionExtrude != nullptr && liveSketch != nullptr,
          "both features are findable by name after undo");
    check(liveRegionExtrude != nullptr && liveRegionExtrude->Profiles().size() == 1,
          "the clone carries exactly the one chosen profile");

    // ---- 9. the "Profiles" parameter round-trips through the panel ----
    std::string encoded;
    for (const Parameter& parameter : liveRegionExtrude->Parameters()) {
        if (parameter.name == "Profiles") {
            encoded = parameter.stringValue;
        }
    }
    check(!encoded.empty(), "the selection is reported as a 'Profiles' parameter");

    check(liveRegionExtrude->SetParameter(Parameter::MakeString("Profiles", encoded)),
          "the 'Profiles' string is accepted back");
    pdoc.Rebuild();
    check(std::fabs(VolumeOf(pdoc.Shape()) - kDiscVolume) < 0.01,
          "a round-tripped 'Profiles' string resolves to the same solid");

    check(!liveRegionExtrude->SetParameter(Parameter::MakeString("Profiles", "not a profile")),
          "text that decodes to nothing is refused, not read as 'whole sketch'");

    check(liveRegionExtrude->SetParameter(Parameter::MakeString("Profiles", "")),
          "an empty 'Profiles' string is accepted");
    pdoc.Rebuild();
    std::cout << "  cleared -> " << VolumeOf(pdoc.Shape()) << " (expect 12000)" << std::endl;
    check(std::fabs(VolumeOf(pdoc.Shape()) - 12000.0) < 0.01,
          "clearing 'Profiles' reverts to the whole sketch");

    // ---- 10. a profile whose boundary was deleted fails loudly ----
    //
    // Deleting the circle leaves the disc's seed point sitting inside the
    // plain rectangle. Resolving it there would turn a 785mm^3 extrude
    // into a 12000mm^3 one without a word, so this must report instead.
    liveRegionExtrude->SetProfiles({discRef});
    pdoc.Rebuild();
    check(pdoc.Errors().empty(), "the disc still resolves before the circle is deleted");

    liveSketch->RemoveEntity(circleId);
    pdoc.Rebuild();
    check(!pdoc.Errors().empty(), "deleting the bounding circle makes the extrude fail");
    if (!pdoc.Errors().empty()) {
        std::cout << "    reported: " << pdoc.Errors().front() << std::endl;
    }
    check(VolumeOf(pdoc.Shape()) < 1.0,
          "the lost profile produces nothing rather than the wrong solid");

    std::cout << (failures == 0 ? "\nALL PIPELINE TESTS PASSED\n" : "\nFAILURES\n");
    return failures == 0 ? 0 : 1;
}
