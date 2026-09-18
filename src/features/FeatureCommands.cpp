#include "core/Command.h"
#include "core/Document.h"
#include "core/ProfileProvider.h"
#include "core/Origin.h"
#include "core/ProfileSelection.h"
#include "core/Registration.h"
#include "features/FeatureDialogs.h"
#include "features/FeatureUtils.h"
#include "features/ModifyFeatures.h"
#include "features/PrimitiveFeatures.h"
#include "features/ConstructionFeatures.h"
#include "features/ProfileFeatures.h"

#include <gp_Pnt.hxx>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>
#include <QStringList>

namespace lcad {

namespace {

const char* const kSolidGroup  = "Solid";
const char* const kModifyGroup = "Modify";

// Placement fields have to accept negative coordinates; dimension fields
// deliberately don't.
constexpr double kAnyNumber = -1.0e6;

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

QStringList ToChoices(const std::vector<std::string>& theNames)
{
    QStringList choices;
    for (const std::string& name : theNames) {
        choices << QString::fromStdString(name);
    }
    return choices;
}

// Sketches are found by type name, not by class: the solid subsystem
// deliberately knows nothing about SketchFeature's header.
std::vector<std::string> SketchNames(const Document* theDocument)
{
    std::vector<std::string> names;
    if (theDocument == nullptr) {
        return names;
    }
    for (const FeaturePtr& feature : theDocument->Features()) {
        if (feature && feature->TypeName() == "Sketch") {
            names.push_back(feature->Name());
        }
    }
    return names;
}

// True when the named sketch actually has something to sweep. Checked
// before the feature is created so the usual "I forgot to close the
// loop" mistake doesn't leave a broken entry in the timeline.
bool SketchHasProfile(Document* theDocument, const std::string& theName)
{
    if (theDocument == nullptr) {
        return false;
    }
    for (const FeaturePtr& feature : theDocument->Features()) {
        if (!feature || feature->Name() != theName) {
            continue;
        }
        ProfileProvider* provider = AsProfileProvider(feature.get());
        return provider != nullptr && !provider->ProfileFaces().empty();
    }
    return false;
}

// True when the user has already clicked one or more regions of a sketch
// that is still in this document. A selection left over from a sketch the
// user has since deleted must not steer the dialog.
bool HasPickedProfiles(const CommandContext& theContext)
{
    const ProfileSelection& selection = ProfileSelection::Instance();
    if (selection.IsEmpty() || selection.SketchName().empty()) {
        return false;
    }
    const std::vector<std::string> names = SketchNames(theContext.document);
    return std::find(names.begin(), names.end(), selection.SketchName()) != names.end();
}

// Everything a construction feature can be built from: the origin
// entities of that kind, then any construction geometry already in the
// timeline. Origin first because it is always there and is what a user
// reaches for by default.
std::vector<std::string> ConstructionNames(const Document* theDocument, EntityType theType)
{
    std::vector<std::string> names;
    for (const OriginEntity& entity : OriginEntities()) {
        if (entity.type == theType) {
            names.push_back(entity.name);
        }
    }
    if (theDocument == nullptr) {
        return names;
    }

    const std::string wanted = EntityTypeName(theType);
    for (const FeaturePtr& feature : theDocument->Features()) {
        if (feature && feature->TypeName() == wanted) {
            names.push_back(feature->Name());
        }
    }
    return names;
}

// Construction features are named the way Fusion names them -- "Plane1",
// "Axis1", "Point1" -- rather than after their type. Their TypeName stays
// the full API name ("ConstructionPlane"), which is what the browser's
// type column and any future file format want; this is only what the user
// reads and types.
void NameAsFusionWould(const CommandContext& theContext,
                       const FeaturePtr&     theFeature,
                       const std::string&    theBase)
{
    if (theContext.document != nullptr && theFeature) {
        theFeature->SetName(theContext.document->MakeUniqueName(theBase));
    }
}

bool DocumentHasBody(const CommandContext& theContext)
{
    return theContext.document != nullptr && HasSolid(theContext.document->Shape());
}

// Fusion's default: add to the existing body if there is one, otherwise
// start a new one.
int DefaultOperationChoice(const CommandContext& theContext)
{
    return DocumentHasBody(theContext) ? static_cast<int>(BooleanOp::Join)
                                       : static_cast<int>(BooleanOp::NewBody);
}

// Shared tail of every primitive dialog: where the primitive sits and how
// it combines with what's already in the document.
void AppendPlacementFields(std::vector<DialogField>& theFields, int theOperationChoice)
{
    theFields.push_back(DialogField::Number("Origin X", 0.0, kAnyNumber));
    theFields.push_back(DialogField::Number("Origin Y", 0.0, kAnyNumber));
    theFields.push_back(DialogField::Number("Origin Z", 0.0, kAnyNumber));
    theFields.push_back(DialogField::Choice("Operation",
                                            ToChoices(BooleanOpNames()),
                                            theOperationChoice));
}

// Reads that tail back. theFirst is the index of the "Origin X" row.
void ApplyPlacement(const std::vector<DialogField>& theFields,
                    std::size_t                     theFirst,
                    PrimitiveFeature&               theFeature)
{
    if (theFirst + 3 >= theFields.size()) {
        return;
    }
    theFeature.SetOrigin(gp_Pnt(theFields[theFirst].value,
                                theFields[theFirst + 1].value,
                                theFields[theFirst + 2].value));
    theFeature.SetOperation(BooleanOpFromInt(theFields[theFirst + 3].choice));
}

// Adds the feature and reports what the rebuild made of it. A feature
// that failed stays in the timeline on purpose -- the properties panel
// is where the user fixes the number that was wrong.
void AddAndReport(CommandContext& theContext, const FeaturePtr& theFeature)
{
    if (theContext.document == nullptr || !theFeature) {
        return;
    }

    theContext.document->AddFeature(theFeature);
    theContext.document->SetActiveFeature(theFeature);

    if (!theFeature->LastError().empty()) {
        // Already carries the feature name, courtesy of Document::Rebuild.
        ShowStatus(theContext, QString::fromStdString(theFeature->LastError()));
        return;
    }
    ShowStatus(theContext, QString::fromStdString(theFeature->Name()) + " created.");
}

// ---- primitives ----

class SolidCommand : public Command
{
public:
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return "Create"; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return theContext.document != nullptr;
    }
};

class BoxCommand : public SolidCommand
{
public:
    std::string Id() const override { return "solid.box"; }
    std::string Title() const override { return "Box"; }
    std::string Icon() const override { return "📦"; }
    std::string Shortcut() const override { return "B"; }
    std::string Description() const override
    {
        return "Create a rectangular box from its length, width and height";
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("Length", 20.0));
        fields.push_back(DialogField::Number("Width", 20.0));
        fields.push_back(DialogField::Number("Height", 20.0));
        AppendPlacementFields(fields, DefaultOperationChoice(theContext));

        if (!ShowFeatureDialog(theContext.parent, "Box", fields,
                               "The origin is the box's minimum corner.")) {
            return;
        }

        auto box = std::make_shared<BoxFeature>(fields[0].value, fields[1].value, fields[2].value);
        ApplyPlacement(fields, 3, *box);
        AddAndReport(theContext, box);
    }
};

class CylinderCommand : public SolidCommand
{
public:
    std::string Id() const override { return "solid.cylinder"; }
    std::string Title() const override { return "Cylinder"; }
    std::string Icon() const override { return "🛢"; }
    std::string Shortcut() const override { return "Y"; }
    std::string Description() const override
    {
        return "Create a cylinder from its radius and height";
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("Radius", 10.0));
        fields.push_back(DialogField::Number("Height", 20.0, kAnyNumber));
        AppendPlacementFields(fields, DefaultOperationChoice(theContext));

        if (!ShowFeatureDialog(theContext.parent, "Cylinder", fields,
                               "A negative height builds downwards, which is what a "
                               "drilled hole usually wants.")) {
            return;
        }

        auto cylinder = std::make_shared<CylinderFeature>(fields[0].value, fields[1].value);
        ApplyPlacement(fields, 2, *cylinder);
        AddAndReport(theContext, cylinder);
    }
};

class SphereCommand : public SolidCommand
{
public:
    std::string Id() const override { return "solid.sphere"; }
    std::string Title() const override { return "Sphere"; }
    std::string Icon() const override { return "🔮"; }
    std::string Shortcut() const override { return "Shift+S"; }
    std::string Description() const override { return "Create a sphere from its radius"; }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("Radius", 10.0));
        AppendPlacementFields(fields, DefaultOperationChoice(theContext));

        if (!ShowFeatureDialog(theContext.parent, "Sphere", fields,
                               "The origin is the sphere's centre.")) {
            return;
        }

        auto sphere = std::make_shared<SphereFeature>(fields[0].value);
        ApplyPlacement(fields, 1, *sphere);
        AddAndReport(theContext, sphere);
    }
};

class ConeCommand : public SolidCommand
{
public:
    std::string Id() const override { return "solid.cone"; }
    std::string Title() const override { return "Cone"; }
    std::string Icon() const override { return "🔺"; }
    std::string Shortcut() const override { return "K"; }
    std::string Description() const override
    {
        return "Create a cone or truncated cone from its bottom radius, top radius and height";
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("Bottom Radius", 10.0));
        fields.push_back(DialogField::Number("Top Radius", 0.0, 0.0));
        fields.push_back(DialogField::Number("Height", 20.0, kAnyNumber));
        AppendPlacementFields(fields, DefaultOperationChoice(theContext));

        if (!ShowFeatureDialog(theContext.parent, "Cone", fields,
                               "A top radius of zero gives a pointed cone.")) {
            return;
        }

        auto cone = std::make_shared<ConeFeature>(fields[0].value, fields[1].value,
                                                  fields[2].value);
        ApplyPlacement(fields, 3, *cone);
        AddAndReport(theContext, cone);
    }
};

class TorusCommand : public SolidCommand
{
public:
    std::string Id() const override { return "solid.torus"; }
    std::string Title() const override { return "Torus"; }
    std::string Icon() const override { return "🍩"; }
    std::string Shortcut() const override { return "Shift+T"; }
    std::string Description() const override
    {
        return "Create a torus from its major (ring) and minor (tube) radius";
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("Major Radius", 20.0));
        fields.push_back(DialogField::Number("Minor Radius", 5.0));
        AppendPlacementFields(fields, DefaultOperationChoice(theContext));

        if (!ShowFeatureDialog(theContext.parent, "Torus", fields,
                               "The minor radius must stay smaller than the major one.")) {
            return;
        }

        auto torus = std::make_shared<TorusFeature>(fields[0].value, fields[1].value);
        ApplyPlacement(fields, 2, *torus);
        AddAndReport(theContext, torus);
    }
};

// ---- sketch-based features ----

class ProfileCommand : public SolidCommand
{
public:
    bool IsEnabled(const CommandContext& theContext) const override
    {
        return !SketchNames(theContext.document).empty();
    }

protected:
    // Runs the dialog with a sketch dropdown prepended, defaulted to the
    // newest sketch -- nearly always the one just drawn. The row is
    // INSERTED AT THE FRONT, so the caller's own rows start at index 1.
    static bool AskForSketch(CommandContext&           theContext,
                             const QString&            theTitle,
                             std::vector<DialogField>& theFields,
                             const QString&            theHint,
                             std::string&              theSketchName)
    {
        const std::vector<std::string> names = SketchNames(theContext.document);
        if (names.empty()) {
            ShowStatus(theContext, "Create a sketch first.");
            return false;
        }

        theFields.insert(theFields.begin(),
                         DialogField::Choice("Sketch",
                                             ToChoices(names),
                                             static_cast<int>(names.size()) - 1));

        if (!ShowFeatureDialog(theContext.parent, theTitle, theFields, theHint)) {
            return false;
        }

        const int choice = theFields.front().choice;
        if (choice < 0 || choice >= static_cast<int>(names.size())) {
            return false;
        }
        theSketchName = names[static_cast<std::size_t>(choice)];

        if (!SketchHasProfile(theContext.document, theSketchName)) {
            ShowStatus(theContext,
                       QString::fromStdString(theSketchName)
                           + " has no closed profile -- close the loop first.");
            return false;
        }
        return true;
    }

    // What this feature should act on: the regions the user has already
    // clicked, or -- when nothing is picked -- a whole sketch chosen from
    // the dropdown, which is what this command has always done.
    //
    // Fusion's flow is to click the region and then press E, so a pick has
    // to make the dialog be about that pick rather than ask the question
    // again. The row is still inserted either way so the caller's own
    // fields keep the same indices whichever path ran; when a pick exists
    // the row confirms the target instead of offering a choice, which is
    // the job Fusion's "Profile: 1 selected" row does.
    static bool AskForTarget(CommandContext&           theContext,
                             const QString&            theTitle,
                             std::vector<DialogField>& theFields,
                             const QString&            theHint,
                             std::string&              theSketchName,
                             std::vector<ProfileRef>&  theProfiles)
    {
        theProfiles.clear();

        if (!HasPickedProfiles(theContext)) {
            return AskForSketch(theContext, theTitle, theFields, theHint, theSketchName);
        }

        const ProfileSelection& selection = ProfileSelection::Instance();
        const int count = static_cast<int>(selection.Count());
        const QString target = QString::fromStdString(selection.SketchName())
                             + QString(" (%1 profile%2)").arg(count).arg(count == 1 ? "" : "s");
        theFields.insert(theFields.begin(),
                         DialogField::Choice("Profile", QStringList{target}, 0));

        if (!ShowFeatureDialog(theContext.parent, theTitle, theFields, theHint)) {
            return false;
        }

        theSketchName = selection.SketchName();
        theProfiles = selection.Items();
        return true;
    }

    // A profile is consumed once it has been built on. Leaving it picked
    // would make a second press of E silently build the same thing again,
    // and the highlight would go on claiming a region the user has already
    // spent.
    static void ClearPickedProfiles() { ProfileSelection::Instance().Clear(); }
};

class ExtrudeCommand : public ProfileCommand
{
public:
    std::string Id() const override { return "solid.extrude"; }
    std::string Title() const override { return "Extrude"; }
    std::string Icon() const override { return "⬆"; }
    std::string Shortcut() const override { return "E"; }
    std::string Description() const override
    {
        return "Push a closed sketch profile along its normal into a solid";
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("Distance", 10.0));
        fields.push_back(DialogField::Toggle("Reversed", false));
        fields.push_back(DialogField::Toggle("Symmetric", false));
        fields.push_back(DialogField::Choice("Operation",
                                             ToChoices(BooleanOpNames()),
                                             DefaultOperationChoice(theContext)));

        std::string             sketchName;
        std::vector<ProfileRef> profiles;
        if (!AskForTarget(theContext, "Extrude", fields,
                          "Symmetric splits the distance either side of the sketch plane.",
                          sketchName, profiles)) {
            return;
        }

        auto extrude = std::make_shared<ExtrudeFeature>(sketchName, fields[1].value);
        extrude->SetProfiles(profiles);
        extrude->SetReversed(fields[2].toggle);
        extrude->SetSymmetric(fields[3].toggle);
        extrude->SetOperation(BooleanOpFromInt(fields[4].choice));
        AddAndReport(theContext, extrude);
        ClearPickedProfiles();
    }
};

class RevolveCommand : public ProfileCommand
{
public:
    std::string Id() const override { return "solid.revolve"; }
    std::string Title() const override { return "Revolve"; }
    std::string Icon() const override { return "🔄"; }
    std::string Shortcut() const override { return "Shift+R"; }
    std::string Description() const override
    {
        return "Turn a closed sketch profile about an axis into a solid";
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("Angle", 360.0, 0.001, " °"));
        fields.back().maximum = 360.0;
        fields.push_back(DialogField::Choice("Axis",
                                             ToChoices(RevolveAxisNames()),
                                             static_cast<int>(RevolveAxis::SketchY)));
        fields.push_back(DialogField::Toggle("Reversed", false));
        fields.push_back(DialogField::Choice("Operation",
                                             ToChoices(BooleanOpNames()),
                                             DefaultOperationChoice(theContext)));

        std::string             sketchName;
        std::vector<ProfileRef> profiles;
        if (!AskForTarget(theContext, "Revolve", fields,
                          "The axis must not cross the profile -- draw the profile to one "
                          "side of it.",
                          sketchName, profiles)) {
            return;
        }

        auto revolve = std::make_shared<RevolveFeature>(
            sketchName, fields[1].value, RevolveAxisFromInt(fields[2].choice));
        revolve->SetProfiles(profiles);
        revolve->SetReversed(fields[3].toggle);
        revolve->SetOperation(BooleanOpFromInt(fields[4].choice));
        AddAndReport(theContext, revolve);
        ClearPickedProfiles();
    }
};

// ---- modify features ----

class ModifyCommand : public Command
{
public:
    // Fusion keeps Modify as a section of the Solid tab, not a tab of its
    // own, so these sit beside the creation tools rather than elsewhere.
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return kModifyGroup; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return DocumentHasBody(theContext);
    }
};

class FilletCommand : public ModifyCommand
{
public:
    std::string Id() const override { return "modify.fillet"; }
    std::string Title() const override { return "Fillet"; }
    std::string Icon() const override { return "⚪"; }
    std::string Shortcut() const override { return "F"; }
    std::string Description() const override { return "Round every edge of the body"; }

    void Execute(CommandContext& theContext) override
    {
        if (!DocumentHasBody(theContext)) {
            ShowStatus(theContext, "Create a body first.");
            return;
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("Radius", 2.0));

        if (!ShowFeatureDialog(theContext.parent, "Fillet", fields,
                               "Applied to every edge. A radius the geometry can't take "
                               "is reported rather than applied.")) {
            return;
        }

        AddAndReport(theContext, std::make_shared<FilletFeature>(fields[0].value));
    }
};

class ChamferCommand : public ModifyCommand
{
public:
    std::string Id() const override { return "modify.chamfer"; }
    std::string Title() const override { return "Chamfer"; }
    std::string Icon() const override { return "🔶"; }
    std::string Shortcut() const override { return "Shift+C"; }
    std::string Description() const override { return "Bevel every edge of the body"; }

    void Execute(CommandContext& theContext) override
    {
        if (!DocumentHasBody(theContext)) {
            ShowStatus(theContext, "Create a body first.");
            return;
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("Distance", 1.0));

        if (!ShowFeatureDialog(theContext.parent, "Chamfer", fields,
                               "Applied to every edge, equally on both adjoining faces.")) {
            return;
        }

        AddAndReport(theContext, std::make_shared<ChamferFeature>(fields[0].value));
    }
};

class ShellCommand : public ModifyCommand
{
public:
    std::string Id() const override { return "modify.shell"; }
    std::string Title() const override { return "Shell"; }
    std::string Icon() const override { return "🥣"; }
    std::string Shortcut() const override { return "Shift+H"; }
    std::string Description() const override
    {
        return "Hollow the body out to a wall thickness, optionally opening one face";
    }

    void Execute(CommandContext& theContext) override
    {
        if (!DocumentHasBody(theContext)) {
            ShowStatus(theContext, "Create a body first.");
            return;
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("Thickness", 2.0));
        fields.push_back(DialogField::Choice("Open Face",
                                             ToChoices(ShellOpeningNames()),
                                             static_cast<int>(ShellOpening::Top)));
        fields.push_back(DialogField::Toggle("Outward", false));

        if (!ShowFeatureDialog(theContext.parent, "Shell", fields,
                               "The open face is re-picked on every rebuild, so it follows "
                               "the body when upstream dimensions change.")) {
            return;
        }

        auto shell = std::make_shared<ShellFeature>(fields[0].value,
                                                    ShellOpeningFromInt(fields[1].choice));
        shell->SetOutward(fields[2].toggle);
        AddAndReport(theContext, shell);
    }
};

} // namespace

// ---- CONSTRUCT ----
//
// Fusion's CONSTRUCT panel, in the Solid tab. Only the fully parametric
// ways of building each one are offered: the versions that start from a
// picked face or edge need selection plumbing that does not exist yet,
// and a menu entry that cannot work is worse than one that is missing.

class ConstructCommand : public Command
{
public:
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return "Construct"; }
};

class OffsetPlaneCommand : public ConstructCommand
{
public:
    std::string Id() const override { return "construct.plane_offset"; }
    std::string Title() const override { return "Offset Plane"; }
    std::string Icon() const override { return "▱"; }
    std::string Description() const override
    {
        return "A plane parallel to another, a set distance away";
    }

    void Execute(CommandContext& theContext) override
    {
        const std::vector<std::string> planes =
            ConstructionNames(theContext.document, EntityType::ConstructionPlane);

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Choice("Plane", ToChoices(planes), 0));
        fields.push_back(DialogField::Number("Distance", 10.0, kAnyNumber));
        if (!ShowFeatureDialog(theContext.parent, "Offset Plane", fields)) {
            return;
        }

        auto plane = std::make_shared<ConstructionPlaneFeature>(
            planes[static_cast<std::size_t>(fields[0].choice)], fields[1].value);
        NameAsFusionWould(theContext, plane, "Plane");
        AddAndReport(theContext, plane);
    }
};

class AnglePlaneCommand : public ConstructCommand
{
public:
    std::string Id() const override { return "construct.plane_angle"; }
    std::string Title() const override { return "Plane at Angle"; }
    std::string Icon() const override { return "◪"; }
    std::string Description() const override
    {
        return "A plane turned about another plane's own X axis";
    }

    void Execute(CommandContext& theContext) override
    {
        const std::vector<std::string> planes =
            ConstructionNames(theContext.document, EntityType::ConstructionPlane);

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Choice("Plane", ToChoices(planes), 0));
        fields.push_back(DialogField::Number("Angle", 45.0, kAnyNumber, " °"));
        if (!ShowFeatureDialog(theContext.parent, "Plane at Angle", fields)) {
            return;
        }

        auto plane = std::make_shared<ConstructionPlaneFeature>();
        plane->SetKind(PlaneKind::AtAngle);
        plane->SetBasePlane(planes[static_cast<std::size_t>(fields[0].choice)]);
        plane->SetAngleDegrees(fields[1].value);
        NameAsFusionWould(theContext, plane, "Plane");
        AddAndReport(theContext, plane);
    }
};

class MidplaneCommand : public ConstructCommand
{
public:
    std::string Id() const override { return "construct.plane_midplane"; }
    std::string Title() const override { return "Midplane"; }
    std::string Icon() const override { return "⬓"; }
    std::string Description() const override { return "Halfway between two parallel planes"; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        // Two planes to sit between, and the three origin planes are never
        // parallel to each other -- so this needs a construction plane.
        return ConstructionNames(theContext.document, EntityType::ConstructionPlane).size() > 3;
    }

    void Execute(CommandContext& theContext) override
    {
        const std::vector<std::string> planes =
            ConstructionNames(theContext.document, EntityType::ConstructionPlane);

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Choice("First Plane", ToChoices(planes), 0));
        fields.push_back(DialogField::Choice("Second Plane", ToChoices(planes),
                                             static_cast<int>(planes.size()) - 1));
        if (!ShowFeatureDialog(theContext.parent, "Midplane", fields,
                               "The two planes must be parallel.")) {
            return;
        }

        auto plane = std::make_shared<ConstructionPlaneFeature>();
        plane->SetKind(PlaneKind::Midplane);
        plane->SetBasePlane(planes[static_cast<std::size_t>(fields[0].choice)]);
        plane->SetSecondPlane(planes[static_cast<std::size_t>(fields[1].choice)]);
        NameAsFusionWould(theContext, plane, "Plane");
        AddAndReport(theContext, plane);
    }
};

class ThreePointPlaneCommand : public ConstructCommand
{
public:
    std::string Id() const override { return "construct.plane_three_points"; }
    std::string Title() const override { return "Plane Through Three Points"; }
    std::string Icon() const override { return "◺"; }
    std::string Description() const override { return "A plane through three points"; }

    void Execute(CommandContext& theContext) override
    {
        std::vector<DialogField> fields;
        const char* const labels[] = {"A X", "A Y", "A Z", "B X", "B Y", "B Z",
                                      "C X", "C Y", "C Z"};
        const double defaults[] = {0, 0, 0, 10, 0, 0, 0, 10, 0};
        for (int i = 0; i < 9; ++i) {
            fields.push_back(DialogField::Number(labels[i], defaults[i], kAnyNumber));
        }
        if (!ShowFeatureDialog(theContext.parent, "Plane Through Three Points", fields,
                               "The three points must not be in a line.")) {
            return;
        }

        auto plane = std::make_shared<ConstructionPlaneFeature>();
        plane->SetKind(PlaneKind::ThreePoints);
        plane->SetPoints(gp_Pnt(fields[0].value, fields[1].value, fields[2].value),
                         gp_Pnt(fields[3].value, fields[4].value, fields[5].value),
                         gp_Pnt(fields[6].value, fields[7].value, fields[8].value));
        NameAsFusionWould(theContext, plane, "Plane");
        AddAndReport(theContext, plane);
    }
};

class TwoPointAxisCommand : public ConstructCommand
{
public:
    std::string Id() const override { return "construct.axis_two_points"; }
    std::string Title() const override { return "Axis Through Two Points"; }
    std::string Icon() const override { return "╱"; }
    std::string Description() const override { return "A construction axis through two points"; }

    void Execute(CommandContext& theContext) override
    {
        std::vector<DialogField> fields;
        const char* const labels[] = {"From X", "From Y", "From Z", "To X", "To Y", "To Z"};
        const double defaults[] = {0, 0, 0, 0, 0, 10};
        for (int i = 0; i < 6; ++i) {
            fields.push_back(DialogField::Number(labels[i], defaults[i], kAnyNumber));
        }
        if (!ShowFeatureDialog(theContext.parent, "Axis Through Two Points", fields)) {
            return;
        }

        auto axis = std::make_shared<ConstructionAxisFeature>();
        axis->SetKind(AxisKind::TwoPoints);
        axis->SetPoints(gp_Pnt(fields[0].value, fields[1].value, fields[2].value),
                        gp_Pnt(fields[3].value, fields[4].value, fields[5].value));
        NameAsFusionWould(theContext, axis, "Axis");
        AddAndReport(theContext, axis);
    }
};

class NormalAxisCommand : public ConstructCommand
{
public:
    std::string Id() const override { return "construct.axis_perpendicular"; }
    std::string Title() const override { return "Axis Perpendicular to Plane"; }
    std::string Icon() const override { return "⊥"; }
    std::string Description() const override
    {
        return "A construction axis along a plane's normal";
    }

    void Execute(CommandContext& theContext) override
    {
        const std::vector<std::string> planes =
            ConstructionNames(theContext.document, EntityType::ConstructionPlane);

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Choice("Plane", ToChoices(planes), 0));
        if (!ShowFeatureDialog(theContext.parent, "Axis Perpendicular to Plane", fields)) {
            return;
        }

        auto axis = std::make_shared<ConstructionAxisFeature>();
        axis->SetKind(AxisKind::PerpendicularToPlane);
        axis->SetBasePlane(planes[static_cast<std::size_t>(fields[0].choice)]);
        NameAsFusionWould(theContext, axis, "Axis");
        AddAndReport(theContext, axis);
    }
};

class CoordinatePointCommand : public ConstructCommand
{
public:
    std::string Id() const override { return "construct.point_coordinates"; }
    std::string Title() const override { return "Point at Coordinates"; }
    std::string Icon() const override { return "•"; }
    std::string Description() const override { return "A construction point at a typed position"; }

    void Execute(CommandContext& theContext) override
    {
        std::vector<DialogField> fields;
        fields.push_back(DialogField::Number("X", 0.0, kAnyNumber));
        fields.push_back(DialogField::Number("Y", 0.0, kAnyNumber));
        fields.push_back(DialogField::Number("Z", 0.0, kAnyNumber));
        if (!ShowFeatureDialog(theContext.parent, "Point at Coordinates", fields)) {
            return;
        }

        auto point = std::make_shared<ConstructionPointFeature>(
            gp_Pnt(fields[0].value, fields[1].value, fields[2].value));
        NameAsFusionWould(theContext, point, "Point");
        AddAndReport(theContext, point);
    }
};

class AxisPlanePointCommand : public ConstructCommand
{
public:
    std::string Id() const override { return "construct.point_axis_plane"; }
    std::string Title() const override { return "Point at Axis and Plane"; }
    std::string Icon() const override { return "✛"; }
    std::string Description() const override { return "Where an axis crosses a plane"; }

    void Execute(CommandContext& theContext) override
    {
        const std::vector<std::string> axes =
            ConstructionNames(theContext.document, EntityType::ConstructionAxis);
        const std::vector<std::string> planes =
            ConstructionNames(theContext.document, EntityType::ConstructionPlane);

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Choice("Axis", ToChoices(axes), 2));
        fields.push_back(DialogField::Choice("Plane", ToChoices(planes), 0));
        if (!ShowFeatureDialog(theContext.parent, "Point at Axis and Plane", fields,
                               "The axis must not lie in the plane.")) {
            return;
        }

        auto point = std::make_shared<ConstructionPointFeature>();
        point->SetKind(PointKind::AxisPlaneIntersection);
        point->SetAxisName(axes[static_cast<std::size_t>(fields[0].choice)]);
        point->SetPlaneName(planes[static_cast<std::size_t>(fields[1].choice)]);
        NameAsFusionWould(theContext, point, "Point");
        AddAndReport(theContext, point);
    }
};

void RegisterFeatureCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<BoxCommand>());
    theRegistry.Add(std::make_unique<CylinderCommand>());
    theRegistry.Add(std::make_unique<SphereCommand>());
    theRegistry.Add(std::make_unique<ConeCommand>());
    theRegistry.Add(std::make_unique<TorusCommand>());
    theRegistry.Add(std::make_unique<ExtrudeCommand>());
    theRegistry.Add(std::make_unique<RevolveCommand>());

    theRegistry.Add(std::make_unique<FilletCommand>());
    theRegistry.Add(std::make_unique<ChamferCommand>());
    theRegistry.Add(std::make_unique<ShellCommand>());

    theRegistry.Add(std::make_unique<OffsetPlaneCommand>());
    theRegistry.Add(std::make_unique<AnglePlaneCommand>());
    theRegistry.Add(std::make_unique<MidplaneCommand>());
    theRegistry.Add(std::make_unique<ThreePointPlaneCommand>());
    theRegistry.Add(std::make_unique<TwoPointAxisCommand>());
    theRegistry.Add(std::make_unique<NormalAxisCommand>());
    theRegistry.Add(std::make_unique<CoordinatePointCommand>());
    theRegistry.Add(std::make_unique<AxisPlanePointCommand>());
}

} // namespace lcad
