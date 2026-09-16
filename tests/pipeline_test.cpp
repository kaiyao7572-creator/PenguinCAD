// End-to-end check of the real Document/Feature pipeline, no GUI:
// sketch a closed rectangle -> extrude it -> edit it -> undo -> suppress,
// verifying actual volumes at each step.
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

    std::cout << (failures == 0 ? "\nALL PIPELINE TESTS PASSED\n" : "\nFAILURES\n");
    return failures == 0 ? 0 : 1;
}
