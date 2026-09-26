// User parameters wired into the document: a feature's number driven by
// an expression over named values, and a change to one name reaching the
// solid. Everything before this was "parametric-looking" -- you could not
// write hole_dia = plate_width / 4 and have it mean anything.
//
// Every expected volume is computed by hand in the comment beside it.
#include "core/Document.h"
#include "features/PrimitiveFeatures.h"
#include "features/ProfileFeatures.h"
#include "sketch/SketchConstraints.h"
#include "sketch/SketchFeature.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cmath>
#include <iostream>

using namespace lcad;

namespace {

int failures = 0;
constexpr double kPi = 3.14159265358979323846;

double VolumeOf(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return 0.0;
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(theShape, props);
    return props.Mass();
}

void check(bool theOk, const std::string& theWhat)
{
    std::cout << (theOk ? "  PASS  " : "  FAIL  ") << theWhat << std::endl;
    if (!theOk) {
        ++failures;
    }
}

void checkVolume(const Document& theDocument, double theExpected, const std::string& theWhat)
{
    const double volume = VolumeOf(theDocument.Shape());
    std::cout << "        volume " << volume << ", expect " << theExpected << std::endl;
    check(std::fabs(volume - theExpected) < 0.01, theWhat);
}

FeaturePtr Find(const Document& theDocument, const std::string& theName)
{
    for (const FeaturePtr& feature : theDocument.Features()) {
        if (feature && feature->Name() == theName) {
            return feature;
        }
    }
    return nullptr;
}

Parameter ParameterOf(const FeaturePtr& theFeature, const std::string& theName)
{
    for (const Parameter& parameter : theFeature->EditableParameters()) {
        if (parameter.name == theName) {
            return parameter;
        }
    }
    return Parameter();
}

// The edit a numeric field sends when the user types text into it.
bool Drive(Document& theDocument, const std::string& theFeature, const std::string& theParameter,
           const std::string& theExpression, std::string& theError)
{
    const FeaturePtr feature = Find(theDocument, theFeature);
    if (!feature) {
        theError = "no feature " + theFeature;
        return false;
    }
    Parameter edited = ParameterOf(feature, theParameter);
    edited.expression = theExpression;
    return theDocument.SetFeatureParameter(feature, edited, theError);
}

bool AddLength(Document& theDocument, const std::string& theName, const std::string& theExpression)
{
    UserParameter parameter;
    parameter.name = theName;
    parameter.expression = theExpression;
    std::string error;
    const bool ok = theDocument.AddUserParameter(parameter, error);
    if (!ok) {
        std::cout << "        refused: " << error << std::endl;
    }
    return ok;
}

// A feature with a bounded number, to see a range enforced on an
// expression the same as on a typed value.
class RangedFeature : public Feature
{
public:
    std::string TypeName() const override { return "Ranged"; }
    bool Compute(const ComputeContext&, const TopoDS_Shape& theInput, TopoDS_Shape& theOutput,
                 std::string&) override
    {
        theOutput = theInput;
        return true;
    }
    std::unique_ptr<Feature> Clone() const override
    {
        auto copy = std::make_unique<RangedFeature>();
        copy->mySize = mySize;
        CopyBaseTo(*copy);
        return copy;
    }
    std::vector<Parameter> Parameters() const override
    {
        Parameter size = Parameter::MakeDouble("Size", mySize);
        size.minimum = 1.0;
        size.maximum = 10.0;
        return {size};
    }
    bool SetParameter(const Parameter& theParameter) override
    {
        if (theParameter.name != "Size") {
            return false;
        }
        mySize = theParameter.doubleValue;
        return true;
    }
    double mySize = 5.0;
};

// A pattern-like feature with a bounded whole-number Count.
class CountedFeature : public Feature
{
public:
    std::string TypeName() const override { return "Counted"; }
    bool Compute(const ComputeContext&, const TopoDS_Shape& theInput, TopoDS_Shape& theOutput,
                 std::string&) override
    {
        theOutput = theInput;
        return true;
    }
    std::unique_ptr<Feature> Clone() const override
    {
        auto copy = std::make_unique<CountedFeature>();
        copy->myCount = myCount;
        CopyBaseTo(*copy);
        return copy;
    }
    std::vector<Parameter> Parameters() const override
    {
        Parameter count = Parameter::MakeInt("Count", myCount);
        count.minimum = 1.0;
        count.maximum = 10.0;
        return {count};
    }
    bool SetParameter(const Parameter& theParameter) override
    {
        if (theParameter.name != "Count") {
            return false;
        }
        myCount = theParameter.intValue;
        return true;
    }
    int myCount = 2;
};

} // namespace

int main()
{
    std::string error;

    std::cout << "-- the handoff's own example: hole_dia = plate_width / 4 --" << std::endl;
    Document doc;
    check(AddLength(doc, "plate_width", "40 mm"), "plate_width = 40 mm");
    check(AddLength(doc, "hole_dia", "plate_width / 4"), "hole_dia = plate_width / 4");
    check(AddLength(doc, "plate_t", "5 mm"), "plate_t = 5 mm");
    check(std::fabs(doc.UserParameters().Find("hole_dia")->value - 10.0) < 1e-9,
          "hole_dia resolves to 10 mm");

    // A circle of diameter 12 with a driving diameter dimension, d1.
    auto sketch = std::make_shared<SketchFeature>(SketchFeature::PlaneXY(), 0.0);
    sketch->SetName("Sketch1");
    const int circle = sketch->AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 6.0));
    SketchConstraint diameter;
    diameter.type = SketchConstraintType::Diameter;
    diameter.a = SketchPointRef{circle, SketchPointRole::Whole};
    diameter.value = 12.0;
    sketch->AddConstraint(diameter);
    doc.AddFeature(sketch);

    auto extrude = std::make_shared<ExtrudeFeature>();
    extrude->SetName("Extrude1");
    extrude->SetSketchName("Sketch1");
    extrude->SetDistance(3.0);
    doc.AddFeature(extrude);
    // pi * 6^2 * 3 = 339.292
    checkVolume(doc, kPi * 36.0 * 3.0, "plain numbers before anything is driven");

    check(Drive(doc, "Sketch1", "d1", "hole_dia", error), "sketch dimension d1 = hole_dia");
    check(Drive(doc, "Extrude1", "Distance", "plate_t * 2", error), "extrude distance = plate_t * 2");
    // pi * 5^2 * 10 = 785.398
    checkVolume(doc, kPi * 25.0 * 10.0, "both expressions reach the solid");
    check(doc.Errors().empty(), "and nothing failed to compute");

    std::cout << "-- THE assertion: edit one name, the solid changes --" << std::endl;
    check(doc.SetUserParameterExpression("plate_width", "80 mm", error), "plate_width = 80 mm");
    // hole_dia = 20, r = 10: pi * 10^2 * 10 = 3141.593
    checkVolume(doc, kPi * 100.0 * 10.0,
                "plate_width -> hole_dia -> d1 -> the circle -> the extrude");
    check(doc.SetUserParameterExpression("plate_t", "2.5 mm", error), "plate_t = 2.5 mm");
    // pi * 10^2 * 5 = 1570.796
    checkVolume(doc, kPi * 100.0 * 5.0, "and plate_t drives the height");

    std::cout << "-- what an editor sees --" << std::endl;
    {
        const Parameter distance = ParameterOf(Find(doc, "Extrude1"), "Distance");
        check(distance.expression == "plate_t * 2", "the Distance row carries its expression");
        check(std::fabs(distance.doubleValue - 5.0) < 1e-9, "and the number it gives");
        const Parameter reversed = ParameterOf(Find(doc, "Extrude1"), "Reversed");
        check(reversed.expression.empty(), "a Bool row carries none");
    }

    std::cout << "-- undo puts parameters back WITH their effect on the model --" << std::endl;
    doc.Undo();  // plate_t back to 5
    checkVolume(doc, kPi * 100.0 * 10.0, "undo the plate_t edit");
    check(std::fabs(doc.UserParameters().Find("plate_t")->value - 5.0) < 1e-9,
          "and the table says 5 again");
    doc.Undo();  // plate_width back to 40
    checkVolume(doc, kPi * 25.0 * 10.0, "undo the plate_width edit");
    doc.Redo();
    checkVolume(doc, kPi * 100.0 * 10.0, "redo it");
    check(Find(doc, "Extrude1")->ExpressionOf("Distance") == "plate_t * 2",
          "the expression survived being restored from clones");

    std::cout << "-- a field that cannot evaluate is refused, not stored --" << std::endl;
    {
        const bool accepted = Drive(doc, "Extrude1", "Distance", "plate_tt * 2", error);
        check(!accepted, "an unknown name is refused");
        std::cout << "        said: " << error << std::endl;
        check(error.find("plate_tt") != std::string::npos, "and names the unknown name");
        check(Find(doc, "Extrude1")->ExpressionOf("Distance") == "plate_t * 2",
              "the old expression is untouched");
        check(!Drive(doc, "Extrude1", "Distance", "plate_t *", error), "malformed text is refused");
        checkVolume(doc, kPi * 100.0 * 10.0, "and the model is untouched");
    }

    std::cout << "-- a refused edit leaves no undo step behind --" << std::endl;
    {
        check(!AddLength(doc, "plate_t", "1 mm"), "a duplicate name is refused");
        check(!AddLength(doc, "sin", "1 mm"), "a reserved name is refused");
        doc.Undo();  // must undo the plate_width redo above, not a refused edit
        checkVolume(doc, kPi * 25.0 * 10.0, "undo skips straight to the last real edit");
        doc.Redo();
    }

    std::cout << "-- renaming rewrites the features that read it --" << std::endl;
    check(doc.RenameUserParameter("plate_t", "thickness", error), "rename plate_t -> thickness");
    check(Find(doc, "Extrude1")->ExpressionOf("Distance") == "thickness * 2",
          "the extrude now reads thickness * 2");
    checkVolume(doc, kPi * 100.0 * 10.0, "and the model did not move");
    check(doc.Errors().empty(), "and nothing broke");
    doc.Undo();
    check(Find(doc, "Extrude1")->ExpressionOf("Distance") == "plate_t * 2",
          "undo gives the old name back to the feature too");
    check(doc.UserParameters().Find("plate_t") != nullptr, "and to the table");
    doc.Redo();

    std::cout << "-- removing a name a feature reads breaks it loudly --" << std::endl;
    check(doc.RemoveUserParameter("thickness", error), "removal is allowed");
    {
        const FeaturePtr live = Find(doc, "Extrude1");
        check(!live->LastError().empty(), "the extrude reports an error");
        std::cout << "        said: " << live->LastError() << std::endl;
        check(live->LastError().find("thickness") != std::string::npos,
              "which names the missing parameter");
        check(VolumeOf(doc.Shape()) < 1e-6,
              "and it does NOT quietly keep building on the last number");
    }
    doc.Undo();
    checkVolume(doc, kPi * 100.0 * 10.0, "undo brings the parameter and the solid back");
    check(doc.Errors().empty(), "cleanly");

    std::cout << "-- who reads a parameter, before offering Delete --" << std::endl;
    {
        const std::vector<std::string> users = doc.UsersOfUserParameter("plate_width");
        check(users.size() == 2 && users[0] == "hole_dia" && users[1] == "Sketch1",
              "plate_width is read by hole_dia, and through it by Sketch1");
        const std::vector<std::string> direct = doc.UsersOfUserParameter("hole_dia");
        check(direct.size() == 1 && direct[0] == "Sketch1", "hole_dia is read by Sketch1");
        check(doc.UsersOfUserParameter("thickness").size() == 1, "thickness by Extrude1");
    }

    std::cout << "-- a cycle is refused and changes nothing --" << std::endl;
    check(!doc.SetUserParameterExpression("plate_width", "hole_dia * 4", error),
          "plate_width = hole_dia * 4 would loop");
    std::cout << "        said: " << error << std::endl;
    checkVolume(doc, kPi * 100.0 * 10.0, "the model is untouched");

    std::cout << "-- constants stay numbers, names stay expressions --" << std::endl;
    check(Drive(doc, "Extrude1", "Distance", "3 * 4", error), "Distance = 3 * 4");
    check(Find(doc, "Extrude1")->ExpressionOf("Distance").empty(),
          "arithmetic on numbers alone is stored as the number it gives");
    checkVolume(doc, kPi * 100.0 * 12.0, "12 mm");
    check(Drive(doc, "Extrude1", "Distance", "1 in", error), "Distance = 1 in");
    checkVolume(doc, kPi * 100.0 * 25.4, "a unit written into it converts");
    check(Find(doc, "Extrude1")->ExpressionOf("Distance").empty(),
          "and with no name in it, it is a number too");
    {
        Parameter plain = ParameterOf(Find(doc, "Extrude1"), "Distance");
        plain.expression.clear();
        plain.doubleValue = 7.0;
        check(doc.SetFeatureParameter(Find(doc, "Extrude1"), plain, error),
              "a plain number is still a plain edit");
    }
    checkVolume(doc, kPi * 100.0 * 7.0, "7 mm");
    doc.Undo();
    doc.Undo();
    doc.Undo();
    // plate_t was renamed thickness above, and that rename still stands.
    check(Find(doc, "Extrude1")->ExpressionOf("Distance") == "thickness * 2",
          "and a feature parameter edit is undoable like any other");
    checkVolume(doc, kPi * 100.0 * 10.0, "back to thickness * 2");

    std::cout << "-- units on the parameter, not just the literal --" << std::endl;
    {
        UserParameter inches;
        inches.name = "lip";
        inches.expression = "1";
        inches.lengthUnit = LengthUnit::Inch;
        check(doc.AddUserParameter(inches, error), "lip = 1, shown in inches");
        check(std::fabs(doc.UserParameters().Find("lip")->value - 25.4) < 1e-9,
              "is 25.4 mm inside");
        check(Drive(doc, "Extrude1", "Distance", "lip", error), "Distance = lip");
        checkVolume(doc, kPi * 100.0 * 25.4, "the extrude is an inch tall");
    }

    std::cout << "-- any feature's number, not just Extrude's --" << std::endl;
    {
        Document boxes;
        check(AddLength(boxes, "side", "10 mm"), "side = 10 mm");
        auto box = std::make_shared<BoxFeature>(1.0, 1.0, 1.0);
        box->SetName("Box1");
        boxes.AddFeature(box);
        check(Drive(boxes, "Box1", "Length", "side", error), "Length = side");
        check(Drive(boxes, "Box1", "Width", "side", error), "Width = side");
        check(Drive(boxes, "Box1", "Height", "side / 2", error), "Height = side / 2");
        checkVolume(boxes, 500.0, "10 x 10 x 5");
        check(boxes.SetUserParameterExpression("side", "20 mm", error), "side = 20 mm");
        checkVolume(boxes, 4000.0, "20 x 20 x 10");

        const std::unique_ptr<Feature> copy = Find(boxes, "Box1")->Clone();
        check(copy->ExpressionOf("Height") == "side / 2", "Clone carries the expressions");
    }

    std::cout << "-- a range is enforced on an expression like on a typed number --" << std::endl;
    {
        Document ranged;
        check(AddLength(ranged, "s", "4 mm"), "s = 4 mm");
        auto feature = std::make_shared<RangedFeature>();
        feature->SetName("Ranged1");
        ranged.AddFeature(feature);
        check(!Drive(ranged, "Ranged1", "Size", "s * 5", error), "Size = s * 5 (20) is refused");
        std::cout << "        said: " << error << std::endl;
        check(Drive(ranged, "Ranged1", "Size", "s * 2", error), "Size = s * 2 (8) is fine");
        check(ranged.SetUserParameterExpression("s", "6 mm", error), "then s = 6 mm");
        const FeaturePtr live = Find(ranged, "Ranged1");
        check(!live->LastError().empty(), "pushes Size to 12 and the rebuild says so");
        std::cout << "        said: " << live->LastError() << std::endl;
    }

    std::cout << "-- an angle survives the degrees/radians round trip --" << std::endl;
    {
        // An angle goes in as degrees and a sketch stores it as radians.
        // Document only pushes a value that really changed, so a rebuild
        // must neither drift the angle nor refuse it.
        Document angled;
        check(AddLength(angled, "unused", "1 mm"), "a table to evaluate against");
        auto lines = std::make_shared<SketchFeature>(SketchFeature::PlaneXY(), 0.0);
        lines->SetName("Sketch1");
        const int a = lines->AddEntity(SketchEntity::MakeLine(gp_Pnt2d(0, 0), gp_Pnt2d(10, 0)));
        const int b = lines->AddEntity(SketchEntity::MakeLine(gp_Pnt2d(0, 0), gp_Pnt2d(0, 10)));
        SketchConstraint angle;
        angle.type = SketchConstraintType::Angle;
        angle.a = SketchPointRef{a, SketchPointRole::Whole};
        angle.b = SketchPointRef{b, SketchPointRole::Whole};
        angle.value = kPi / 6.0;
        lines->AddConstraint(angle);
        angled.AddFeature(lines);
        check(Drive(angled, "Sketch1", "d1", "30 deg + unused * 0", error), "d1 = 30 deg");
        const Parameter d1 = ParameterOf(Find(angled, "Sketch1"), "d1");
        check(std::fabs(d1.doubleValue - 30.0) < 1e-9, "reads 30 degrees");
        angled.Rebuild();
        angled.Rebuild();
        check(std::fabs(ParameterOf(Find(angled, "Sketch1"), "d1").doubleValue - 30.0) < 1e-9,
              "and still 30 after rebuilding twice");
    }

    std::cout << "-- what the review found --" << std::endl;
    {
        Document d;
        check(AddLength(d, "plate_width", "40 mm"), "plate_width = 40 mm");
        check(AddLength(d, "hole_dia", "plate_width / 4"), "hole_dia = plate_width / 4");
        auto s = std::make_shared<SketchFeature>(SketchFeature::PlaneXY(), 0.0);
        s->SetName("Sketch1");
        const int c = s->AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(0.0, 0.0), 6.0));
        SketchConstraint dia;
        dia.type = SketchConstraintType::Diameter;
        dia.a = SketchPointRef{c, SketchPointRole::Whole};
        dia.value = 12.0;
        const int diaId = s->AddConstraint(dia);
        d.AddFeature(s);
        auto e = std::make_shared<ExtrudeFeature>();
        e->SetName("Extrude1");
        e->SetSketchName("Sketch1");
        e->SetDistance(10.0);
        d.AddFeature(e);
        check(Drive(d, "Sketch1", "d1", "hole_dia", error), "d1 = hole_dia");
        checkVolume(d, kPi * 25.0 * 10.0, "a 10 mm hole, 10 deep");

        // 1. A sketch dimension that stops evaluating fails what is built
        //    on the sketch too -- not just the sketch.
        check(d.SetUserParameterExpression("plate_width", "40 mmm", error),
              "plate_width = \"40 mmm\" (a typo the table accepts as a broken row)");
        const FeaturePtr ext = Find(d, "Extrude1");
        std::cout << "        extrude says: " << ext->LastError() << std::endl;
        check(!ext->LastError().empty(), "the extrude fails with the sketch");
        check(VolumeOf(d.Shape()) < 1e-6, "and does NOT keep building the 10 mm hole");

        // 2. A broken parameter is reported as broken, not as missing.
        std::cout << "        sketch says: " << Find(d, "Sketch1")->LastError() << std::endl;
        check(Find(d, "Sketch1")->LastError().find("has an error") != std::string::npos,
              "hole_dia is reported as having an error, not as not existing");
        check(Find(d, "Sketch1")->LastError().find("no parameter named") == std::string::npos,
              "no \"there is no parameter named hole_dia\"");
        check(d.SetUserParameterExpression("plate_width", "40 mm", error), "fix the typo");
        checkVolume(d, kPi * 25.0 * 10.0, "and the hole comes back");

        // 3. An edit that changes nothing is not an undo step: Redo survives.
        check(Drive(d, "Extrude1", "Distance", "20", error), "Distance = 20");
        d.Undo();
        check(d.CanRedo(), "undo leaves a redo");
        Parameter same = ParameterOf(Find(d, "Extrude1"), "Distance");
        check(d.SetFeatureParameter(Find(d, "Extrude1"), same, error),
              "re-committing the value it already has is accepted");
        check(d.CanRedo(), "and does not throw the redo away");
        d.Redo();
        checkVolume(d, kPi * 25.0 * 20.0, "redo still restores Distance = 20");

        // 4. A literal of the wrong kind is refused, not converted.
        check(!Drive(d, "Extrude1", "Distance", "90 deg", error),
              "90 deg is refused as an extrude distance");
        std::cout << "        said: " << error << std::endl;

        // 5. A deleted dimension's leftover expression does not pin the
        //    parameter it read.
        auto live = std::dynamic_pointer_cast<SketchFeature>(Find(d, "Sketch1"));
        d.PushUndoSnapshot();
        live = std::dynamic_pointer_cast<SketchFeature>(Find(d, "Sketch1"));
        check(live->RemoveConstraint(diaId), "delete the d1 dimension");
        d.Rebuild();
        check(d.UsersOfUserParameter("hole_dia").empty(),
              "hole_dia is used by nothing once the dimension it drove is gone");
    }

    std::cout << "-- a count driven by an expression, as a pattern quantity is --" << std::endl;
    {
        Document d;
        UserParameter n;
        n.name = "n";
        n.expression = "3";
        n.kind = UnitKind::Unitless;
        check(d.AddUserParameter(n, error), "n = 3 (no units)");
        auto counted = std::make_shared<CountedFeature>();
        counted->SetName("Counted1");
        d.AddFeature(counted);
        check(Drive(d, "Counted1", "Count", "n", error), "Count = n");
        auto live = std::dynamic_pointer_cast<CountedFeature>(Find(d, "Counted1"));
        check(live->myCount == 3, "the count is 3");
        check(ParameterOf(live, "Count").expression == "n", "and the row shows its expression");
        check(d.SetUserParameterExpression("n", "7", error), "n = 7");
        live = std::dynamic_pointer_cast<CountedFeature>(Find(d, "Counted1"));
        check(live->myCount == 7, "the count follows it to 7");

        check(!Drive(d, "Counted1", "Count", "n / 2", error), "Count = n / 2 (3.5) is refused");
        std::cout << "        said: " << error << std::endl;
        check(error.find("whole number") != std::string::npos, "because a count must be whole");

        check(d.SetUserParameterExpression("n", "4.5", error), "n = 4.5 is fine in the table");
        live = std::dynamic_pointer_cast<CountedFeature>(Find(d, "Counted1"));
        std::cout << "        feature says: " << live->LastError() << std::endl;
        check(!live->LastError().empty(), "but the feature it drives fails, not rounds");
        check(d.SetUserParameterExpression("n", "20", error), "n = 20");
        check(!Find(d, "Counted1")->LastError().empty(), "and 20 is outside Count's 1..10");
        check(d.SetUserParameterExpression("n", "5", error), "n = 5");
        live = std::dynamic_pointer_cast<CountedFeature>(Find(d, "Counted1"));
        check(live->LastError().empty() && live->myCount == 5, "back to a good count of 5");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "ALL DOCUMENT PARAMETER TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failures << " FAILURE(S)" << std::endl;
    return 1;
}
