#include "core/Command.h"
#include "core/Document.h"
#include "core/GeometrySelection.h"
#include "features/CombineFeature.h"
#include "features/FeatureDialogs.h"
#include "features/FeatureUtils.h"
#include "features/PatternFeatures.h"

#include <AIS_Shape.hxx>

#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>
#include <QStringList>

namespace lcad {

namespace {

// Fusion keeps Rectangular Pattern, Circular Pattern and Mirror in the
// Solid tab's CREATE panel, under Extrude and Sweep. Spelled out rather
// than shared: the constants in FeatureCommands.cpp are file-local, and
// these have to stay in step with them or the buttons land in a tab of
// their own.
const char* const kSolidGroup     = "Solid";
const char* const kCreateSection  = "Create";

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

// The document's bodies as picks. A body a pattern cannot copy -- a
// surface, a wire, anything with no volume -- is left out of the list
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
// document's own bodies -- the same signal Combine trusts, because the
// viewport got the shape from there in the first place.
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
// reads a click too.
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

// What a pattern will copy.
//
// Fusion's flow is to pick the bodies in the viewport and then say how to
// repeat them, so with a pick already made the dialog has nothing to ask.
// Without one it asks for a single body, which is the common case;
// picking several is what the viewport is for.
struct BodyPicks
{
    std::vector<CombineBodyRef> pickable;
    std::vector<CombineBodyRef> selected;
    bool                        fromSelection = false;
    std::size_t                 row           = 0;  // the "Body" row, when there is one
    bool                        hasRow        = false;
};

BodyPicks GatherBodies(const CommandContext& theContext)
{
    BodyPicks picks;
    picks.pickable = PickableBodies(theContext.document);
    picks.selected = SelectedBodies(theContext, picks.pickable);
    picks.fromSelection = !picks.selected.empty();
    return picks;
}

void AppendBodyRow(std::vector<DialogField>& theFields, BodyPicks& thePicks)
{
    if (thePicks.fromSelection) {
        return;
    }
    thePicks.row = theFields.size();
    thePicks.hasRow = true;
    theFields.push_back(DialogField::Choice("Body", NamesOf(thePicks.pickable), 0));
}

std::vector<CombineBodyRef> ChosenBodies(const std::vector<DialogField>& theFields,
                                         const BodyPicks&                thePicks)
{
    if (thePicks.fromSelection) {
        return thePicks.selected;
    }
    if (!thePicks.hasRow || thePicks.row >= theFields.size()) {
        return {};
    }
    const int choice = theFields[thePicks.row].choice;
    if (choice < 0 || static_cast<std::size_t>(choice) >= thePicks.pickable.size()) {
        return {};
    }
    return {thePicks.pickable[static_cast<std::size_t>(choice)]};
}

QString SelectionHint(const BodyPicks& thePicks)
{
    if (!thePicks.fromSelection) {
        return QString();
    }
    const QString what = thePicks.selected.size() == 1
                             ? QStringLiteral("the body")
                             : QStringLiteral("the %1 bodies").arg(thePicks.selected.size());
    return QStringLiteral("Repeating %1 picked in the viewport. ").arg(what);
}

// The seed bodies survive a pattern, but the rebuild replaces every shape
// in the document, so a selection still pointing at the old ones would
// leave a highlight on geometry that no longer exists.
void ClearPicks(const CommandContext& theContext)
{
    GeometrySelection::Instance().Clear();
    const Handle(AIS_InteractiveContext) aisContext = theContext.AisContext();
    if (!aisContext.IsNull()) {
        aisContext->ClearSelected(Standard_False);
    }
}

// Re-queried on every document change, so it asks the cheap question:
// Body::IsSolid() is a stored kind, while building the picks measures
// every body in the document to fill in its signature.
bool HasPickableBody(const CommandContext& theContext)
{
    if (theContext.document == nullptr) {
        return false;
    }
    for (const BodyPtr& body : theContext.document->Bodies()) {
        if (body && body->IsSolid()) {
            return true;
        }
    }
    return false;
}

// A whole-number field. The Number factory exists for millimetres, so the
// suffix and the range are widened afterwards rather than guessed at by
// it; the ceiling is the one Compute enforces, so the dialog cannot ask
// for a grid that is refused on OK.
DialogField CountField(const QString& theLabel, int theValue)
{
    DialogField field =
        DialogField::Number(theLabel, static_cast<double>(theValue), 1.0, QString());
    field.maximum  = static_cast<double>(PatternFeature::kMaxInstances);
    field.decimals = 0;
    return field;
}

// A spacing may be negative: that is how the row runs the other way,
// which is what Fusion's direction flip does.
DialogField SpacingField(const QString& theLabel, double theValue)
{
    DialogField field = DialogField::Number(theLabel, theValue, -1.0e6);
    field.decimals = 3;
    return field;
}

DialogField AngleField(const QString& theLabel, double theValue)
{
    DialogField field = DialogField::Number(theLabel, theValue, -360.0, QStringLiteral(" deg"));
    field.maximum  = 360.0;
    field.decimals = 2;
    return field;
}

// The unit field hands back a double, and a quantity is a whole number of
// copies: 3.9999 is the 4 the user typed.
int CountOf(const DialogField& theField)
{
    return static_cast<int>(std::lround(theField.value));
}

// Fusion's CREATE > PATTERN > RECTANGULAR PATTERN.
class RectangularPatternCommand : public Command
{
public:
    std::string Id() const override { return "solid.pattern.rectangular"; }
    std::string Title() const override { return "Rectangular Pattern"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return kCreateSection; }
    std::string Icon() const override { return "▦"; }
    std::string Shortcut() const override { return "Shift+P"; }
    std::string Description() const override
    {
        return "Repeat a body in a grid along one or two directions";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return HasPickableBody(theContext);
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        BodyPicks picks = GatherBodies(theContext);
        if (picks.pickable.empty()) {
            ShowStatus(theContext, "Rectangular Pattern needs a solid body.");
            return;
        }

        std::vector<DialogField> fields;
        AppendBodyRow(fields, picks);

        const std::size_t direction1Row = fields.size();
        fields.push_back(DialogField::Choice("Direction 1", ToChoices(PatternAxisNames()),
                                             static_cast<int>(PatternAxis::WorldX)));
        const std::size_t quantity1Row = fields.size();
        fields.push_back(CountField("Quantity 1", 2));
        const std::size_t spacing1Row = fields.size();
        fields.push_back(SpacingField("Spacing 1", 20.0));

        const std::size_t direction2Row = fields.size();
        fields.push_back(DialogField::Choice("Direction 2", ToChoices(PatternAxisNames()),
                                             static_cast<int>(PatternAxis::WorldY)));
        const std::size_t quantity2Row = fields.size();
        fields.push_back(CountField("Quantity 2", 1));
        const std::size_t spacing2Row = fields.size();
        fields.push_back(SpacingField("Spacing 2", 20.0));

        const std::size_t operationRow = fields.size();
        fields.push_back(DialogField::Choice("Operation", ToChoices(BooleanOpNames()),
                                             static_cast<int>(BooleanOp::NewBody)));

        const QString hint =
            SelectionHint(picks)
            + QStringLiteral("Spacing is the gap between one instance and the next, so a "
                             "bigger quantity makes a longer row rather than a denser one. "
                             "A negative spacing runs the other way, and a quantity of 1 "
                             "leaves that direction alone.");

        // Every Number label here is the name of the parameter it sets, so
        // the fields take expressions ("n_holes", "pitch * 2").
        if (!ShowFeatureDialog(theContext.parent, "Rectangular Pattern", fields, hint,
                               &theContext.document->UserParameters())) {
            return;
        }

        std::vector<CombineBodyRef> bodies = ChosenBodies(fields, picks);
        if (bodies.empty()) {
            return;
        }

        auto pattern = std::make_shared<RectangularPatternFeature>(
            std::move(bodies),
            CountOf(fields[quantity1Row]), fields[spacing1Row].value,
            CountOf(fields[quantity2Row]), fields[spacing2Row].value);
        pattern->SetDirection1(PatternAxisFromInt(fields[direction1Row].choice));
        pattern->SetDirection2(PatternAxisFromInt(fields[direction2Row].choice));
        pattern->SetOperation(BooleanOpFromInt(fields[operationRow].choice));

        ApplyFieldExpressions(*pattern, fields);
        AddAndReport(theContext, pattern);
        ClearPicks(theContext);
    }
};

// Fusion's CREATE > PATTERN > CIRCULAR PATTERN.
class CircularPatternCommand : public Command
{
public:
    std::string Id() const override { return "solid.pattern.circular"; }
    std::string Title() const override { return "Circular Pattern"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return kCreateSection; }
    std::string Icon() const override { return "✳"; }
    std::string Shortcut() const override { return "Shift+O"; }
    std::string Description() const override
    {
        return "Repeat a body evenly around an axis";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return HasPickableBody(theContext);
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        BodyPicks picks = GatherBodies(theContext);
        if (picks.pickable.empty()) {
            ShowStatus(theContext, "Circular Pattern needs a solid body.");
            return;
        }

        std::vector<DialogField> fields;
        AppendBodyRow(fields, picks);

        const std::size_t axisRow = fields.size();
        fields.push_back(DialogField::Choice("Axis", ToChoices(PatternAxisNames()),
                                             static_cast<int>(PatternAxis::WorldZ)));
        const std::size_t quantityRow = fields.size();
        fields.push_back(CountField("Quantity", 4));
        const std::size_t angleRow = fields.size();
        fields.push_back(AngleField("Total Angle", 360.0));
        const std::size_t operationRow = fields.size();
        fields.push_back(DialogField::Choice("Operation", ToChoices(BooleanOpNames()),
                                             static_cast<int>(BooleanOp::NewBody)));

        const QString hint =
            SelectionHint(picks)
            + QStringLiteral("The instances are spread evenly over the total angle. A full "
                             "360 closes the ring, so the step is the angle divided by the "
                             "quantity; a smaller angle leaves the last instance sitting on "
                             "it. The axis runs through the world origin.");

        if (!ShowFeatureDialog(theContext.parent, "Circular Pattern", fields, hint,
                               &theContext.document->UserParameters())) {
            return;
        }

        std::vector<CombineBodyRef> bodies = ChosenBodies(fields, picks);
        if (bodies.empty()) {
            return;
        }

        auto pattern = std::make_shared<CircularPatternFeature>(
            std::move(bodies), PatternAxisFromInt(fields[axisRow].choice),
            CountOf(fields[quantityRow]), fields[angleRow].value);
        pattern->SetOperation(BooleanOpFromInt(fields[operationRow].choice));

        ApplyFieldExpressions(*pattern, fields);
        AddAndReport(theContext, pattern);
        ClearPicks(theContext);
    }
};

// Fusion's CREATE > MIRROR, at the body level. The sketch subsystem
// already mirrors curves; this reflects whole bodies, fillets and shells
// included.
class MirrorCommand : public Command
{
public:
    std::string Id() const override { return "solid.mirror"; }
    std::string Title() const override { return "Mirror"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return kCreateSection; }
    std::string Icon() const override { return "◫"; }
    std::string Shortcut() const override { return "Shift+M"; }
    std::string Description() const override
    {
        return "Reflect a body across one of the world planes";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return HasPickableBody(theContext);
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        BodyPicks picks = GatherBodies(theContext);
        if (picks.pickable.empty()) {
            ShowStatus(theContext, "Mirror needs a solid body.");
            return;
        }

        std::vector<DialogField> fields;
        AppendBodyRow(fields, picks);

        const std::size_t planeRow = fields.size();
        fields.push_back(DialogField::Choice("Mirror Plane", ToChoices(MirrorPlaneNames()),
                                             static_cast<int>(MirrorPlane::YZ)));
        const std::size_t operationRow = fields.size();
        fields.push_back(DialogField::Choice("Operation", ToChoices(BooleanOpNames()),
                                             static_cast<int>(BooleanOp::NewBody)));

        const QString hint =
            SelectionHint(picks)
            + QStringLiteral("The planes run through the world origin. A body sitting "
                             "symmetrically across the chosen plane would be reflected onto "
                             "itself, and is refused rather than doubled.");

        if (!ShowFeatureDialog(theContext.parent, "Mirror", fields, hint)) {
            return;
        }

        std::vector<CombineBodyRef> bodies = ChosenBodies(fields, picks);
        if (bodies.empty()) {
            return;
        }

        auto mirror = std::make_shared<MirrorFeature>(
            std::move(bodies), MirrorPlaneFromInt(fields[planeRow].choice));
        mirror->SetOperation(BooleanOpFromInt(fields[operationRow].choice));

        AddAndReport(theContext, mirror);
        ClearPicks(theContext);
    }
};

} // namespace

// Declared in core/Registration.h alongside the other subsystems'
// registration hooks; MainWindow calls it once at startup.
void RegisterPatternCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<RectangularPatternCommand>());
    theRegistry.Add(std::make_unique<CircularPatternCommand>());
    theRegistry.Add(std::make_unique<MirrorCommand>());
}

} // namespace lcad
