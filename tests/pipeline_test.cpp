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

    // Fusion makes ONE body from an extrude of adjoining profiles. A
    // compound of the ring prism and the disc prism listed as two bodies
    // sharing a cylindrical face, with a seam through the top and bottom.
    check(pdoc.Bodies().size() == 1, "ring + disc extrude as ONE body, as in Fusion");
    if (!pdoc.Bodies().empty()) {
        const std::size_t faces = pdoc.Bodies().front()->Faces().size();
        std::cout << "  faces on it: " << faces << " (expect 6)" << std::endl;
        check(faces == 6, "a plain 40 x 30 x 10 block: no seam where the regions met");
    }

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

    // Profiles that do NOT touch stay separate bodies, as they do in Fusion.
    {
        Document apart;
        auto twoCircles = std::make_shared<SketchFeature>(SketchFeature::PlaneXY(), 0.0);
        twoCircles->SetName("Sketch1");
        twoCircles->AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));
        twoCircles->AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(30.0, 0.0), 5.0));
        apart.AddFeature(twoCircles);
        auto both = std::make_shared<ExtrudeFeature>();
        both->SetName("Extrude1");
        both->SetSketchName("Sketch1");
        both->SetDistance(10.0);
        apart.AddFeature(both);
        check(apart.Bodies().size() == 2, "two separate circles extrude as two bodies");
        check(std::fabs(VolumeOf(apart.Shape()) - 2.0 * kDiscVolume) < 0.01,
              "of pi * 25 * 10 each");
    }

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

    // ---- 9b. a MULTI-ref string is all-or-nothing ----
    //
    // Picking both regions by hand must come back to the same 12000 the
    // whole sketch gives -- ring + disc = 11214.602 + 785.398 = 12000 --
    // because the two are disjoint and cover it exactly.
    //
    // The interesting case is the corrupt one. DecodeProfileRefs drops
    // segments it cannot parse, so a row hand-edited into "<good>;oops"
    // would decode to ONE ref: the feature would silently narrow from
    // 12000 to the ring's 11214.602 with nothing reported. Narrowing a
    // model behind the user's back is the same sin as widening it, so the
    // whole string has to be refused.
    liveRegionExtrude->SetProfiles({ringRef, discRef});
    pdoc.Rebuild();
    check(std::fabs(VolumeOf(pdoc.Shape()) - 12000.0) < 0.01,
          "selecting both regions by hand equals the whole sketch");

    std::string encodedBoth;
    for (const Parameter& parameter : liveRegionExtrude->Parameters()) {
        if (parameter.name == "Profiles") {
            encodedBoth = parameter.stringValue;
        }
    }
    check(encodedBoth.find(';') != std::string::npos,
          "two chosen profiles encode as two ';'-separated refs");
    check(liveRegionExtrude->SetParameter(Parameter::MakeString("Profiles", encodedBoth)),
          "the two-ref 'Profiles' string is accepted back");
    pdoc.Rebuild();
    check(std::fabs(VolumeOf(pdoc.Shape()) - 12000.0) < 0.01,
          "a round-tripped two-ref string resolves to the same solid");

    const std::string mangled = encodedBoth.substr(0, encodedBoth.find(';') + 1) + "oops";
    check(!liveRegionExtrude->SetParameter(Parameter::MakeString("Profiles", mangled)),
          "a string with one unparseable ref is refused whole");
    check(liveRegionExtrude->Profiles().size() == 2,
          "the refused string left the selection untouched");
    pdoc.Rebuild();
    std::cout << "  after refused edit -> " << VolumeOf(pdoc.Shape()) << " (expect 12000)"
              << std::endl;
    check(std::fabs(VolumeOf(pdoc.Shape()) - 12000.0) < 0.01,
          "a refused 'Profiles' edit does not shrink the solid");

    // The same region twice is one region: sweeping it twice would stack
    // two coincident prisms into the compound and report 2 * 785.398.
    liveRegionExtrude->SetProfiles({discRef, discRef});
    check(liveRegionExtrude->Profiles().size() == 1, "a repeated profile is stored once");
    pdoc.Rebuild();
    std::cout << "  disc picked twice -> " << VolumeOf(pdoc.Shape()) << " (expect 785.398)"
              << std::endl;
    check(std::fabs(VolumeOf(pdoc.Shape()) - kDiscVolume) < 0.01,
          "picking the disc twice still extrudes one disc");

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

    // ---- 11. the RING is the dangerous half of the same deletion ----
    //
    // The disc above is the easy case: its only bounding curve was the
    // circle, so once the circle goes nothing shares a curve with it and
    // the reference is plainly dead. The ring is the trap. It is bounded
    // by {4 rectangle edges + the circle}, so after the delete the plain
    // rectangle still shares four of those five curves AND still holds the
    // ring's seed point -- a reference that looks alive from every angle
    // but now names an area fifteen times bigger:
    //
    //   what the user picked : (1200 - pi * 25) * 10 = 11214.602
    //   what it would become : 40 * 30 * 10          = 12000
    //
    // A 7% change to the model with no error is exactly the outcome the
    // whole ProfileRef design exists to prevent, so it must fail like the
    // disc does. Needs its own document: pdoc's circle is gone by now.
    Document rdoc;
    auto ring_sketch = std::make_shared<SketchFeature>(SketchFeature::PlaneXY(), 0.0);
    ring_sketch->SetName("Sketch1");
    ring_sketch->AddRectangle(gp_Pnt2d(-20.0, -15.0), gp_Pnt2d(20.0, 15.0));
    const int ringCircleId =
        ring_sketch->AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 5.0));
    rdoc.AddFeature(ring_sketch);

    ProfileRef ringOnly;
    for (const ProfileRegion& region : ring_sketch->ProfileRegions()) {
        if (region.area >= 100.0) {
            ringOnly = region.ref;
        }
    }
    check(!ringOnly.IsNull(), "the ring of a fresh sketch has a usable reference");

    auto ringExtrude = std::make_shared<ExtrudeFeature>();
    ringExtrude->SetName("Extrude1");
    ringExtrude->SetSketchName("Sketch1");
    ringExtrude->SetDistance(10.0);
    ringExtrude->SetProfiles({ringOnly});
    rdoc.AddFeature(ringExtrude);
    check(std::fabs(VolumeOf(rdoc.Shape()) - kRingVolume) < 0.01,
          "the ring still resolves before the circle is deleted");
    check(rdoc.Errors().empty(), "and does so without an error");

    ring_sketch->RemoveEntity(ringCircleId);
    rdoc.Rebuild();
    std::cout << "  ring after deleting its circle -> " << VolumeOf(rdoc.Shape())
              << " (expect 0, NOT 12000)" << std::endl;
    check(!rdoc.Errors().empty(), "deleting the circle makes the ring extrude fail");
    if (!rdoc.Errors().empty()) {
        std::cout << "    reported: " << rdoc.Errors().front() << std::endl;
    }
    check(VolumeOf(rdoc.Shape()) < 1.0,
          "the ring does not silently become the whole rectangle");

    // ---- 12. re-pointing at another sketch drops the selection ----
    //
    // Entity ids are per-sketch, so a ref carried across would resolve
    // against whatever happens to hold the same numbers over there. Worth
    // pinning: this also fires when a sketch is RENAMED and the feature
    // re-pointed at the very same geometry, because a feature only sees
    // that the name changed. Losing the pick there is deliberate, not an
    // oversight -- re-picking costs a click, resolving onto a stranger's
    // ids costs a wrong model.
    auto pointed = std::make_shared<ExtrudeFeature>();
    pointed->SetSketchName("Sketch1");
    pointed->SetProfiles({ringOnly});
    pointed->SetSketchName("Sketch1");
    check(pointed->Profiles().size() == 1, "re-setting the SAME sketch name keeps the pick");
    pointed->SetSketchName("Sketch2");
    check(pointed->Profiles().empty(), "pointing at another sketch clears the pick");

    pointed->SetSketchName("Sketch1");
    pointed->SetProfiles({ringOnly});
    check(pointed->SetParameter(Parameter::MakeString("Sketch", "Sketch2")),
          "the 'Sketch' parameter accepts another sketch");
    check(pointed->Profiles().empty(), "and clears the pick the same way");

    std::cout << (failures == 0 ? "\nALL PIPELINE TESTS PASSED\n" : "\nFAILURES\n");
    return failures == 0 ? 0 : 1;
}
