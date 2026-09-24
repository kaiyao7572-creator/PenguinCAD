#include "core/Command.h"
#include "core/Document.h"
#include "core/GeometrySelection.h"
#include "features/CombineFeature.h"
#include "features/FeatureDialogs.h"
#include "features/FeatureUtils.h"

#include <AIS_Shape.hxx>

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

// Fusion keeps Combine in the Solid tab's MODIFY panel, beside Fillet and
// Shell. Spelled out rather than shared: the constants in
// FeatureCommands.cpp are file-local, and these two have to stay in step
// with them or the button lands in a tab of its own.
const char* const kSolidGroup  = "Solid";
const char* const kModifyGroup = "Modify";

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

QStringList NamesOf(const std::vector<CombineBodyRef>& theRefs)
{
    QStringList names;
    for (const CombineBodyRef& ref : theRefs) {
        names << QString::fromStdString(ref.name);
    }
    return names;
}

// Mirrors the helper of the same name in FeatureCommands.cpp: a feature
// that failed stays in the timeline on purpose, because the properties
// panel is where the user fixes whichever pick was wrong.
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

std::size_t SolidBodyCount(const Document* theDocument)
{
    if (theDocument == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const BodyPtr& body : theDocument->Bodies()) {
        if (body && body->IsSolid()) {
            ++count;
        }
    }
    return count;
}

// The document's bodies as picks. A body Combine cannot act on -- a
// surface, a wire, anything with no volume -- is left out of the lists
// rather than offered and then refused.
std::vector<CombineBodyRef> PickableBodies(const Document* theDocument)
{
    std::vector<CombineBodyRef> refs;
    if (theDocument == nullptr) {
        return refs;
    }
    for (const BodyPtr& body : theDocument->Bodies()) {
        if (!body) {
            continue;
        }
        CombineBodyRef ref = MakeCombineBodyRef(*body);
        if (!ref.IsNull()) {
            refs.push_back(std::move(ref));
        }
    }
    return refs;
}

const CombineBodyRef* FindPick(const std::vector<CombineBodyRef>& theRefs,
                               const std::string&                 theName)
{
    for (const CombineBodyRef& ref : theRefs) {
        if (ref.name == theName) {
            return &ref;
        }
    }
    return nullptr;
}

// The body a displayed shape is, matched by SHAPE IDENTITY against the
// document's own bodies -- the same signal MoveGizmoTool trusts, because
// the viewport got the shape from there in the first place.
std::string BodyNameOf(const Document& theDocument, const TopoDS_Shape& theShape)
{
    for (const BodyPtr& body : theDocument.Bodies()) {
        if (body && !body->Shape().IsNull() && theShape.IsSame(body->Shape())) {
            return body->Name();
        }
    }
    return std::string();
}

// The bodies already picked in the viewport, in the order OCCT reports
// them. Picking a FACE counts as picking its body, which is how Fusion
// reads a click too -- and the dialog names what it took, so a stray pick
// is visible before OK rather than after.
std::vector<CombineBodyRef> SelectedBodies(const CommandContext&              theContext,
                                           const std::vector<CombineBodyRef>& thePickable)
{
    std::vector<CombineBodyRef> picked;
    const Handle(AIS_InteractiveContext) aisContext = theContext.AisContext();
    if (aisContext.IsNull() || theContext.document == nullptr) {
        return picked;
    }

    for (aisContext->InitSelected(); aisContext->MoreSelected(); aisContext->NextSelected()) {
        Handle(AIS_Shape) displayed =
            Handle(AIS_Shape)::DownCast(aisContext->SelectedInteractive());
        if (displayed.IsNull() || displayed->Shape().IsNull()) {
            continue;
        }

        const std::string name = BodyNameOf(*theContext.document, displayed->Shape());
        if (name.empty() || FindPick(picked, name) != nullptr) {
            continue;
        }
        if (const CombineBodyRef* ref = FindPick(thePickable, name)) {
            picked.push_back(*ref);
        }
    }
    return picked;
}

const CombineBodyRef* ChosenBody(const std::vector<CombineBodyRef>& theRefs, int theChoice)
{
    if (theChoice < 0 || static_cast<std::size_t>(theChoice) >= theRefs.size()) {
        return nullptr;
    }
    return &theRefs[static_cast<std::size_t>(theChoice)];
}

// Fusion's MODIFY > COMBINE. Extrude and Revolve can already join, cut
// and intersect as they CREATE geometry; this is the same three
// operations between two bodies that are already there.
class CombineCommand : public Command
{
public:
    std::string Id() const override { return "modify.combine"; }
    std::string Title() const override { return "Combine"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return kModifyGroup; }
    std::string Icon() const override { return "⧉"; }
    std::string Description() const override
    {
        return "Join, cut or intersect one body with another";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        // Two, because a body has nothing to combine with on its own.
        return SolidBodyCount(theContext.document) >= 2;
    }

    void Execute(CommandContext& theContext) override
    {
        const std::vector<CombineBodyRef> pickable = PickableBodies(theContext.document);
        if (pickable.size() < 2) {
            ShowStatus(theContext, "Combine needs two solid bodies.");
            return;
        }

        // Fusion's flow is to pick the bodies in the viewport and then say
        // what to do with them. With two or more already picked the dialog
        // only has to ask which of THEM is the target; everything else
        // picked is a tool, and several tools are the normal case.
        const std::vector<CombineBodyRef> selected = SelectedBodies(theContext, pickable);
        const bool fromSelection = selected.size() >= 2;
        const std::vector<CombineBodyRef>& targets = fromSelection ? selected : pickable;

        // Default the target to the one body already picked, if there is
        // one, and the tool to something that isn't it.
        int targetChoice = 0;
        if (!fromSelection && selected.size() == 1) {
            for (std::size_t i = 0; i < pickable.size(); ++i) {
                if (pickable[i].name == selected.front().name) {
                    targetChoice = static_cast<int>(i);
                }
            }
        }

        std::vector<DialogField> fields;
        fields.push_back(DialogField::Choice("Target Body", NamesOf(targets), targetChoice));

        std::size_t toolRow = 0;
        if (!fromSelection) {
            toolRow = fields.size();
            fields.push_back(DialogField::Choice("Tool Body", NamesOf(pickable),
                                                 targetChoice == 0 ? 1 : 0));
        }

        const std::size_t operationRow = fields.size();
        fields.push_back(
            DialogField::Choice("Operation", ToChoices(CombineOperationNames()), 0));
        const std::size_t keepRow = fields.size();
        fields.push_back(DialogField::Toggle("Keep Tools", false));

        QString hint = QStringLiteral(
            "The tool body is consumed unless Keep Tools is on. Cut and Intersect "
            "need the bodies to overlap.");
        if (fromSelection) {
            hint = QStringLiteral("Combining the %1 bodies picked in the viewport: everything "
                                  "except the target is a tool. ")
                       .arg(selected.size())
                   + hint;
        }

        if (!ShowFeatureDialog(theContext.parent, "Combine", fields, hint)) {
            return;
        }

        const CombineBodyRef* target = ChosenBody(targets, fields[0].choice);
        if (target == nullptr) {
            return;
        }

        std::vector<CombineBodyRef> tools;
        if (fromSelection) {
            for (const CombineBodyRef& ref : selected) {
                if (ref.name != target->name) {
                    tools.push_back(ref);
                }
            }
        } else {
            const CombineBodyRef* tool = ChosenBody(pickable, fields[toolRow].choice);
            if (tool == nullptr) {
                return;
            }
            if (tool->name == target->name) {
                ShowStatus(theContext, "A body cannot be combined with itself.");
                return;
            }
            tools.push_back(*tool);
        }
        if (tools.empty()) {
            ShowStatus(theContext, "A body cannot be combined with itself.");
            return;
        }

        auto combine = std::make_shared<CombineFeature>(
            *target, std::move(tools), CombineOperationFromChoice(fields[operationRow].choice));
        combine->SetKeepTools(fields[keepRow].toggle);
        AddAndReport(theContext, combine);

        // The bodies this consumed are gone, so a selection still pointing
        // at them would leave a highlight on geometry that no longer
        // exists and let a second Combine act on stale picks.
        GeometrySelection::Instance().Clear();
        const Handle(AIS_InteractiveContext) aisContext = theContext.AisContext();
        if (!aisContext.IsNull()) {
            aisContext->ClearSelected(Standard_False);
        }
    }
};

} // namespace

// Declared in core/Registration.h alongside the other subsystems'
// registration hooks; MainWindow calls it once at startup.
void RegisterCombineCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<CombineCommand>());
}

} // namespace lcad
