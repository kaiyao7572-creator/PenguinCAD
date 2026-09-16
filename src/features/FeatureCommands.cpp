#include "core/Command.h"
#include "core/Document.h"
#include "core/ProfileProvider.h"
#include "core/Registration.h"
#include "features/FeatureDialogs.h"
#include "features/FeatureUtils.h"
#include "features/ModifyFeatures.h"
#include "features/PrimitiveFeatures.h"
#include "features/ProfileFeatures.h"

#include <gp_Pnt.hxx>

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

        std::string sketchName;
        if (!AskForSketch(theContext, "Extrude", fields,
                          "Symmetric splits the distance either side of the sketch plane.",
                          sketchName)) {
            return;
        }

        auto extrude = std::make_shared<ExtrudeFeature>(sketchName, fields[1].value);
        extrude->SetReversed(fields[2].toggle);
        extrude->SetSymmetric(fields[3].toggle);
        extrude->SetOperation(BooleanOpFromInt(fields[4].choice));
        AddAndReport(theContext, extrude);
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

        std::string sketchName;
        if (!AskForSketch(theContext, "Revolve", fields,
                          "The axis must not cross the profile -- draw the profile to one "
                          "side of it.",
                          sketchName)) {
            return;
        }

        auto revolve = std::make_shared<RevolveFeature>(
            sketchName, fields[1].value, RevolveAxisFromInt(fields[2].choice));
        revolve->SetReversed(fields[3].toggle);
        revolve->SetOperation(BooleanOpFromInt(fields[4].choice));
        AddAndReport(theContext, revolve);
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
}

} // namespace lcad
