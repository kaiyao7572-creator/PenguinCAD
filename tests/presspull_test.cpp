// Press/Pull: pick a face, move it. Out adds material, in removes it.
// Volumes are computed by hand in the comments, not read back from the
// implementation.
#include "core/Body.h"
#include "core/Document.h"
#include "core/GeometryRef.h"
#include "core/GeometrySelection.h"
#include "features/PressPullFeature.h"
#include "features/FeatureUtils.h"
#include "features/PrimitiveFeatures.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
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

static void checkText(const std::string& theValue, const std::string& theExpected,
                      const std::string& theWhat)
{
    const bool ok = theValue == theExpected;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << theWhat << "  (got \"" << theValue
              << "\", expect \"" << theExpected << "\")" << std::endl;
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

// Every face of the document's first body, as references.
static std::vector<GeometryRef> FacesOf(const Document& theDocument)
{
    if (theDocument.Bodies().empty()) {
        return {};
    }
    return CollectGeometryRefs(*theDocument.Bodies().front(), EntityType::BRepFace);
}

// The one face of that area. Returns a null ref when nothing matches or
// when two faces match, so a test can never quietly act on the wrong one.
static GeometryRef FaceWithArea(const Document& theDocument, double theArea)
{
    GeometryRef found;
    int matches = 0;
    for (const GeometryRef& ref : FacesOf(theDocument)) {
        // Relative: a cone's lateral area comes back from a numeric
        // integration, so an absolute micron on a 971mm^2 face is luck.
        if (std::fabs(ref.measure - theArea) <= 1.0e-7 * std::max(1.0, theArea)) {
            found = ref;
            ++matches;
        }
    }
    return matches == 1 ? found : GeometryRef();
}

// A 40 x 40 x 10 plate with a radius-5 hole drilled through its centre.
// 40*40*10 - pi*5^2*10 = 16000 - 785.3982 = 15214.6018
static void MakePlateWithHole(Document& theDocument)
{
    auto plate = std::make_shared<BoxFeature>(40.0, 40.0, 10.0);
    plate->SetName("Plate");
    plate->SetOrigin(gp_Pnt(0.0, 0.0, 0.0));
    theDocument.AddFeature(plate);

    auto drill = std::make_shared<CylinderFeature>(5.0, 20.0);
    drill->SetName("Drill");
    drill->SetOrigin(gp_Pnt(20.0, 20.0, -5.0));
    drill->SetOperation(BooleanOp::Cut);
    theDocument.AddFeature(drill);
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

    // ---- 10. a cylindrical face changes its RADIUS ----
    //
    // Fusion press/pulls a curved wall by moving it along its own normal,
    // which for a cylinder means in or out from the axis. Sweeping it
    // would build a tube; this has to grow the cylinder itself.
    {
        Document doc;
        auto cylinder = std::make_shared<CylinderFeature>(10.0, 20.0);
        cylinder->SetName("Cylinder1");
        doc.AddFeature(cylinder);
        // pi * 10^2 * 20 = 6283.1853
        checkNear(VolumeOf(doc.Shape()), 6283.1853071796, 1.0e-6, "a r=10 h=20 cylinder");

        // The wall: 2 * pi * 10 * 20 = 1256.6371
        const GeometryRef wall = FaceWithArea(doc, 1256.6370614359);
        check(!wall.IsNull(), "found the cylindrical wall, and only one of it");

        auto grow = std::make_shared<PressPullFeature>(wall, 2.0);
        grow->SetName("PressPull1");
        doc.AddFeature(grow);
        check(doc.Errors().empty(), "pulling a cylindrical wall computes cleanly");
        // r = 12: pi * 12^2 * 20 = 9047.7868
        checkNear(VolumeOf(doc.Shape()), 9047.7868423386, 1.0e-5,
                  "pulled out 2mm: the radius went 10 -> 12");
    }
    {
        Document doc;
        auto cylinder = std::make_shared<CylinderFeature>(10.0, 20.0);
        cylinder->SetName("Cylinder1");
        doc.AddFeature(cylinder);
        auto shrink =
            std::make_shared<PressPullFeature>(FaceWithArea(doc, 1256.6370614359), -2.0);
        shrink->SetName("PressPull1");
        doc.AddFeature(shrink);
        check(doc.Errors().empty(), "pushing a cylindrical wall computes cleanly");
        // r = 8: pi * 8^2 * 20 = 4021.2386
        checkNear(VolumeOf(doc.Shape()), 4021.2385965949, 1.0e-5,
                  "pushed in 2mm: the radius went 10 -> 8");
    }
    {
        // Past the axis the kernel does NOT fail: it builds a valid solid
        // with the radius mirrored -- r = 10 pushed 12 comes back as r = 2,
        // pi * 2^2 * 20 = 251.3274. That is a wrong model nobody notices,
        // so the feature refuses it up front.
        Document doc;
        auto cylinder = std::make_shared<CylinderFeature>(10.0, 20.0);
        cylinder->SetName("Cylinder1");
        doc.AddFeature(cylinder);
        auto through =
            std::make_shared<PressPullFeature>(FaceWithArea(doc, 1256.6370614359), -12.0);
        through->SetName("PressPull1");
        doc.AddFeature(through);
        check(!doc.Errors().empty(), "pushing a wall past its own axis is refused");
        checkNear(VolumeOf(doc.Shape()), 6283.1853071796, 1.0e-6,
                  "and the cylinder is left alone, not silently mirrored to r=2");
    }

    // ---- 11. the sign convention on a hole ----
    //
    // The rule is one rule: a face moves the way it LOOKS. A hole's wall
    // looks at its own axis -- out of the metal, into the void -- so a
    // positive distance closes the hole and adds material, exactly as a
    // positive distance on a boss's wall fattens it. Getting this backwards
    // would be invisible in the box tests and wrong in every real part.
    {
        Document doc;
        MakePlateWithHole(doc);
        checkNear(VolumeOf(doc.Shape()), 15214.6018366026, 1.0e-5,
                  "40 x 40 x 10 less a r=5 hole");
        check(doc.Bodies().size() == 1, "one body with a hole in it");

        // The hole's wall: 2 * pi * 5 * 10 = 314.1593
        const GeometryRef wall = FaceWithArea(doc, 314.1592653589793);
        check(!wall.IsNull(), "found the hole's wall, and only one of it");
        checkNear(wall.point.Z(), 5.0, 1.0e-9, "its centre is halfway up the plate");

        auto close = std::make_shared<PressPullFeature>(wall, 2.0);
        close->SetName("PressPull1");
        doc.AddFeature(close);
        check(doc.Errors().empty(), "pressing the hole's wall computes cleanly");
        // r = 3: 16000 - pi * 3^2 * 10 = 16000 - 282.7433 = 15717.2567
        checkNear(VolumeOf(doc.Shape()), 15717.2566611769, 1.0e-5,
                  "+2 shrank the hole to r=3, so the SOLID grew");
    }
    {
        Document doc;
        MakePlateWithHole(doc);
        auto open =
            std::make_shared<PressPullFeature>(FaceWithArea(doc, 314.1592653589793), -2.0);
        open->SetName("PressPull1");
        doc.AddFeature(open);
        check(doc.Errors().empty(), "pulling the hole's wall computes cleanly");
        // r = 7: 16000 - pi * 7^2 * 10 = 16000 - 1539.3804 = 14460.6196
        checkNear(VolumeOf(doc.Shape()), 14460.6195997410, 1.0e-5,
                  "-2 opened the hole to r=7, so the SOLID shrank");
    }
    {
        // r=5 closed by 5 is a hole of no radius at all.
        Document doc;
        MakePlateWithHole(doc);
        auto vanish =
            std::make_shared<PressPullFeature>(FaceWithArea(doc, 314.1592653589793), 5.0);
        vanish->SetName("PressPull1");
        doc.AddFeature(vanish);
        check(!doc.Errors().empty(), "closing the hole to nothing is refused");
        checkNear(VolumeOf(doc.Shape()), 15214.6018366026, 1.0e-5,
                  "and the plate is untouched");
    }

    // ---- 12. several faces move together ----
    {
        Document doc;
        MakeBox(doc);

        GeometryRef top, bottom;
        for (const GeometryRef& ref : FacesOf(doc)) {
            if (std::fabs(ref.measure - 200.0) > 1.0e-9) {
                continue;  // the 10 x 20 pair, not the walls
            }
            if (ref.point.Z() > 15.0) {
                top = ref;
            } else {
                bottom = ref;
            }
        }
        check(!top.IsNull() && !bottom.IsNull(), "found the two 10 x 20 faces");

        auto both = std::make_shared<PressPullFeature>(
            std::vector<GeometryRef>{top, bottom}, 5.0);
        both->SetName("PressPull1");
        doc.AddFeature(both);
        check(doc.Errors().empty(), "two opposite faces compute cleanly");
        check(both->Faces().size() == 2, "and the feature really holds two");
        // Both grew outward along their own normals: 10 * 20 * 40 = 8000
        checkNear(VolumeOf(doc.Shape()), 8000.0, 1.0e-6,
                  "both moved out 5mm: 10 x 20 x 40");
        check(doc.Bodies().size() == 1, "still one body");
    }
    {
        // Four walls at once: the 20x30 pair out 2 and the 10x30 pair out 3
        // would need two features, but all four out 2 is one.
        // (10+4) * (20+4) * 30 = 14 * 24 * 30 = 10080
        Document doc;
        MakeBox(doc);
        std::vector<GeometryRef> walls;
        for (const GeometryRef& ref : FacesOf(doc)) {
            if (std::fabs(ref.measure - 200.0) > 1.0e-9) {
                walls.push_back(ref);
            }
        }
        check(walls.size() == 4, "four side walls");
        auto out = std::make_shared<PressPullFeature>(walls, 2.0);
        out->SetName("PressPull1");
        doc.AddFeature(out);
        check(doc.Errors().empty(), "four walls at once computes cleanly");
        checkNear(VolumeOf(doc.Shape()), 10080.0, 1.0e-6, "14 x 24 x 30");
    }

    // ---- 13. the neighbouring walls are EXTENDED, not stepped ----
    //
    // A cone's top face has a slanted neighbour, which is what makes it the
    // test that tells the two algorithms apart. Sweeping the disc and
    // fusing the prism would leave a 5mm cylindrical collar on top:
    // 3665.1914 + pi*5^2*5 = 4057.8905. Extending the cone wall instead
    // narrows it as it climbs, which is what Fusion does.
    {
        Document doc;
        auto cone = std::make_shared<ConeFeature>(10.0, 5.0, 20.0);
        cone->SetName("Cone1");
        doc.AddFeature(cone);
        // pi * 20/3 * (10^2 + 10*5 + 5^2) = pi * 20/3 * 175 = 3665.1914
        checkNear(VolumeOf(doc.Shape()), 3665.1914291881, 1.0e-5,
                  "a cone r 10 -> 5 over 20");

        // The small disc on top: pi * 5^2 = 78.5398
        const GeometryRef top = FaceWithArea(doc, 78.5398163397);
        check(!top.IsNull(), "found the cone's top disc");

        auto raise = std::make_shared<PressPullFeature>(top, 5.0);
        raise->SetName("PressPull1");
        doc.AddFeature(raise);
        check(doc.Errors().empty(), "raising the cone's top computes cleanly");
        // The wall loses 0.25 of radius per unit height, so at z = 25 it is
        // 10 - 25*0.25 = 3.75 across:
        // pi * 25/3 * (10^2 + 10*3.75 + 3.75^2) = 3967.8970
        checkNear(VolumeOf(doc.Shape()), 3967.8969713309, 1.0e-4,
                  "the cone wall grew with it (a sweep would give 4057.89)");
    }
    {
        // And the slanted wall itself offsets PERPENDICULAR to its own
        // surface, not radially: 2mm along the normal opens the radius by
        // 2 / cos(atan(5/20)) = 2 * sqrt(425)/20 = 2.0615528.
        Document doc;
        auto cone = std::make_shared<ConeFeature>(10.0, 5.0, 20.0);
        cone->SetName("Cone1");
        doc.AddFeature(cone);

        // The lateral surface: pi * (10 + 5) * sqrt(20^2 + 5^2) = 971.4839
        const GeometryRef wall = FaceWithArea(doc, 971.4838757561);
        check(!wall.IsNull(), "found the cone's slanted wall");

        auto fatten = std::make_shared<PressPullFeature>(wall, 2.0);
        fatten->SetName("PressPull1");
        doc.AddFeature(fatten);
        check(doc.Errors().empty(), "offsetting the slanted wall computes cleanly");
        // r1 = 12.0615528, r2 = 7.0615528, same height 20:
        // pi * 20/3 * (r1^2 + r1*r2 + r2^2) = 5875.1946
        checkNear(VolumeOf(doc.Shape()), 5875.1945562555, 1.0e-4,
                  "perpendicular, not radial: a radial 2mm would give 5875.19 only by luck");
    }

    // ---- 14. what a list of faces refuses ----
    {
        Document doc;
        MakeBox(doc);
        const GeometryRef top = TopFaceOf(doc);

        // One good face and one that names nothing. Building from the
        // survivor would change the model silently, which is the failure
        // this whole reference scheme exists to prevent.
        GeometryRef invented;
        invented.type = EntityType::BRepFace;
        invented.body = "Body1";
        invented.point = gp_Pnt(0.0, 0.0, 500.0);
        invented.measure = 12345.0;

        auto partial =
            std::make_shared<PressPullFeature>(std::vector<GeometryRef>{top, invented}, 5.0);
        partial->SetName("Partial");
        doc.AddFeature(partial);
        check(!doc.Errors().empty(), "one unresolvable face fails the whole feature");
        checkNear(VolumeOf(doc.Shape()), 6000.0, 1.0e-6,
                  "and the good face was NOT moved on its own");
        doc.RemoveFeature(doc.FeatureAt(1));

        auto twice =
            std::make_shared<PressPullFeature>(std::vector<GeometryRef>{top, top}, 5.0);
        twice->SetName("Twice");
        doc.AddFeature(twice);
        check(!doc.Errors().empty(), "the same face listed twice is refused");
        doc.RemoveFeature(doc.FeatureAt(1));

        auto none = std::make_shared<PressPullFeature>(std::vector<GeometryRef>{}, 5.0);
        none->SetName("None");
        doc.AddFeature(none);
        check(!doc.Errors().empty(), "an empty list is refused");
    }

    // ---- 15. the face list is a parameter, and survives a clone ----
    {
        Document doc;
        MakeBox(doc);
        GeometryRef top, bottom;
        for (const GeometryRef& ref : FacesOf(doc)) {
            if (std::fabs(ref.measure - 200.0) > 1.0e-9) {
                continue;
            }
            (ref.point.Z() > 15.0 ? top : bottom) = ref;
        }

        auto both = std::make_shared<PressPullFeature>(
            std::vector<GeometryRef>{top, bottom}, 5.0);
        both->SetName("PressPull1");
        doc.AddFeature(both);
        checkNear(VolumeOf(doc.Shape()), 8000.0, 1.0e-6, "two faces out 5mm: 8000");

        std::string encoded;
        for (const Parameter& parameter : both->Parameters()) {
            if (parameter.name == "Faces") {
                encoded = parameter.stringValue;
            }
        }
        check(!encoded.empty(), "the picks are exposed as a 'Faces' row");
        check(encoded.find(';') != std::string::npos, "with both of them in it");

        PressPullFeature rebuilt;
        check(rebuilt.SetParameter(Parameter::MakeString("Faces", encoded)),
              "and the row reads back");
        check(rebuilt.Faces().size() == 2, "as two faces");
        check(!rebuilt.SetParameter(Parameter::MakeString("Faces", "")),
              "an unparseable row is refused rather than clearing the picks");
        check(rebuilt.Faces().size() == 2, "so the picks are still there");

        // A single pick still reads back through Face(), which is what
        // every existing caller uses.
        PressPullFeature single(top, 3.0);
        check(single.Faces().size() == 1, "one face makes a one-entry list");
        check(!single.Face().IsNull(), "and Face() still answers");
        checkNear(single.Face().measure, 200.0, 1.0e-9, "with the right face");

        PressPullFeature empty;
        check(empty.Face().IsNull(), "an unset feature has a null Face()");
        check(empty.Faces().empty(), "and an empty list");
    }

    // ---- 16. a shape the offset builder cannot rebuild ----
    //
    // Fusing two boxes leaves a seam edge splitting the top into two
    // coplanar faces, and the offset builder returns a null shape on that
    // -- measured, not guessed. The feature falls back to sweeping the
    // face and fusing the prism, which is the pre-multi-face behaviour: a
    // worse answer where walls meet at an angle, but the right one here,
    // where every neighbour is square to the face that moves.
    {
        Document doc;
        auto base = std::make_shared<BoxFeature>(20.0, 10.0, 10.0);
        base->SetName("Base");
        doc.AddFeature(base);
        auto arm = std::make_shared<BoxFeature>(10.0, 10.0, 10.0);
        arm->SetName("Arm");
        arm->SetOrigin(gp_Pnt(0.0, 10.0, 0.0));
        arm->SetOperation(BooleanOp::Join);
        doc.AddFeature(arm);
        // 20*10*10 + 10*10*10 = 3000
        checkNear(VolumeOf(doc.Shape()), 3000.0, 1.0e-6, "an L of 2000 + 1000");

        std::vector<GeometryRef> tops;
        for (const GeometryRef& ref : FacesOf(doc)) {
            if (ref.point.Z() > 9.9) {
                tops.push_back(ref);
            }
        }
        check(tops.size() == 2, "the fuse left the top split in two");

        auto both = std::make_shared<PressPullFeature>(tops, 5.0);
        both->SetName("PressPull1");
        doc.AddFeature(both);
        check(doc.Errors().empty(), "both halves of a seamed top still compute");
        // The same 300mm^2 footprint, 5mm taller: 300 * 15 = 4500
        checkNear(VolumeOf(doc.Shape()), 4500.0, 1.0e-6, "300 x 15 = 4500");
        check(doc.Bodies().size() == 1, "and one body, not a stack of prisms");
    }

    // ---- 10. what a click tells you about what it picked ----
    //
    // Fusion answers a click on a face or an edge with the measurement
    // anyone would want off it. Same box: 10 x 20 x 30.
    {
        Document doc;
        MakeBox(doc);
        const std::vector<GeometryRef> faces =
            CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepFace);
        const std::vector<GeometryRef> edges =
            CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepEdge);

        check(DescribeSelection({}).empty(), "nothing picked says nothing");

        // The top face is 10 x 20 = 200.
        GeometryRef top;
        for (const GeometryRef& ref : faces) {
            if (top.IsNull() || ref.point.Z() > top.point.Z()) {
                top = ref;
            }
        }
        checkText(DescribeSelection({top}), "Face   Area 200 mm\u00B2",
                  "one face reports its area");

        // Every edge of this box is 10, 20 or 30 long.
        const std::string edgeText = DescribeSelection({edges.front()});
        check(edgeText.rfind("Edge   Length ", 0) == 0, "one edge reports its length");
        check(edgeText == "Edge   Length 10 mm" || edgeText == "Edge   Length 20 mm"
                  || edgeText == "Edge   Length 30 mm",
              "and the length is one of the box's three");

        // Two opposite 10 x 20 faces: 400 in total.
        GeometryRef bottom;
        for (const GeometryRef& ref : faces) {
            if (bottom.IsNull() || ref.point.Z() < bottom.point.Z()) {
                bottom = ref;
            }
        }
        checkText(DescribeSelection({top, bottom}), "2 faces   Total area 400 mm\u00B2",
                  "several faces total up");

        // A mixed pick has no one number that would mean anything, so it
        // counts and stops there.
        checkText(DescribeSelection({top, edges.front()}), "1 face, 1 edge",
                  "a mixed pick just counts");

        // Plurals, because "2 vertexs" would be the sort of thing nobody
        // fixes until it has been on screen for a year.
        const std::vector<GeometryRef> corners =
            CollectGeometryRefs(*doc.Bodies().front(), EntityType::BRepVertex);
        checkText(DescribeSelection({corners[0], corners[1], corners[2]}), "3 vertices",
                  "vertices, not vertexs");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL PRESS/PULL TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " PRESS/PULL TEST(S) FAILED" << std::endl;
    return 1;
}
