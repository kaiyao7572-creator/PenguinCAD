// Saving a design and opening it again (.pcad, src/io/NativeFormat).
//
// Every feature type the format knows is built into a real design, written
// out, read back into a fresh document through Document::ReplaceDesign,
// rebuilt, and compared with the original: the same volume, the same body
// names, the same expressions and d# names, the same user parameters --
// and written out a second time, the very same bytes. A file that changes
// every time it is opened and saved again is a file nobody can diff.
//
// Then the refusals: a newer version, an unknown feature type and broken
// JSON each give a message and leave the open design exactly as it was.
#include "core/Body.h"
#include "core/Document.h"
#include "core/GeometryRef.h"
#include "core/ShapeFeature.h"
#include "features/CombineFeature.h"
#include "features/ConstructionFeatures.h"
#include "features/LoftFeature.h"
#include "features/ModifyFeatures.h"
#include "features/PatternFeatures.h"
#include "features/PressPullFeature.h"
#include "features/PrimitiveFeatures.h"
#include "features/ProfileFeatures.h"
#include "features/SweepFeature.h"
#include "gizmos/TransformFeature.h"
#include "io/NativeFormat.h"
#include "sketch/SketchConstraints.h"
#include "sketch/SketchFeature.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <gp_Trsf.hxx>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <clocale>
#include <cmath>
#include <iostream>
#include <set>

using namespace lcad;

namespace {

int failures = 0;
constexpr double kPi = 3.14159265358979323846;

// Every "type" any round trip below wrote, to prove none was skipped.
std::set<std::string> theTypesSeen;

void check(bool theOk, const std::string& theWhat)
{
    std::cout << (theOk ? "  PASS  " : "  FAIL  ") << theWhat << std::endl;
    if (!theOk) {
        ++failures;
    }
}

double VolumeOf(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return 0.0;
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(theShape, props);
    return props.Mass();
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

// What a numeric field sends when the user types an expression into it.
bool Drive(Document& theDocument, const std::string& theFeature, const std::string& theParameter,
           const std::string& theExpression)
{
    const FeaturePtr feature = Find(theDocument, theFeature);
    if (!feature) {
        std::cout << "        no feature " << theFeature << std::endl;
        return false;
    }
    Parameter edited = ParameterOf(feature, theParameter);
    edited.expression = theExpression;
    std::string error;
    if (!theDocument.SetFeatureParameter(feature, edited, error)) {
        std::cout << "        refused: " << error << std::endl;
        return false;
    }
    return true;
}

bool AddParameter(Document& theDocument, const std::string& theName,
                  const std::string& theExpression, UnitKind theKind = UnitKind::Length,
                  const std::string& theComment = std::string())
{
    UserParameter row;
    row.name = theName;
    row.expression = theExpression;
    row.kind = theKind;
    row.comment = theComment;
    std::string error;
    if (!theDocument.AddUserParameter(row, error)) {
        std::cout << "        refused: " << error << std::endl;
        return false;
    }
    return true;
}

std::string Describe(const Parameter& theParameter)
{
    switch (theParameter.type) {
        case Parameter::Type::Double: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.17g", theParameter.doubleValue);
            return buffer;
        }
        case Parameter::Type::Int:    return std::to_string(theParameter.intValue);
        case Parameter::Type::Bool:   return theParameter.boolValue ? "true" : "false";
        case Parameter::Type::String: return "\"" + theParameter.stringValue + "\"";
    }
    return std::string();
}

// The design the way a user would judge it came back: what it is made of,
// what it evaluates to, and every name an expression or a pick relies on.
void CompareDesigns(const Document& theSaved, const Document& theOpened, const std::string& theWhat)
{
    const double savedVolume = VolumeOf(theSaved.Shape());
    const double openedVolume = VolumeOf(theOpened.Shape());
    std::cout << "        volume " << savedVolume << " saved, " << openedVolume << " reopened"
              << std::endl;
    check(std::fabs(savedVolume - openedVolume) <= 1.0e-9 * std::max(1.0, savedVolume),
          theWhat + ": the same volume");
    check(theOpened.Errors() == theSaved.Errors(), theWhat + ": the same rebuild errors (none)");

    bool sameTimeline = theSaved.FeatureCount() == theOpened.FeatureCount();
    for (std::size_t i = 0; sameTimeline && i < theSaved.FeatureCount(); ++i) {
        const Feature& a = *theSaved.Features()[i];
        const Feature& b = *theOpened.Features()[i];
        std::vector<Parameter> pa = a.Parameters();
        std::vector<Parameter> pb = b.Parameters();
        bool same = a.TypeName() == b.TypeName() && a.Name() == b.Name()
                    && a.IsSuppressed() == b.IsSuppressed() && pa.size() == pb.size();
        for (std::size_t j = 0; same && j < pa.size(); ++j) {
            same = pa[j].name == pb[j].name && Describe(pa[j]) == Describe(pb[j]);
            if (!same) {
                std::cout << "        " << a.Name() << "." << pa[j].name << ": "
                          << Describe(pa[j]) << " saved, " << Describe(pb[j]) << " reopened"
                          << std::endl;
            }
        }
        sameTimeline = same;
    }
    check(sameTimeline, theWhat + ": the same timeline, feature by feature, parameter by parameter");

    bool sameExpressions = sameTimeline;
    for (std::size_t i = 0; sameExpressions && i < theSaved.FeatureCount(); ++i) {
        sameExpressions = theSaved.Features()[i]->Expressions() == theOpened.Features()[i]->Expressions()
                          && theSaved.Features()[i]->ModelNames() == theOpened.Features()[i]->ModelNames();
    }
    check(sameExpressions, theWhat + ": the same expressions and d# names");

    bool sameBodies = theSaved.Bodies().size() == theOpened.Bodies().size();
    for (std::size_t i = 0; sameBodies && i < theSaved.Bodies().size(); ++i) {
        sameBodies = theSaved.Bodies()[i]->Name() == theOpened.Bodies()[i]->Name()
                     && theSaved.Bodies()[i]->IsVisible() == theOpened.Bodies()[i]->IsVisible();
    }
    check(sameBodies, theWhat + ": the same bodies, by name");

    const auto& ua = theSaved.UserParameters().Parameters();
    const auto& ub = theOpened.UserParameters().Parameters();
    bool sameUser = ua.size() == ub.size();
    for (std::size_t i = 0; sameUser && i < ua.size(); ++i) {
        sameUser = ua[i].name == ub[i].name && ua[i].expression == ub[i].expression
                   && ua[i].comment == ub[i].comment && ua[i].kind == ub[i].kind
                   && ua[i].lengthUnit == ub[i].lengthUnit && ua[i].angleUnit == ub[i].angleUnit
                   && ua[i].value == ub[i].value;
    }
    check(sameUser, theWhat + ": the same user parameters");
    check(theSaved.RollbackIndex() == theOpened.RollbackIndex(),
          theWhat + ": the same timeline marker");
}

void NoteTypes(const QByteArray& theText)
{
    const QJsonArray features =
        QJsonDocument::fromJson(theText).object().value("features").toArray();
    for (const QJsonValue& value : features) {
        theTypesSeen.insert(value.toObject().value("type").toString().toStdString());
    }
}

// Save theSaved, open it into theOpened, rebuild, compare, save again.
void RoundTrip(const Document& theSaved, Document& theOpened, const std::string& theWhat)
{
    check(theSaved.Errors().empty(), theWhat + ": the design itself builds cleanly");
    for (const std::string& error : theSaved.Errors()) {
        std::cout << "        " << error << std::endl;
    }

    DesignExtras extras;
    QByteArray first;
    std::string error;
    if (!WriteNativeText(theSaved, extras, first, error)) {
        check(false, theWhat + ": writes (" + error + ")");
        return;
    }
    NoteTypes(first);

    Document::DesignState design;
    DesignExtras readExtras;
    if (!ReadNativeText(first, design, readExtras, error)) {
        check(false, theWhat + ": reads back (" + error + ")");
        return;
    }
    if (!theOpened.ReplaceDesign(std::move(design), error)) {
        check(false, theWhat + ": opens (" + error + ")");
        return;
    }
    check(!theOpened.CanUndo() && !theOpened.IsModified(),
          theWhat + ": opens with no undo history and nothing modified");

    CompareDesigns(theSaved, theOpened, theWhat);

    QByteArray second;
    if (!WriteNativeText(theOpened, readExtras, second, error)) {
        check(false, theWhat + ": writes again (" + error + ")");
        return;
    }
    check(first == second, theWhat + ": save -> open -> save is byte-identical");
    if (first != second) {
        const QList<QByteArray> a = first.split('\n');
        const QList<QByteArray> b = second.split('\n');
        for (int i = 0; i < std::min(a.size(), b.size()); ++i) {
            if (a[i] != b[i]) {
                std::cout << "        line " << i + 1 << ": " << a[i].toStdString() << "\n"
                          << "             vs " << b[i].toStdString() << std::endl;
                break;
            }
        }
    }
}

SketchPointRef Point(int theEntity, SketchPointRole theRole)
{
    return SketchPointRef{theEntity, theRole};
}

SketchConstraint Relation(SketchConstraintType theType, SketchPointRef theA,
                          SketchPointRef theB = SketchPointRef())
{
    SketchConstraint constraint;
    constraint.type = theType;
    constraint.a = theA;
    constraint.b = theB;
    return constraint;
}

// A 40 x 20 plate outline with a 10 mm hole: fully related the way the
// rectangle tool relates one, one construction diagonal, and two driving
// dimensions (d1 the width, d2 the hole's diameter).
std::shared_ptr<SketchFeature> PlateSketch(const std::string& theName)
{
    auto sketch = std::make_shared<SketchFeature>(SketchFeature::PlaneXY(), 0.0);
    sketch->SetName(theName);
    const int bottom = sketch->AddEntity(SketchEntity::MakeLine(gp_Pnt2d(0, 0), gp_Pnt2d(40, 0)));
    const int right = sketch->AddEntity(SketchEntity::MakeLine(gp_Pnt2d(40, 0), gp_Pnt2d(40, 20)));
    const int top = sketch->AddEntity(SketchEntity::MakeLine(gp_Pnt2d(40, 20), gp_Pnt2d(0, 20)));
    const int left = sketch->AddEntity(SketchEntity::MakeLine(gp_Pnt2d(0, 20), gp_Pnt2d(0, 0)));
    const int hole = sketch->AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(20, 10), 5.0));
    SketchEntity diagonal = SketchEntity::MakeLine(gp_Pnt2d(0, 0), gp_Pnt2d(40, 20));
    diagonal.isConstruction = true;
    sketch->AddEntity(diagonal);

    using T = SketchConstraintType;
    using R = SketchPointRole;
    sketch->AddConstraint(Relation(T::Coincident, Point(bottom, R::End), Point(right, R::Start)));
    sketch->AddConstraint(Relation(T::Coincident, Point(right, R::End), Point(top, R::Start)));
    sketch->AddConstraint(Relation(T::Coincident, Point(top, R::End), Point(left, R::Start)));
    sketch->AddConstraint(Relation(T::Coincident, Point(left, R::End), Point(bottom, R::Start)));
    sketch->AddConstraint(Relation(T::Horizontal, Point(bottom, R::Whole)));
    sketch->AddConstraint(Relation(T::Horizontal, Point(top, R::Whole)));
    sketch->AddConstraint(Relation(T::Vertical, Point(left, R::Whole)));
    sketch->AddConstraint(Relation(T::Vertical, Point(right, R::Whole)));
    sketch->AddConstraint(Relation(T::Fix, Point(left, R::Whole)));

    SketchConstraint width = Relation(T::Distance, Point(bottom, R::Start), Point(bottom, R::End));
    width.value = 40.0;
    width.labelPosition = gp_Pnt2d(20.0, -6.0);
    sketch->AddConstraint(width);
    SketchConstraint diameter = Relation(T::Diameter, Point(hole, R::Whole));
    diameter.value = 10.0;
    diameter.labelPosition = gp_Pnt2d(28.0, 14.0);
    sketch->AddConstraint(diameter);
    return sketch;
}

std::shared_ptr<SketchFeature> CircleSketch(const gp_Ax3& thePlane, double theOffset,
                                            const gp_Pnt2d& theCentre, double theRadius,
                                            const std::string& theName)
{
    auto sketch = std::make_shared<SketchFeature>(thePlane, theOffset);
    sketch->SetName(theName);
    sketch->AddEntity(SketchEntity::MakeCircle(theCentre, theRadius));
    return sketch;
}

// The region of a sketch with the largest area -- the plate, not the hole.
ProfileRef LargestRegion(const SketchFeature& theSketch)
{
    ProfileRef best;
    double bestArea = -1.0;
    for (const ProfileRegion& region : theSketch.ProfileRegions()) {
        if (region.area > bestArea) {
            bestArea = region.area;
            best = region.ref;
        }
    }
    return best;
}

CombineBodyRef BodyRef(const Document& theDocument, std::size_t theIndex)
{
    return MakeCombineBodyRef(*theDocument.Bodies().at(theIndex));
}

std::shared_ptr<BoxFeature> BoxAt(double theSize, const gp_Pnt& theOrigin, const std::string& theName)
{
    auto box = std::make_shared<BoxFeature>(theSize, theSize, theSize);
    box->SetOrigin(theOrigin);
    box->SetName(theName);
    return box;
}

// Text surgery for the refusal tests: the saved design with one thing
// changed, the way a hand edit or a newer PenguinCAD would change it.
QByteArray Edited(const QByteArray& theText, const QByteArray& theFrom, const QByteArray& theTo)
{
    QByteArray text = theText;
    const int at = text.indexOf(theFrom);
    if (at < 0) {
        std::cout << "        (the saved text has no " << theFrom.toStdString() << ")" << std::endl;
        return text;
    }
    return text.replace(at, theFrom.size(), theTo);
}

// A feature class the format has no entry for, as a plug-in or a class
// added without a row in NativeFormat's table would be.
class UnlistedFeature : public Feature
{
public:
    std::string TypeName() const override { return "Unlisted"; }
    bool Compute(const ComputeContext&, const TopoDS_Shape& theInput, TopoDS_Shape& theOutput,
                 std::string&) override
    {
        theOutput = theInput;
        return true;
    }
    std::unique_ptr<Feature> Clone() const override
    {
        auto copy = std::make_unique<UnlistedFeature>();
        CopyBaseTo(*copy);
        return copy;
    }
};

} // namespace

int main()
{
    // ---- 1. sketch + extrude + fillet, driven by user parameters ----
    std::cout << "\n-- 1. a plate: sketch, extrude of one region, fillet, parameters --" << std::endl;
    QByteArray plateText;
    {
        Document saved;
        check(AddParameter(saved, "plate_w", "40 mm", UnitKind::Length, "overall width"),
              "plate_w = 40 mm");
        check(AddParameter(saved, "plate_t", "plate_w / 8"), "plate_t = plate_w / 8");
        check(AddParameter(saved, "fillet_r", "1.5 mm"), "fillet_r = 1.5 mm");
        check(AddParameter(saved, "tilt", "30 deg", UnitKind::Angle), "tilt = 30 deg");
        check(AddParameter(saved, "count", "3", UnitKind::Unitless), "count = 3");

        auto sketch = PlateSketch("Sketch1");
        saved.AddFeature(sketch);
        check(Drive(saved, "Sketch1", "d1", "plate_w"), "the width dimension reads plate_w");

        auto extrude = std::make_shared<ExtrudeFeature>("Sketch1", 5.0);
        extrude->SetName("Extrude1");
        extrude->SetProfiles({LargestRegion(*sketch)});
        saved.AddFeature(extrude);
        check(Drive(saved, "Extrude1", "Distance", "plate_t"), "the extrude reads plate_t");

        auto fillet = std::make_shared<FilletFeature>(1.0);
        fillet->SetName("Fillet1");
        saved.AddFeature(fillet);
        check(Drive(saved, "Fillet1", "Radius", "fillet_r"), "the fillet reads fillet_r");

        std::string error;
        check(saved.RenameModelParameter(extrude, "Distance", "thickness", error),
              "the extrude's d# is renamed \"thickness\"");
        saved.Bodies().front()->SetName("Plate");

        // Unfilleted this is (40*20 - 25 pi) * 5 = 3607.3; the fillet
        // takes a little off every edge.
        const double volume = VolumeOf(saved.Shape());
        std::cout << "        plate volume " << volume << std::endl;
        check(volume > 3400.0 && volume < 3607.3, "the plate has the ring's volume less the rounds");

        Document opened;
        RoundTrip(saved, opened, "plate");
        check(opened.Bodies().size() == 1 && opened.Bodies().front()->Name() == "Plate",
              "the renamed body comes back as Plate, not Body1");
        const FeaturePtr reopenedExtrude = Find(opened, "Extrude1");
        check(reopenedExtrude && reopenedExtrude->ModelNameOf("Distance") == "thickness"
                  && reopenedExtrude->ExpressionOf("Distance") == "plate_t",
              "Extrude1's Distance is still thickness = plate_t");
        check(opened.UserParameters().Find("plate_w") != nullptr
                  && opened.UserParameters().Find("plate_w")->comment == "overall width",
              "a user parameter's comment comes back");

        // Undo history is not saved; the next edit starts a fresh one.
        check(Drive(opened, "Fillet1", "Radius", "fillet_r * 2") && opened.CanUndo(),
              "the reopened design edits and undoes like any other");
        opened.Undo();
        check(std::fabs(VolumeOf(opened.Shape()) - volume) < 1.0e-6,
              "undo puts the reopened design back to the saved volume");

        std::string writeError;
        WriteNativeText(saved, DesignExtras(), plateText, writeError);

        // PCAD_DUMP=plate.pcad keeps a copy to look at (docs/FILE_FORMAT.md
        // quotes it).
        if (!qEnvironmentVariableIsEmpty("PCAD_DUMP")) {
            QFile dump(qEnvironmentVariable("PCAD_DUMP"));
            if (dump.open(QIODevice::WriteOnly)) {
                dump.write(plateText);
            }
        }
    }

    // ---- 2. revolve, driven by an angle ----
    std::cout << "\n-- 2. revolve --" << std::endl;
    {
        Document saved;
        check(AddParameter(saved, "sweep_angle", "270 deg", UnitKind::Angle), "sweep_angle = 270 deg");
        auto sketch = std::make_shared<SketchFeature>(SketchFeature::PlaneXZ(), 0.0);
        sketch->SetName("Sketch1");
        sketch->AddRectangle(gp_Pnt2d(10.0, 0.0), gp_Pnt2d(15.0, 10.0));
        saved.AddFeature(sketch);
        auto revolve = std::make_shared<RevolveFeature>("Sketch1", 180.0, RevolveAxis::WorldZ);
        revolve->SetName("Revolve1");
        saved.AddFeature(revolve);
        check(Drive(saved, "Revolve1", "Angle", "sweep_angle"), "the revolve reads sweep_angle");
        // A 270-degree ring: 0.75 * pi * (15^2 - 10^2) * 10 = 2945.2.
        std::cout << "        revolve volume " << VolumeOf(saved.Shape()) << std::endl;
        check(std::fabs(VolumeOf(saved.Shape()) - 0.75 * kPi * 125.0 * 10.0) < 0.5,
              "three quarters of a 10..15 ring, 10 tall");
        Document opened;
        RoundTrip(saved, opened, "revolve");
    }

    // ---- 3. sweep ----
    std::cout << "\n-- 3. sweep --" << std::endl;
    {
        Document saved;
        saved.AddFeature(CircleSketch(SketchFeature::PlaneXZ(), 0.0, gp_Pnt2d(20.0, 0.0), 3.0, "Profile"));
        saved.AddFeature(CircleSketch(SketchFeature::PlaneXY(), 0.0, gp_Pnt2d(0.0, 0.0), 20.0, "Path"));
        auto sweep = std::make_shared<SweepFeature>("Profile", "Path");
        sweep->SetName("Sweep1");
        saved.AddFeature(sweep);
        Document opened;
        RoundTrip(saved, opened, "sweep");
    }

    // ---- 4. loft, one section naming a region of a two-region sketch ----
    std::cout << "\n-- 4. loft --" << std::endl;
    {
        Document saved;
        saved.AddFeature(CircleSketch(SketchFeature::PlaneXY(), 0.0, gp_Pnt2d(0.0, 0.0), 10.0, "Base"));
        auto top = CircleSketch(SketchFeature::PlaneXY(), 20.0, gp_Pnt2d(0.0, 0.0), 5.0, "Top");
        top->AddEntity(SketchEntity::MakeCircle(gp_Pnt2d(40.0, 0.0), 2.0));
        saved.AddFeature(top);
        check(Drive(saved, "Top", "Offset", "20 mm"), "the top sketch sits 20 mm up");

        LoftSection base;
        base.sketch = "Base";
        LoftSection upper;
        upper.sketch = "Top";
        upper.profile = LargestRegion(*top);
        auto loft = std::make_shared<LoftFeature>(std::vector<LoftSection>{base, upper});
        loft->SetName("Loft1");
        saved.AddFeature(loft);
        // A frustum: pi * 20 / 3 * (100 + 50 + 25) = 3665.2.
        std::cout << "        loft volume " << VolumeOf(saved.Shape()) << std::endl;
        check(std::fabs(VolumeOf(saved.Shape()) - kPi * 20.0 / 3.0 * 175.0) < 1.0,
              "a frustum from r10 to r5 over 20 mm");
        Document opened;
        RoundTrip(saved, opened, "loft");
    }

    // ---- 5. the primitives, one suppressed and one past the marker ----
    std::cout << "\n-- 5. primitives, suppression and the timeline marker --" << std::endl;
    {
        Document saved;
        saved.AddFeature(BoxAt(10.0, gp_Pnt(0, 0, 0), "Box1"));
        auto cylinder = std::make_shared<CylinderFeature>(4.0, 12.0);
        cylinder->SetOrigin(gp_Pnt(30, 0, 0));
        cylinder->SetName("Cylinder1");
        saved.AddFeature(cylinder);
        auto sphere = std::make_shared<SphereFeature>(5.0);
        sphere->SetOrigin(gp_Pnt(60, 0, 0));
        sphere->SetName("Sphere1");
        saved.AddFeature(sphere);
        auto cone = std::make_shared<ConeFeature>(5.0, 2.0, 8.0);
        cone->SetOrigin(gp_Pnt(90, 0, 0));
        cone->SetName("Cone1");
        saved.AddFeature(cone);
        auto torus = std::make_shared<TorusFeature>(10.0, 2.0);
        torus->SetOrigin(gp_Pnt(130, 0, 0));
        torus->SetName("Torus1");
        saved.AddFeature(torus);
        // A second box that joins the first, so a boolean op is in play.
        auto joined = BoxAt(6.0, gp_Pnt(7, 0, 0), "Box2");
        Parameter join = Parameter::MakeString("Operation", "Join");
        check(joined->SetParameter(join), "Box2 joins");
        saved.AddFeature(joined);

        sphere->SetSuppressed(true);
        saved.SetRollbackIndex(saved.FeatureCount() - 1);   // Box2 rolled back
        saved.Bodies().back()->SetVisible(false);
        Document opened;
        RoundTrip(saved, opened, "primitives");
        check(opened.Bodies().back()->IsVisible() == false, "a hidden body stays hidden");
        check(opened.RollbackIndex() == 5, "the timeline marker is still before Box2");
    }

    // ---- 6. chamfer and shell ----
    std::cout << "\n-- 6. chamfer and shell --" << std::endl;
    {
        Document saved;
        saved.AddFeature(BoxAt(20.0, gp_Pnt(0, 0, 0), "Box1"));
        auto chamfer = std::make_shared<ChamferFeature>(1.0);
        chamfer->SetName("Chamfer1");
        saved.AddFeature(chamfer);
        Document opened;
        RoundTrip(saved, opened, "chamfer");

        Document shelled;
        shelled.AddFeature(BoxAt(20.0, gp_Pnt(0, 0, 0), "Box1"));
        auto shell = std::make_shared<ShellFeature>(2.0, ShellOpening::Top);
        shell->SetName("Shell1");
        shelled.AddFeature(shell);
        Document opened2;
        RoundTrip(shelled, opened2, "shell");
    }

    // ---- 7. press/pull of a picked face ----
    std::cout << "\n-- 7. press/pull --" << std::endl;
    {
        Document saved;
        saved.AddFeature(BoxAt(10.0, gp_Pnt(0, 0, 0), "Box1"));
        GeometryRef topFace;
        double highest = -1.0e9;
        for (const GeometryRef& face :
             CollectGeometryRefs(*saved.Bodies().front(), EntityType::BRepFace)) {
            if (face.point.Z() > highest) {
                highest = face.point.Z();
                topFace = face;
            }
        }
        auto pull = std::make_shared<PressPullFeature>(topFace, 5.0);
        pull->SetName("PressPull1");
        saved.AddFeature(pull);
        check(std::fabs(VolumeOf(saved.Shape()) - 1500.0) < 1.0e-6,
              "the top face pulled up 5 mm: 10 x 10 x 15");
        Document opened;
        RoundTrip(saved, opened, "press/pull");
    }

    // ---- 8. combine ----
    std::cout << "\n-- 8. combine --" << std::endl;
    {
        Document saved;
        saved.AddFeature(BoxAt(10.0, gp_Pnt(0, 0, 0), "Box1"));
        saved.AddFeature(BoxAt(10.0, gp_Pnt(5, 0, 0), "Box2"));
        auto combine = std::make_shared<CombineFeature>(BodyRef(saved, 0),
                                                        std::vector<CombineBodyRef>{BodyRef(saved, 1)},
                                                        BooleanOp::Cut);
        combine->SetName("Combine1");
        saved.AddFeature(combine);
        check(std::fabs(VolumeOf(saved.Shape()) - 500.0) < 1.0e-6, "Box1 cut by Box2 leaves half");
        Document opened;
        RoundTrip(saved, opened, "combine");
    }

    // ---- 9. the patterns and mirror ----
    std::cout << "\n-- 9. patterns and mirror --" << std::endl;
    {
        Document grid;
        grid.AddFeature(BoxAt(5.0, gp_Pnt(0, 0, 0), "Box1"));
        auto rectangular = std::make_shared<RectangularPatternFeature>(
            std::vector<CombineBodyRef>{BodyRef(grid, 0)}, 3, 10.0, 2, 10.0);
        rectangular->SetName("RectangularPattern1");
        grid.AddFeature(rectangular);
        check(Drive(grid, "RectangularPattern1", "Spacing 1", "12 mm"), "a spacing driven by text");
        check(std::fabs(VolumeOf(grid.Shape()) - 6.0 * 125.0) < 1.0e-6, "six boxes");
        Document opened;
        RoundTrip(grid, opened, "rectangular pattern");

        Document ring;
        ring.AddFeature(BoxAt(5.0, gp_Pnt(30, 0, 0), "Box1"));
        auto circular = std::make_shared<CircularPatternFeature>(
            std::vector<CombineBodyRef>{BodyRef(ring, 0)}, PatternAxis::WorldZ, 4, 360.0);
        circular->SetName("CircularPattern1");
        ring.AddFeature(circular);
        Document opened2;
        RoundTrip(ring, opened2, "circular pattern");

        Document mirrored;
        mirrored.AddFeature(BoxAt(5.0, gp_Pnt(10, 0, 0), "Box1"));
        auto mirror = std::make_shared<MirrorFeature>(std::vector<CombineBodyRef>{BodyRef(mirrored, 0)},
                                                      MirrorPlane::YZ);
        mirror->SetName("Mirror1");
        mirrored.AddFeature(mirror);
        Document opened3;
        RoundTrip(mirrored, opened3, "mirror");
    }

    // ---- 10. move one of two bodies ----
    std::cout << "\n-- 10. move --" << std::endl;
    {
        Document saved;
        saved.AddFeature(BoxAt(10.0, gp_Pnt(0, 0, 0), "Box1"));
        saved.AddFeature(BoxAt(4.0, gp_Pnt(30, 0, 0), "Box2"));
        auto move = std::make_shared<TransformFeature>(0.0, 15.0, 2.5, 0.0, 0.0, 30.0,
                                                       gp_Pnt(32.0, 2.0, 0.0));
        move->SetTarget(BodyRef(saved, 1));
        check(move->SetScale(1.5), "the move scales 1.5x");
        move->SetName("Move1");
        saved.AddFeature(move);
        // No body picked: the whole model moves, and the empty pick has to
        // come back empty rather than as a pick of nothing.
        auto moveAll = std::make_shared<TransformFeature>(-3.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        moveAll->SetName("Move2");
        saved.AddFeature(moveAll);
        Document opened;
        RoundTrip(saved, opened, "move");
        const auto* reopenedAll = dynamic_cast<const TransformFeature*>(Find(opened, "Move2").get());
        check(reopenedAll != nullptr && reopenedAll->Target().IsNull(),
              "a move of the whole model reopens as one");
    }

    // ---- 11. construction geometry, and a sketch on an angled plane ----
    std::cout << "\n-- 11. construction plane, axis, point; a sketch on the plane --" << std::endl;
    {
        Document saved;
        auto plane = std::make_shared<ConstructionPlaneFeature>("XZ", 0.0);
        plane->SetKind(PlaneKind::AtAngle);
        plane->SetAngleDegrees(37.0);
        plane->SetName("Plane1");
        saved.AddFeature(plane);
        auto axis = std::make_shared<ConstructionAxisFeature>();
        axis->SetPoints(gp_Pnt(0, 0, 0), gp_Pnt(1.0 / 3.0, 2.0, 7.0));
        axis->SetName("Axis1");
        saved.AddFeature(axis);
        auto point = std::make_shared<ConstructionPointFeature>(gp_Pnt(0.1, 0.2, 0.3));
        point->SetName("Point1");
        saved.AddFeature(point);

        gp_Ax3 frame;
        check(plane->AsPlane(frame), "the angled plane evaluated");
        auto sketch = std::make_shared<SketchFeature>(frame, 0.0);
        sketch->SetName("Sketch1");
        sketch->AddRectangle(gp_Pnt2d(-5.0, -5.0), gp_Pnt2d(5.0, 5.0));
        saved.AddFeature(sketch);
        auto extrude = std::make_shared<ExtrudeFeature>("Sketch1", 4.0);
        extrude->SetName("Extrude1");
        saved.AddFeature(extrude);
        check(std::fabs(VolumeOf(saved.Shape()) - 400.0) < 1.0e-6, "a 10 x 10 x 4 block, tilted");
        Document opened;
        RoundTrip(saved, opened, "construction");
    }

    // ---- 12. every kind of sketch curve ----
    std::cout << "\n-- 12. every sketch curve kind, an angle dimension, an offset --" << std::endl;
    {
        Document saved;
        auto sketch = std::make_shared<SketchFeature>(SketchFeature::PlaneYZ(), 0.0);
        sketch->SetName("Zoo");
        const int a = sketch->AddEntity(SketchEntity::MakeLine(gp_Pnt2d(0, 0), gp_Pnt2d(10, 0)));
        const int b = sketch->AddEntity(SketchEntity::MakeLine(gp_Pnt2d(0, 0), gp_Pnt2d(10, 10)));
        sketch->AddEntity(SketchEntity::MakeArc(gp_Pnt2d(3, 4), 2.5, 0.1, 2.0));
        sketch->AddEntity(SketchEntity::MakeEllipse(gp_Pnt2d(20, 5), 6.0, 2.0, 0.3));
        sketch->AddEntity(SketchEntity::MakeSpline({gp_Pnt2d(0, 20), gp_Pnt2d(5, 25), gp_Pnt2d(9, 21)}));
        sketch->AddEntity(
            SketchEntity::MakeControlPointSpline({gp_Pnt2d(0, 30), gp_Pnt2d(4, 36), gp_Pnt2d(8, 31)}));
        sketch->AddEntity(SketchEntity::MakeConic(gp_Pnt2d(20, 20), gp_Pnt2d(25, 28), gp_Pnt2d(30, 20), 0.7));
        sketch->AddEntity(SketchEntity::MakePoint(gp_Pnt2d(1.0 / 7.0, -2.0)));
        SketchConstraint angle = Relation(SketchConstraintType::Angle,
                                          Point(a, SketchPointRole::Whole),
                                          Point(b, SketchPointRole::Whole));
        angle.value = kPi / 4.0;
        sketch->AddConstraint(angle);
        saved.AddFeature(sketch);
        check(Drive(saved, "Zoo", "Offset", "2.5 mm"), "the sketch is offset from its plane");
        Document opened;
        RoundTrip(saved, opened, "every curve kind");
    }

    // ---- 13. an imported shape rides along as BRep text ----
    std::cout << "\n-- 13. an imported shape --" << std::endl;
    {
        Document saved;
        TopoDS_Shape block = BRepPrimAPI_MakeBox(gp_Pnt(1.25, -3.5, 0.0), 12.0, 7.0, 3.0).Shape();
        gp_Trsf turn;
        turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0.4);
        block.Move(TopLoc_Location(turn));
        auto imported = std::make_shared<ShapeFeature>(block, "Import");
        imported->SetName("Import1");
        saved.AddFeature(imported);
        auto fillet = std::make_shared<FilletFeature>(0.5);
        fillet->SetName("Fillet1");
        saved.AddFeature(fillet);
        Document opened;
        RoundTrip(saved, opened, "imported shape");
        check(opened.FeatureCount() == 2 && opened.Features()[0]->TypeName() == "Import",
              "the timeline still calls it Import");
    }

    // ---- 14. every type the format knows was exercised ----
    std::cout << "\n-- 14. coverage --" << std::endl;
    {
        bool all = true;
        for (const std::string& type : NativeFeatureTypes()) {
            if (theTypesSeen.count(type) == 0) {
                std::cout << "        never round-tripped: " << type << std::endl;
                all = false;
            }
        }
        check(all, "every feature type the format knows was saved and reopened ("
                       + std::to_string(NativeFeatureTypes().size()) + " types)");
        check(theTypesSeen.size() == NativeFeatureTypes().size(),
              "and nothing was written under a type the format does not list");
    }

    // ---- 15. the file on disk ----
    std::cout << "\n-- 15. files: save, open, and a save that cannot happen --" << std::endl;
    {
        QTemporaryDir dir;
        check(dir.isValid(), "a scratch directory");
        const QString path = dir.filePath("part.pcad");

        Document saved;
        saved.AddFeature(BoxAt(10.0, gp_Pnt(0, 0, 0), "Box1"));
        std::string error;
        check(SaveNativeFile(path, saved, DesignExtras(), error), "saves to part.pcad");
        check(QFile::exists(path), "the file is there");
        check(QDir(dir.path()).entryList(QDir::Files).size() == 1,
              "and nothing else is -- no temporary left beside it");

        Document opened;
        DesignExtras extras;
        check(OpenNativeFile(path, opened, extras, error) && opened.FeatureCount() == 1,
              "opens it again");
        check(std::fabs(VolumeOf(opened.Shape()) - 1000.0) < 1.0e-6, "a 10 mm cube");

        // Atomic: a design that cannot be written must not cost the user
        // the good file already there.
        QFile before(path);
        before.open(QIODevice::ReadOnly);
        const QByteArray good = before.readAll();
        before.close();
        Document unsaveable;
        unsaveable.AddFeature(BoxAt(10.0, gp_Pnt(0, 0, 0), "Box1"));
        auto unlisted = std::make_shared<UnlistedFeature>();
        unlisted->SetName("Mystery1");
        unsaveable.AddFeature(unlisted);
        error.clear();
        check(!SaveNativeFile(path, unsaveable, DesignExtras(), error), "a feature the format "
              "cannot store refuses the save");
        std::cout << "        says: " << error << std::endl;
        QFile after(path);
        after.open(QIODevice::ReadOnly);
        check(after.readAll() == good, "and the file already there is untouched");

        error.clear();
        check(!SaveNativeFile(dir.filePath("no/such/dir/part.pcad"), saved, DesignExtras(), error)
                  && !error.empty(),
              "a folder that is not there is an error, not a crash");
        std::cout << "        says: " << error << std::endl;

        error.clear();
        check(!OpenNativeFile(dir.filePath("missing.pcad"), opened, extras, error) && !error.empty(),
              "opening a file that is not there is an error");
    }

    // ---- 16. the camera and the units ----
    std::cout << "\n-- 16. the view and the default unit --" << std::endl;
    {
        Document saved;
        saved.AddFeature(BoxAt(10.0, gp_Pnt(0, 0, 0), "Box1"));
        DesignExtras extras;
        extras.units = LengthUnit::Inch;
        extras.view.isSet = true;
        extras.view.eye[0] = 100.0;
        extras.view.eye[1] = -80.5;
        extras.view.eye[2] = 60.0;
        extras.view.up[2] = 1.0;
        extras.view.scale = 123.456;
        extras.view.perspective = true;
        extras.view.fieldOfView = 30.0;
        QByteArray text;
        std::string error;
        check(WriteNativeText(saved, extras, text, error), "writes a view and a unit");
        Document::DesignState design;
        DesignExtras back;
        check(ReadNativeText(text, design, back, error), "reads them back");
        check(back.units == LengthUnit::Inch, "the default unit is inches");
        check(back.view.isSet && back.view.eye[1] == -80.5 && back.view.scale == 123.456
                  && back.view.perspective && back.view.fieldOfView == 30.0,
              "the camera comes back exactly");
    }

    // ---- 17. numbers do not depend on where the user lives ----
    std::cout << "\n-- 17. a decimal-comma locale --" << std::endl;
    {
        const char* comma = nullptr;
        for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_AT.utf8", "fr_FR.UTF-8"}) {
            if (std::setlocale(LC_NUMERIC, name) != nullptr) {
                comma = name;
                break;
            }
        }
        if (comma == nullptr) {
            std::cout << "  SKIPPED  no decimal-comma locale installed" << std::endl;
        } else {
            char probe[16];
            std::snprintf(probe, sizeof(probe), "%.1f", 1.5);
            check(std::string(probe) == "1,5", std::string("under ") + comma + " printf writes 1,5");
            // The combine's body picks are spelled with printf.
            Document saved;
            saved.AddFeature(BoxAt(10.0, gp_Pnt(0, 0, 0), "Box1"));
            saved.AddFeature(BoxAt(10.0, gp_Pnt(5.5, 0, 0), "Box2"));
            auto combine = std::make_shared<CombineFeature>(
                BodyRef(saved, 0), std::vector<CombineBodyRef>{BodyRef(saved, 1)}, BooleanOp::Join);
            combine->SetName("Combine1");
            saved.AddFeature(combine);
            QByteArray underComma;
            std::string error;
            WriteNativeText(saved, DesignExtras(), underComma, error);
            std::setlocale(LC_NUMERIC, "C");
            QByteArray underC;
            WriteNativeText(saved, DesignExtras(), underC, error);
            check(underComma == underC, "the same design writes the same bytes in either locale");
            check(underComma.contains("Body2|10.5,5,5|1000"), "Box2's centre is spelled 10.5, not 10,5");

            std::setlocale(LC_NUMERIC, comma);
            Document opened;
            DesignExtras extras;
            Document::DesignState design;
            check(ReadNativeText(underC, design, extras, error)
                      && opened.ReplaceDesign(std::move(design), error)
                      && std::fabs(VolumeOf(opened.Shape()) - 1550.0) < 1.0e-6,
                  "and reads back under the comma locale to the same joined volume");
            std::setlocale(LC_NUMERIC, "C");
        }
    }

    // ---- 18. a reopened design is rebuilt, not trusted ----
    std::cout << "\n-- 18. rebuilt from the recipe, not from stored results --" << std::endl;
    {
        // plate_w edited by hand from 40 to 50: the sketch dimension that
        // reads it re-solves and the extrude follows, which only happens if
        // opening really rebuilds from the recipe.
        const QByteArray wider = Edited(plateText, "\"expression\": \"40 mm\"", "\"expression\": \"50 mm\"");
        Document opened;
        Document::DesignState design;
        DesignExtras extras;
        std::string error;
        check(ReadNativeText(wider, design, extras, error)
                  && opened.ReplaceDesign(std::move(design), error),
              "a hand-edited plate_w opens");
        const FeaturePtr sketch = Find(opened, "Sketch1");
        check(sketch && std::fabs(ParameterOf(sketch, "d1").doubleValue - 50.0) < 1.0e-9,
              "the width dimension now reads 50");
        const FeaturePtr extrude = Find(opened, "Extrude1");
        check(extrude && std::fabs(ParameterOf(extrude, "Distance").doubleValue - 6.25) < 1.0e-9,
              "and the extrude 50 / 8 = 6.25");
        std::cout << "        volume " << VolumeOf(opened.Shape()) << std::endl;
        check(VolumeOf(opened.Shape()) > 4800.0, "the plate grew: (50*20 - 25 pi) * 6.25 less rounds");
        for (const std::string& message : opened.Errors()) {
            std::cout << "        " << message << std::endl;
        }
        check(opened.Errors().empty(), "and built cleanly");
    }

    // ---- 19. refusals leave the open design alone ----
    std::cout << "\n-- 19. files this version will not open --" << std::endl;
    {
        Document current;
        current.AddFeature(BoxAt(10.0, gp_Pnt(0, 0, 0), "Box1"));
        current.AddFeature(BoxAt(3.0, gp_Pnt(20, 0, 0), "Box2"));
        const std::size_t count = current.FeatureCount();
        const double volume = VolumeOf(current.Shape());

        const auto refuse = [&](const QByteArray& theText, const std::string& theWhat,
                                const std::string& theMustSay) {
            Document::DesignState design;
            DesignExtras extras;
            std::string error;
            bool opened = ReadNativeText(theText, design, extras, error);
            if (opened) {
                opened = current.ReplaceDesign(std::move(design), error);
            }
            std::cout << "        says: " << error << std::endl;
            check(!opened && error.find(theMustSay) != std::string::npos,
                  theWhat + " is refused, saying \"" + theMustSay + "\"");
            check(current.FeatureCount() == count && VolumeOf(current.Shape()) == volume
                      && current.CanUndo(),
                  theWhat + ": the open design is exactly as it was, undo and all");
        };

        refuse(Edited(plateText, "\"version\": 1", "\"version\": 2"), "a newer version", "newer version");
        refuse(Edited(plateText, "\"type\": \"Fillet\"", "\"type\": \"Hole\""), "an unknown type",
               "\"Hole\" is not a kind of feature");
        refuse(plateText.left(plateText.size() / 2), "a file cut off halfway", "not valid JSON");
        refuse(QByteArray("{\"format\": \"somethingelse\"}"), "another program's JSON",
               "not a PenguinCAD design");
        refuse(QByteArray("\x89PNG\r\n"), "a picture", "not valid JSON");
        refuse(Edited(plateText, "\"suppressed\": false", "\"suppressed\": false, \"colour\": \"red\""),
               "a key this version does not know", "unknown key \"colour\"");
        refuse(Edited(plateText, "\"expression\": \"40 mm\"", "\"expression\": \"plate_t * 8\""),
               "user parameters that read each other in a loop", "user parameter \"plate_t\"");
        refuse(Edited(plateText, "\"Distance\": 5", "\"Distance\": \"five\""), "a string where a number goes",
               "must be a number");
        refuse(Edited(plateText, "\"units\": \"mm\"", "\"units\": \"furlong\""), "an unknown unit",
               "\"furlong\" is not a length unit");
        refuse(Edited(plateText, "\"entity\": 5,", "\"entity\": 55,"), "a constraint on a curve that is not there",
               "names curve 55");
        refuse(Edited(plateText, "\"label\": \"d2\"", "\"label\": \"d1\""), "two dimensions with one label",
               "two dimensions are both called d1");
    }

    std::cout << std::endl;
    if (failures == 0) {
        std::cout << "All save/open tests passed." << std::endl;
        return 0;
    }
    std::cout << failures << " save/open test(s) FAILED." << std::endl;
    return 1;
}
