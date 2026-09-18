// Press/Pull: pick a face, move it. Out adds material, in removes it.
// Volumes are computed by hand in the comments, not read back from the
// implementation.
#include "core/Body.h"
#include "core/Document.h"
#include "core/GeometryRef.h"
#include "core/GeometrySelection.h"
#include "features/PressPullFeature.h"
#include "features/PrimitiveFeatures.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

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

static double VolumeOf(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return 0.0;
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(theShape, props);
    return props.Mass();
}

// A 10 x 20 x 30 box at the origin, so every face has a distinct area and
// a reference to one of them is never ambiguous.
static std::shared_ptr<BoxFeature> MakeBox(Document& theDocument)
{
    auto box = std::make_shared<BoxFeature>(10.0, 20.0, 30.0);
    box->SetName("Box1");
    box->SetOrigin(gp_Pnt(0.0, 0.0, 0.0));
    theDocument.AddFeature(box);
    return box;
}

// The reference to the face whose centre is highest -- the top of the box.
static GeometryRef TopFaceOf(const Document& theDocument)
{
    GeometryRef top;
    if (theDocument.Bodies().empty()) {
        return top;
    }
    for (const GeometryRef& ref :
         CollectGeometryRefs(*theDocument.Bodies().front(), EntityType::BRepFace)) {
        if (top.IsNull() || ref.point.Z() > top.point.Z()) {
            top = ref;
        }
    }
    return top;
}

int main()
{
    // ---- 1. pulling a face out adds material ----
    {
        Document doc;
        MakeBox(doc);
        checkNear(VolumeOf(doc.Shape()), 6000.0, 1.0e-6, "the box starts at 10 x 20 x 30");

        const GeometryRef top = TopFaceOf(doc);
        check(!top.IsNull(), "found the top face");
        checkNear(top.point.Z(), 30.0, 1.0e-9, "and it really is the top");
        checkNear(top.measure, 200.0, 1.0e-9, "its area is 10 x 20");

        auto pull = std::make_shared<PressPullFeature>(top, 5.0);
        pull->SetName("PressPull1");
        doc.AddFeature(pull);

        check(doc.Errors().empty(), "pulling the top face computes cleanly");
        // 10 * 20 * 35 = 7000
        checkNear(VolumeOf(doc.Shape()), 7000.0, 1.0e-6, "pulled out 5mm: 10 x 20 x 35");
        check(doc.Bodies().size() == 1, "and it is still one body, not two");
    }

    // ---- 2. pushing a face in removes material ----
    {
        Document doc;
        MakeBox(doc);
        auto push = std::make_shared<PressPullFeature>(TopFaceOf(doc), -5.0);
        push->SetName("PressPull1");
        doc.AddFeature(push);

        check(doc.Errors().empty(), "pushing the top face computes cleanly");
        // 10 * 20 * 25 = 5000
        checkNear(VolumeOf(doc.Shape()), 5000.0, 1.0e-6, "pushed in 5mm: 10 x 20 x 25");
    }

    // ---- 3. a side face moves sideways, not up ----
    //
    // This is what the face's ORIENTATION is for: a face's surface normal
    // points out of its own parameterisation, not out of the solid, and
    // ignoring that pushes half the faces of a box the wrong way.
    {
        Document doc;
        MakeBox(doc);

        GeometryRef side;
        for (const GeometryRef& ref :
             CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepFace)) {
            // The far face in X: centre at x = 10, area 20 x 30 = 600.
            if (std::fabs(ref.point.X() - 10.0) < 1.0e-9) {
                side = ref;
            }
        }
        check(!side.IsNull(), "found the far X face");
        checkNear(side.measure, 600.0, 1.0e-9, "its area is 20 x 30");

        auto pull = std::make_shared<PressPullFeature>(side, 4.0);
        pull->SetName("PressPull1");
        doc.AddFeature(pull);
        check(doc.Errors().empty(), "pulling a side face computes");
        // 14 * 20 * 30 = 8400 -- grown in X, so the normal was outward.
        checkNear(VolumeOf(doc.Shape()), 8400.0, 1.0e-6, "grew in X: 14 x 20 x 30");
    }

    // ---- 4. every face of the box pulls OUTWARD ----
    //
    // Six separate documents, because the interesting failure is that some
    // faces grow and others shrink depending on how the box was built.
    {
        for (int i = 0; i < 6; ++i) {
            Document doc;
            MakeBox(doc);
            const std::vector<GeometryRef> faces =
                CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepFace);
            if (faces.size() != 6) {
                check(false, "six faces to try");
                break;
            }
            auto pull = std::make_shared<PressPullFeature>(faces[static_cast<std::size_t>(i)], 2.0);
            pull->SetName("PressPull1");
            doc.AddFeature(pull);
            check(VolumeOf(doc.Shape()) > 6000.0,
                  "face " + std::to_string(i) + " pulled outward, adding material");
        }
    }

    // ---- 5. it is parametric and survives undo ----
    {
        Document doc;
        MakeBox(doc);
        auto pull = std::make_shared<PressPullFeature>(TopFaceOf(doc), 5.0);
        pull->SetName("PressPull1");
        doc.AddFeature(pull);
        checkNear(VolumeOf(doc.Shape()), 7000.0, 1.0e-6, "starts at 7000");

        Parameter distance = Parameter::MakeDouble("Distance", 20.0);
        check(pull->SetParameter(distance), "the distance is editable");
        doc.Rebuild();
        // 10 * 20 * 50 = 10000
        checkNear(VolumeOf(doc.Shape()), 10000.0, 1.0e-6, "edited to 20mm: 10 x 20 x 50");

        // The face reference has to survive Clone(), or undo silently
        // forgets which face this was built on.
        doc.PushUndoSnapshot();
        auto second = std::make_shared<BoxFeature>();
        second->SetName("Box2");
        second->SetOrigin(gp_Pnt(200.0, 0.0, 0.0));
        doc.AddFeature(second);
        doc.Undo();

        check(doc.FeatureCount() == 2, "undo dropped the second box");
        auto* clone = dynamic_cast<PressPullFeature*>(doc.FeatureAt(1).get());
        check(clone != nullptr, "the press/pull survived as a clone");
        if (clone != nullptr) {
            check(clone != pull.get(), "and really is a different object");
            check(!clone->Face().IsNull(), "the clone still knows its face");
            checkNear(clone->Distance(), 20.0, 1.0e-12, "and its distance");
        }
        checkNear(VolumeOf(doc.Shape()), 10000.0, 1.0e-6, "so the model is unchanged by undo");
    }

    // ---- 6. a lost face fails loudly ----
    {
        Document doc;
        auto box = MakeBox(doc);
        auto pull = std::make_shared<PressPullFeature>(TopFaceOf(doc), 5.0);
        pull->SetName("PressPull1");
        doc.AddFeature(pull);
        checkNear(VolumeOf(doc.Shape()), 7000.0, 1.0e-6, "built fine to start with");

        // Move the box far away: the face it named is not there any more.
        box->SetOrigin(gp_Pnt(500.0, 500.0, 500.0));
        doc.Rebuild();
        check(!doc.Errors().empty(), "the press/pull reports a broken reference");
        // 10 * 20 * 30 -- the box alone, with the press/pull contributing
        // nothing, rather than quietly moving to some other face.
        checkNear(VolumeOf(doc.Shape()), 6000.0, 1.0e-6,
                  "and the model falls back to the box, not a wrong solid");
    }

    // ---- 7. the things it refuses ----
    {
        Document doc;
        MakeBox(doc);
        const GeometryRef top = TopFaceOf(doc);

        auto zero = std::make_shared<PressPullFeature>(top, 0.0);
        zero->SetName("Zero");
        doc.AddFeature(zero);
        check(!doc.Errors().empty(), "a zero distance is refused");
        doc.RemoveFeature(doc.FeatureAt(1));

        auto noFace = std::make_shared<PressPullFeature>(GeometryRef(), 5.0);
        noFace->SetName("NoFace");
        doc.AddFeature(noFace);
        check(!doc.Errors().empty(), "no face is refused");
        doc.RemoveFeature(doc.FeatureAt(1));

        // An edge is not a face, and its measurements match no face.
        GeometryRef edge =
            CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepEdge).front();
        auto wrongKind = std::make_shared<PressPullFeature>(edge, 5.0);
        wrongKind->SetName("Edge");
        doc.AddFeature(wrongKind);
        check(!doc.Errors().empty(), "an edge reference is refused");
    }

    // ---- 8. press/pull with nothing to act on ----
    {
        Document doc;
        GeometryRef invented;
        invented.type = EntityType::BRepFace;
        invented.body = "Body1";
        invented.point = gp_Pnt(0, 0, 0);
        invented.measure = 100.0;

        auto orphan = std::make_shared<PressPullFeature>(invented, 5.0);
        orphan->SetName("PressPull1");
        doc.AddFeature(orphan);
        check(!doc.Errors().empty(), "press/pull on an empty document is refused");
        check(doc.Shape().IsNull(), "and makes nothing");
    }

    // ---- 9. the selection filter ----
    {
        GeometrySelection& selection = GeometrySelection::Instance();
        check(selection.IsFilterOn(EntityType::BRepFace), "faces are pickable by default");
        check(selection.IsFilterOn(EntityType::BRepEdge), "so are edges");
        check(!selection.IsFilterOn(EntityType::BRepVertex),
              "vertices are not, until asked for");

        const std::size_t before = selection.FilterGeneration();
        selection.SetFilter(EntityType::BRepVertex, true);
        check(selection.IsFilterOn(EntityType::BRepVertex), "turning vertices on works");
        check(selection.FilterGeneration() != before, "and the viewport is told to re-arm");

        selection.SetFilter(EntityType::BRepVertex, false);
        selection.SetFilter(EntityType::BRepEdge, false);
        check(selection.Filters().size() == 1, "down to one filter");
        selection.SetFilter(EntityType::BRepFace, false);
        check(selection.Filters().size() == 1,
              "the last filter cannot be turned off -- an inert viewport looks broken");

        selection.SetFilter(EntityType::BRepEdge, true);

        // SoleItem refuses to pick one when the user picked several.
        Document doc;
        MakeBox(doc);
        const std::vector<GeometryRef> faces =
            CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepFace);
        selection.Set({faces[0]});
        check(!selection.SoleItem(EntityType::BRepFace).IsNull(), "one face is a sole item");
        selection.Set({faces[0], faces[1]});
        check(selection.SoleItem(EntityType::BRepFace).IsNull(),
              "two faces are not -- the caller has to refuse rather than guess");
        check(selection.SoleItem(EntityType::BRepEdge).IsNull(), "and no edge was picked");
        selection.Clear();
        check(selection.IsEmpty(), "clearing empties it");

        // A sub-shape picked in the viewport is traced back to its body.
        GeometryRef traced;
        check(MakeGeometryRefIn(doc, doc.Bodies().front()->Faces().front(), traced),
              "a picked face is traced back to its body");
        check(traced.body == "Body1", "and names it");
        check(!MakeGeometryRefIn(doc, doc.Bodies().front()->Shape(), traced),
              "a whole body is not a face, edge or vertex");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL PRESS/PULL TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " PRESS/PULL TEST(S) FAILED" << std::endl;
    return 1;
}
