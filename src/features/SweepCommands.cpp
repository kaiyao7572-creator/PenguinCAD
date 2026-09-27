#include "core/Command.h"
#include "core/Document.h"
#include "core/ProfileProvider.h"
#include "core/ProfileSelection.h"
#include "features/FeatureDialogs.h"
#include "features/FeatureUtils.h"
#include "features/SweepFeature.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>
#include <QStringList>

namespace lcad {

namespace {

const char* const kSolidGroup = "Solid";

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

// Sketches that actually close into something. Both ends of a sweep need
// one: the section is a face, and the path is resolved through the same
// ProfileProvider seam, which hands over closed wires and nothing else.
std::vector<std::string> SketchesWithProfile(const Document* theDocument)
{
    std::vector<std::string> names;
    if (theDocument == nullptr) {
        return names;
    }
    for (const FeaturePtr& feature : theDocument->Features()) {
        if (!feature || feature->TypeName() != "Sketch") {
            continue;
        }
        ProfileProvider* provider = AsProfileProvider(feature.get());
        if (provider != nullptr && !provider->ProfileFaces().empty()) {
            names.push_back(feature->Name());
        }
    }
    return names;
}

// True when the user has already clicked regions of a sketch still in
// this document. A selection left over from a sketch since deleted must
// not steer the dialog.
bool HasPickedProfiles(const CommandContext& theContext)
{
    const ProfileSelection& selection = ProfileSelection::Instance();
    if (selection.IsEmpty() || selection.SketchName().empty()) {
        return false;
    }
    const std::vector<std::string> names = SketchNames(theContext.document);
    return std::find(names.begin(), names.end(), selection.SketchName()) != names.end();
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

// Adds the feature and reports what the rebuild made of it. A feature
// that failed stays in the timeline on purpose -- the properties panel is
// where the user fixes the answer that was wrong.
void AddAndReport(CommandContext& theContext, const FeaturePtr& theFeature)
{
    if (theContext.document == nullptr || !theFeature) {
        return;
    }

    theContext.document->AddFeature(theFeature);
    theContext.document->SetActiveFeature(theFeature);

    if (!theFeature->LastError().empty()) {
        ShowStatus(theContext, QString::fromStdString(theFeature->LastError()));
        return;
    }
    ShowStatus(theContext, QString::fromStdString(theFeature->Name()) + " created.");
}

// An angle field: the Number factory exists for millimetres, so the range
// and suffix are widened afterwards rather than guessed at by it.
DialogField AngleField(const QString& theLabel, double theMinimum, double theMaximum)
{
    DialogField field = DialogField::Number(theLabel, 0.0, theMinimum, QStringLiteral(" deg"));
    field.maximum  = theMaximum;
    field.decimals = 2;
    return field;
}

// Fusion's CREATE > SWEEP, beside Extrude and Revolve.
class SweepCommand : public Command
{
public:
    std::string Id() const override { return "solid.sweep"; }
    std::string Title() const override { return "Sweep"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return "Create"; }
    std::string Icon() const override { return ":/icons/solid.sweep.svg"; }
    std::string Shortcut() const override { return "Shift+W"; }
    std::string Description() const override
    {
        return "Drive a closed sketch profile along a second sketch used as the path";
    }

    // A section and a path are two different sketches, so the button
    // greys out until there are two closed ones rather than opening a
    // dialog that cannot be answered.
    bool IsEnabled(const CommandContext& theContext) const override
    {
        return SketchesWithProfile(theContext.document).size() >= 2;
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        const std::vector<std::string> usable = SketchesWithProfile(theContext.document);
        if (usable.size() < 2) {
            ShowStatus(theContext,
                       "Sweep needs two closed sketches: one to sweep, one to follow.");
            return;
        }

        // The section: whatever the user already clicked, or a dropdown.
        // Holding the picked regions is what lets a sweep act on one
        // region of a split sketch, the job Fusion's "Profile: 1 selected"
        // row does.
        const bool              picked = HasPickedProfiles(theContext);
        const ProfileSelection& selection = ProfileSelection::Instance();

        std::vector<std::string> sectionNames;
        QStringList              sectionLabels;
        if (picked) {
            const int count = static_cast<int>(selection.Count());
            sectionNames.push_back(selection.SketchName());
            sectionLabels << QString::fromStdString(selection.SketchName())
                                 + QStringLiteral(" (%1 profile%2)")
                                       .arg(count)
                                       .arg(count == 1 ? "" : "s");
        } else {
            sectionNames = usable;
            sectionLabels = ToChoices(usable);
        }

        // The path opens on a sketch that is not the section, since those
        // two being the same is the one answer that can never build.
        int pathPreset = 0;
        for (std::size_t i = 0; i < usable.size(); ++i) {
            if (usable[i] != sectionNames.front()) {
                pathPreset = static_cast<int>(i);
                break;
            }
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Choice("Profile", sectionLabels, 0));
        fields.push_back(DialogField::Choice("Path", ToChoices(usable), pathPreset));
        fields.push_back(DialogField::Choice("Orientation",
                                             ToChoices(SweepOrientationNames()),
                                             static_cast<int>(SweepOrientation::Perpendicular)));
        fields.push_back(AngleField("Taper Angle", -89.0, 89.0));
        fields.push_back(AngleField("Twist Angle", -3600.0, 3600.0));
        fields.push_back(DialogField::Choice("Operation",
                                             ToChoices(BooleanOpNames()),
                                             DefaultOperationChoice(theContext)));

        const QString hint =
            QStringLiteral("The path is a closed sketch loop. Perpendicular keeps the "
                           "profile square to the path; Parallel slides it along unturned.");

        // Taper Angle and Twist Angle are the names of the parameters they
        // set, so both take expressions.
        if (!ShowFeatureDialog(theContext.parent, "Sweep", fields, hint,
                               &theContext.document->EvaluationTable())) {
            return;
        }

        const int sectionPick = fields[0].choice;
        const int pathPick    = fields[1].choice;
        if (sectionPick < 0 || sectionPick >= static_cast<int>(sectionNames.size())
            || pathPick < 0 || pathPick >= static_cast<int>(usable.size())) {
            return;
        }

        const std::string sectionName = sectionNames[static_cast<std::size_t>(sectionPick)];
        const std::string pathName    = usable[static_cast<std::size_t>(pathPick)];

        // Checked here as well as in the feature so an answer that cannot
        // build never becomes a timeline entry; the feature still checks
        // because both names stay editable afterwards.
        if (sectionName == pathName) {
            ShowStatus(theContext, "Pick a different sketch for the path than the profile.");
            return;
        }

        auto sweep = std::make_shared<SweepFeature>(sectionName, pathName);
        if (picked) {
            sweep->SetProfiles(selection.Items());
        }
        sweep->SetOrientation(SweepOrientationFromInt(fields[2].choice));
        sweep->SetTaperDegrees(fields[3].value);
        sweep->SetTwistDegrees(fields[4].value);
        sweep->SetOperation(BooleanOpFromInt(fields[5].choice));
        ApplyFieldExpressions(*sweep, fields);
        AddAndReport(theContext, sweep);

        // A profile is consumed once it has been built on: leaving it
        // picked would make a second press silently build the same thing
        // again.
        ProfileSelection::Instance().Clear();
    }
};

} // namespace

void RegisterSweepCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<SweepCommand>());
}

} // namespace lcad
