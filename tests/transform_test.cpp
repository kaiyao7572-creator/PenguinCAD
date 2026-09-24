// Move/rotate gizmo: does a transform land on the body the user picked,
// or on everything?
//
// A document held one body for most of this app's life, which made the
// difference invisible. Combine, patterns and mirror all make a second
// body routinely, so an untargeted transform now silently drags the whole
// model along with the handle that was dragged.
#include "core/Document.h"
#include "core/ShapeFeature.h"
#include "features/CombineFeature.h"
#include "gizmos/TransformFeature.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

using namespace lcad;

static int failures = 0;

static void check(bool ok, const std::string& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) ++failures;
}

static double VolumeOf(const TopoDS_Shape& s)
{
    if (s.IsNull()) return 0.0;
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return p.Mass();
}

// Centre of the body whose centre is nearest thePoint, so a body can be
// found again after something moved it.
static gp_Pnt NearestBodyCentre(const Document& theDoc, const gp_Pnt& thePoint)
{
    gp_Pnt best;
    double bestDistance = 1.0e30;
    for (const BodyPtr& body : theDoc.Bodies()) {
        if (!body || body->Shape().IsNull()) continue;
        GProp_GProps p;
        BRepGProp::VolumeProperties(body->Shape(), p);
        const double d = p.CentreOfMass().Distance(thePoint);
        if (d < bestDistance) {
            bestDistance = d;
            best = p.CentreOfMass();
        }
    }
    return best;
}

// Two 10mm cubes, 50mm apart: A centred (5,5,5), B centred (55,5,5).
static std::shared_ptr<ShapeFeature> TwoCubes()
{
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(compound, BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10, 10, 10).Shape());
    builder.Add(compound, BRepPrimAPI_MakeBox(gp_Pnt(50, 0, 0), 10, 10, 10).Shape());
    return std::make_shared<ShapeFeature>(compound, "Stock");
}

int main()
{
    // ---- 1. the fixture ----
    Document doc;
    doc.AddFeature(TwoCubes());
    check(doc.Bodies().size() == 2, "the document holds two bodies");
    check(std::fabs(VolumeOf(doc.Shape()) - 2000.0) < 1e-6, "2 x 1000 mm3");

    // ---- 2. a TARGETED move takes one body and leaves the other ----
    const Body* first = nullptr;
    for (const BodyPtr& body : doc.Bodies()) {
        GProp_GProps p;
        BRepGProp::VolumeProperties(body->Shape(), p);
        if (p.CentreOfMass().Distance(gp_Pnt(5, 5, 5)) < 0.1) {
            first = body.get();
        }
    }
    check(first != nullptr, "found the cube at the origin");

    if (first != nullptr) {
        auto move = std::make_shared<TransformFeature>(0.0, 0.0, 100.0, 0.0, 0.0, 0.0);
        move->SetName("Move1");
        move->SetTarget(MakeCombineBodyRef(*first));
        doc.AddFeature(move);

        if (!move->LastError().empty()) {
            std::cout << "  move error: " << move->LastError() << std::endl;
        }
        check(move->LastError().empty(), "a targeted move computes without error");
        check(doc.Bodies().size() == 2, "still two bodies afterwards");
        check(std::fabs(VolumeOf(doc.Shape()) - 2000.0) < 1e-6, "volume is unchanged by a move");

        // The picked cube went up 100mm...
        const gp_Pnt moved = NearestBodyCentre(doc, gp_Pnt(5, 5, 105));
        std::cout << "  picked cube now at (" << moved.X() << ", " << moved.Y() << ", "
                  << moved.Z() << ") -- expect (5, 5, 105)" << std::endl;
        check(moved.Distance(gp_Pnt(5, 5, 105)) < 1e-6, "the PICKED body moved 100mm in Z");

        // ...and the one that was not picked did NOT.
        const gp_Pnt stayed = NearestBodyCentre(doc, gp_Pnt(55, 5, 5));
        std::cout << "  other cube now at (" << stayed.X() << ", " << stayed.Y() << ", "
                  << stayed.Z() << ") -- expect (55, 5, 5)" << std::endl;
        check(stayed.Distance(gp_Pnt(55, 5, 5)) < 1e-6,
              "the body that was NOT picked did not move with it");
    }

    // ---- 3. no target still means "everything", which is what the
    //         numeric Move dialog means ----
    Document doc2;
    doc2.AddFeature(TwoCubes());
    auto moveAll = std::make_shared<TransformFeature>(0.0, 0.0, 100.0, 0.0, 0.0, 0.0);
    moveAll->SetName("MoveAll");
    doc2.AddFeature(moveAll);
    check(moveAll->LastError().empty(), "an untargeted move computes");
    check(NearestBodyCentre(doc2, gp_Pnt(5, 5, 105)).Distance(gp_Pnt(5, 5, 105)) < 1e-6,
          "with no pick the first body moves");
    check(NearestBodyCentre(doc2, gp_Pnt(55, 5, 105)).Distance(gp_Pnt(55, 5, 105)) < 1e-6,
          "and so does the second -- no target means the whole shape");

    // ---- 4. a target that can no longer be found fails LOUDLY ----
    Document doc3;
    doc3.AddFeature(TwoCubes());
    CombineBodyRef ghost;
    ghost.name = "Body7";
    ghost.centre = gp_Pnt(900.0, 900.0, 900.0);
    ghost.volume = 123456.0;

    auto lost = std::make_shared<TransformFeature>(0.0, 0.0, 100.0, 0.0, 0.0, 0.0);
    lost->SetName("MoveLost");
    lost->SetTarget(ghost);
    doc3.AddFeature(lost);
    check(!lost->LastError().empty(),
          "a pick that no longer resolves is refused, not widened to everything");
    check(std::fabs(VolumeOf(doc3.Shape()) - 2000.0) < 1e-6, "and the model is left alone");

    // ---- 5. undo safety ----
    auto original = std::make_shared<TransformFeature>(1.0, 2.0, 3.0, 10.0, 20.0, 30.0);
    original->SetName("Move9");
    CombineBodyRef ref;
    ref.name = "Body1";
    ref.centre = gp_Pnt(5, 5, 5);
    ref.volume = 1000.0;
    original->SetTarget(ref);

    std::unique_ptr<Feature> clonedBase = original->Clone();
    auto* cloned = dynamic_cast<TransformFeature*>(clonedBase.get());
    check(cloned != nullptr, "Clone returns a TransformFeature");
    if (cloned != nullptr) {
        check(cloned->Name() == "Move9", "Clone carries the name (CopyBaseTo was called)");
        check(cloned->Target().name == "Body1", "Clone carries the target pick");
        check(std::fabs(cloned->Target().volume - 1000.0) < 1e-9,
              "including the signature that resolves it");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL TRANSFORM TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " TRANSFORM TEST(S) FAILED" << std::endl;
    return 1;
}
