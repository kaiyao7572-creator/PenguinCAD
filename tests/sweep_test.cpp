// Sweep: a closed profile driven along a closed path, checked against
// volumes computed by hand.
//
// The headline case is a torus. A circle of radius r swept along a circle
// of radius R encloses exactly 2 * pi^2 * R * r^2, so the result is not a
// "looks plausible" check -- it either matches the closed form or the
// sweep is building the wrong solid.
#include "core/Document.h"
#include "core/ProfileProvider.h"
#include "features/SweepFeature.h"
#include "sketch/SketchEntity.h"
#include "sketch/SketchFeature.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

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

// Path: a circle of radius R on XY, centred on the origin.
// Profile: a circle of radius r on XZ, centred where the path passes
// through, so the section starts square to the path and the sweep closes
// into a torus rather than a sheared tube.
static std::shared_ptr<SketchFeature> PathCircle(double theRadius, const std::string& theName)
{
    auto sketch = std::make_shared<SketchFeature>(SketchFeature::PlaneXY(), 0.0);
    sketch->SetName(theName);
    sketch->AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), theRadius));
    return sketch;
}

static std::shared_ptr<SketchFeature> ProfileCircle(double theRadius,
                                                    double theOffsetAlongX,
                                                    const std::string& theName)
{
    auto sketch = std::make_shared<SketchFeature>(SketchFeature::PlaneXZ(), 0.0);
    sketch->SetName(theName);
    sketch->AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(theOffsetAlongX, 0.0), theRadius));
    return sketch;
}

int main()
{
    const double kPathR    = 20.0;
    const double kProfileR = 3.0;

    // ---- 1. the sketches themselves ----
    Document doc;
    auto profile = ProfileCircle(kProfileR, kPathR, "Profile");
    auto path    = PathCircle(kPathR, "Path");
    doc.AddFeature(profile);
    doc.AddFeature(path);

    check(profile->ProfileWires().size() == 1, "profile circle yields one closed wire");
    check(path->ProfileWires().size() == 1, "path circle yields one closed wire");
    check(doc.Errors().empty(), "both sketches compute without error");

    // ---- 2. sweep -> torus, 2 * pi^2 * R * r^2 ----
    auto sweep = std::make_shared<SweepFeature>("Profile", "Path");
    sweep->SetName("Sweep1");
    doc.AddFeature(sweep);

    if (!sweep->LastError().empty()) {
        std::cout << "  sweep error: " << sweep->LastError() << std::endl;
    }
    check(sweep->LastError().empty(), "sweep computes without error");

    const double expected = 2.0 * M_PI * M_PI * kPathR * kProfileR * kProfileR;
    const double actual   = VolumeOf(doc.Shape());
    std::cout << "  swept volume = " << actual << " (expect " << expected << ")" << std::endl;
    check(std::fabs(actual - expected) < expected * 0.02,
          "a circle swept along a circle is a torus of the closed-form volume");

    // ---- 3. the section is carried, not just copied: a bigger section
    //         scales the solid by r^2 ----
    Document doc2;
    doc2.AddFeature(ProfileCircle(kProfileR * 2.0, kPathR, "Profile"));
    doc2.AddFeature(PathCircle(kPathR, "Path"));
    auto sweep2 = std::make_shared<SweepFeature>("Profile", "Path");
    sweep2->SetName("Sweep1");
    doc2.AddFeature(sweep2);

    const double expected2 = 2.0 * M_PI * M_PI * kPathR * (kProfileR * 2.0) * (kProfileR * 2.0);
    const double actual2   = VolumeOf(doc2.Shape());
    std::cout << "  doubled section = " << actual2 << " (expect " << expected2 << ")"
              << std::endl;
    check(std::fabs(actual2 - expected2) < expected2 * 0.02,
          "doubling the section radius quadruples the swept volume");

    // ---- 4. degenerate answers fail LOUDLY rather than building something ----
    Document doc3;
    doc3.AddFeature(ProfileCircle(kProfileR, kPathR, "Profile"));
    doc3.AddFeature(PathCircle(kPathR, "Path"));

    auto samePath = std::make_shared<SweepFeature>("Profile", "Profile");
    samePath->SetName("SweepSame");
    doc3.AddFeature(samePath);
    check(!samePath->LastError().empty(),
          "sweeping a profile along ITSELF is refused, not quietly built");

    auto missing = std::make_shared<SweepFeature>("Profile", "NoSuchSketch");
    missing->SetName("SweepMissing");
    doc3.AddFeature(missing);
    check(!missing->LastError().empty(), "a path sketch that does not exist is refused");

    auto noProfile = std::make_shared<SweepFeature>("NoSuchSketch", "Path");
    noProfile->SetName("SweepNoProfile");
    doc3.AddFeature(noProfile);
    check(!noProfile->LastError().empty(), "a profile sketch that does not exist is refused");

    // ---- 5. undo safety: Clone must deep-copy every field ----
    auto original = std::make_shared<SweepFeature>("Profile", "Path");
    original->SetName("Sweep9");
    original->SetOrientation(SweepOrientation::Parallel);
    original->SetTaperDegrees(4.0);
    original->SetTwistDegrees(15.0);

    std::unique_ptr<Feature> clonedBase = original->Clone();
    auto* cloned = dynamic_cast<SweepFeature*>(clonedBase.get());
    check(cloned != nullptr, "Clone returns a SweepFeature");
    if (cloned != nullptr) {
        check(cloned->Name() == "Sweep9", "Clone carries the name (CopyBaseTo was called)");
        check(cloned->PathSketchName() == "Path", "Clone carries the path sketch");
        check(cloned->SketchName() == "Profile", "Clone carries the profile sketch");
        check(cloned->Orientation() == SweepOrientation::Parallel, "Clone carries orientation");
        check(std::fabs(cloned->TaperDegrees() - 4.0) < 1.0e-9, "Clone carries taper");
        check(std::fabs(cloned->TwistDegrees() - 15.0) < 1.0e-9, "Clone carries twist");

        // A clone that shared state with its original would corrupt undo.
        cloned->SetTaperDegrees(40.0);
        check(std::fabs(original->TaperDegrees() - 4.0) < 1.0e-9,
              "editing the clone does not reach back into the original");
    }

    // ---- 6. angles are stored and read back in DEGREES ----
    bool sawTaper = false;
    for (const Parameter& parameter : original->Parameters()) {
        if (parameter.name == "Taper Angle") {
            sawTaper = true;
            check(std::fabs(parameter.doubleValue - 4.0) < 1.0e-9,
                  "Taper Angle reads back the degrees it was given, not radians");
        }
    }
    check(sawTaper, "Taper Angle is exposed as an editable parameter");

    Parameter twist = Parameter::MakeDouble("Twist Angle", 33.0, "deg");
    check(original->SetParameter(twist), "Twist Angle accepts an edit");
    check(std::fabs(original->TwistDegrees() - 33.0) < 1.0e-9, "and stores it in degrees");

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL SWEEP TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " SWEEP TEST(S) FAILED" << std::endl;
    return 1;
}
