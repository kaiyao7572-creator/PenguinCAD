#include "core/Command.h"
#include "core/Document.h"
#include "core/Registration.h"
#include "sketch/SketchCommandGroups.h"
#include "sketch/SketchDialogs.h"
#include "sketch/SketchDisplay.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchModifyTools.h"
#include "sketch/SketchPlanePicker.h"
#include "sketch/SketchSelection.h"
#include "sketch/SketchTools.h"

#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>

#include <memory>
#include <vector>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

const char* const kSketchGroup = "Sketch";
// Entering a sketch is a Solid-tab action in Fusion, not a Sketch-tab
// one -- the Sketch tab does not exist yet at that point.
const char* const kSolidGroup = "Solid";

// Fusion's own ribbon captions, in Fusion's own order.
const char* const kCreateSection = "Create";
const char* const kModifySection = "Modify";

constexpr double kPi = 3.14159265358979323846;

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

// A planar face already picked in the viewport, if there is one. Fusion's
// single shortcut past the plane pick: with a face selected, the question
// has already been answered, so it isn't asked.
bool SelectedPlanarFace(const CommandContext& theContext, gp_Ax3& thePosition)
{
    const Handle(AIS_InteractiveContext) aisContext = theContext.AisContext();
    if (aisContext.IsNull()) {
        return false;
    }

    for (aisContext->InitSelected(); aisContext->MoreSelected(); aisContext->NextSelected()) {
        if (!aisContext->HasSelectedShape()) {
            continue;
        }
        if (SketchPlaneFromFace(aisContext->SelectedShape(), thePosition)) {
            return true;
        }
    }
    return false;
}

// Create the sketch on a plane the user has committed to and drop straight
// into sketch mode. Everything the viewport does about that -- look at the
// plane, move the grid onto it, fade the model, switch the ribbon tab --
// belongs to SketchSession::Begin, so both entry points get it.
void StartSketchOn(const CommandContext& theContext, const gp_Ax3& thePlane)
{
    if (theContext.document == nullptr) {
        return;
    }

    // Whatever was picked to get here has served its purpose; leaving it
    // highlighted underneath the new sketch just adds noise.
    const Handle(AIS_InteractiveContext) aisContext = theContext.AisContext();
    if (!aisContext.IsNull()) {
        aisContext->ClearSelected(Standard_False);
    }

    // No offset: picking first and adjusting after is the Fusion order,
    // and Offset is a sketch Parameter, so the properties panel edits it
    // without a dialog ever appearing.
    auto sketch = std::make_shared<SketchFeature>(thePlane, 0.0);
    sketch->SetName(theContext.document->MakeUniqueName("Sketch"));

    // Attach before the feature lands in the document: AddFeature
    // rebuilds immediately, and that rebuild is what draws the sketch.
    SketchDisplay::Instance().Attach(theContext);
    theContext.document->AddFeature(sketch);
    theContext.document->SetActiveFeature(sketch);

    SketchSession::Instance().Begin(theContext, sketch->Name());
    ShowStatus(theContext,
               QString::fromStdString(sketch->Name())
                   + " active - pick a sketch tool (L, R, C, A, D).");
}

// ---- session commands (the unlabelled section that leads the tab) ----

class CreateSketchCommand : public Command
{
public:
    std::string Id() const override { return "sketch.create"; }
    std::string Title() const override { return "Create Sketch"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return kCreateSection; }
    std::string Icon() const override { return ":/icons/sketch.create.svg"; }
    // No key, as in Fusion: Ctrl+Shift+S was one here until File > Save As
    // needed the key every other desktop app gives it.

    std::string Description() const override
    {
        return "Pick a plane or a planar face in the viewport and start sketching on it";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        // The plane pick happens in the viewport, so there is nothing to
        // run until the viewer is actually up.
        return theContext.document != nullptr && !theContext.View().IsNull();
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }

        gp_Ax3 facePosition;
        if (SelectedPlanarFace(theContext, facePosition)) {
            StartSketchOn(theContext, facePosition);
            return;
        }

        // No dialog: the origin planes appear in the viewport and the user
        // clicks one. Nothing is created until they do, so Escape here
        // leaves the document exactly as it was.
        //
        // The context is captured by value because Execute's reference is
        // gone long before the click arrives.
        const CommandContext context = theContext;
        SketchPlanePicker::Instance().Start(
            theContext, [context](const gp_Ax3& thePlane) { StartSketchOn(context, thePlane); });
    }
};

// Re-opens a finished sketch, so Finish Sketch isn't a one-way door.
class EditSketchCommand : public Command
{
public:
    std::string Id() const override { return "sketch.edit"; }
    std::string Title() const override { return "Edit Sketch"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return kCreateSection; }
    std::string Icon() const override { return ":/icons/sketch.edit.svg"; }
    std::string Shortcut() const override { return "Ctrl+Shift+E"; }
    std::string Description() const override
    {
        return "Re-open the selected sketch (or the most recent one) for editing";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        const SketchFeature* target = Target(theContext);
        return target != nullptr && target->Name() != SketchSession::Instance().ActiveName();
    }

    void Execute(CommandContext& theContext) override
    {
        SketchFeature* target = Target(theContext);
        if (target == nullptr) {
            return;
        }

        SketchDisplay::Instance().Attach(theContext);
        SketchSession::Instance().Begin(theContext, target->Name());
        ShowStatus(theContext,
                   "Editing " + QString::fromStdString(target->Name())
                       + " - pick a sketch tool (L, R, C, A, D).");
    }

private:
    static SketchFeature* Target(const CommandContext& theContext)
    {
        if (theContext.document == nullptr) {
            return nullptr;
        }
        // Whatever the browser tree has selected wins...
        if (const FeaturePtr active = theContext.document->ActiveFeature()) {
            if (SketchFeature* sketch = dynamic_cast<SketchFeature*>(active.get())) {
                return sketch;
            }
        }
        // ...otherwise fall back to the newest sketch in the timeline.
        const std::vector<FeaturePtr>& features = theContext.document->Features();
        for (auto it = features.rbegin(); it != features.rend(); ++it) {
            if (SketchFeature* sketch = dynamic_cast<SketchFeature*>(it->get())) {
                return sketch;
            }
        }
        return nullptr;
    }
};

class FinishSketchCommand : public Command
{
public:
    std::string Id() const override { return "sketch.finish"; }
    std::string Title() const override { return "Finish Sketch"; }
    std::string Group() const override { return kSketchGroup; }
    std::string Icon() const override { return ":/icons/sketch.finish.svg"; }
    std::string Shortcut() const override { return "Ctrl+Return"; }
    std::string Description() const override
    {
        return "Leave sketch mode and hand the viewport back to selection";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return SketchSession::Instance().ActiveSketch(theContext.document) != nullptr;
    }

    void Execute(CommandContext& theContext) override
    {
        SketchFeature* sketch = SketchSession::Instance().ActiveSketch(theContext.document);
        const std::size_t profiles = sketch != nullptr ? sketch->ProfileWires().size() : 0;

        SketchSession::Instance().End(theContext);

        ShowStatus(theContext,
                   profiles > 0
                       ? QString("Sketch finished - %1 closed profile(s) ready to extrude.")
                             .arg(static_cast<int>(profiles))
                       : QString("Sketch finished - no closed profile yet."));
    }
};

class ShowSketchesCommand : public Command
{
public:
    std::string Id() const override { return "sketch.visible"; }
    std::string Title() const override { return "Show Sketches"; }
    std::string Group() const override { return "View"; }
    std::string Section() const override { return "Show"; }
    std::string Icon() const override { return ":/icons/sketch.visible.svg"; }
    std::string Description() const override { return "Show or hide all sketch geometry"; }

    bool IsCheckable() const override { return true; }

    bool IsChecked(const CommandContext& theContext) const override
    {
        (void)theContext;
        return SketchDisplay::Instance().AreSketchesVisible();
    }

    void Execute(CommandContext& theContext) override
    {
        SketchDisplay& display = SketchDisplay::Instance();
        display.Attach(theContext);
        display.SetSketchesVisible(!display.AreSketchesVisible());
    }
};

class ShowConstraintsCommand : public Command
{
public:
    std::string Id() const override { return "sketch.constraints.visible"; }
    std::string Title() const override { return "Show Constraints"; }
    std::string Group() const override { return kSketchGroup; }
    std::string Icon() const override { return ":/icons/sketch.constraints.visible.svg"; }
    std::string Description() const override
    {
        return "Show or hide the constraint marks and dimensions on the active sketch";
    }

    bool IsCheckable() const override { return true; }

    bool IsChecked(const CommandContext& theContext) const override
    {
        (void)theContext;
        return SketchDisplay::Instance().AreConstraintsVisible();
    }

    void Execute(CommandContext& theContext) override
    {
        SketchDisplay& display = SketchDisplay::Instance();
        display.Attach(theContext);
        display.SetConstraintsVisible(!display.AreConstraintsVisible());
    }
};

// ---- shared plumbing for the drawing tools ----

class SketchToolCommand : public Command
{
public:
    std::string Group() const override { return kSketchGroup; }
    std::string Section() const override { return kCreateSection; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return SketchSession::Instance().ActiveSketch(theContext.document) != nullptr;
    }

    void Execute(CommandContext& theContext) override
    {
        if (SketchSession::Instance().ActiveSketch(theContext.document) == nullptr) {
            ShowStatus(theContext, "Create or re-open a sketch first.");
            return;
        }
        SketchDisplay::Instance().Attach(theContext);
        if (Prepare(theContext)) {
            Tool().Start(theContext);
        }
    }

protected:
    virtual SketchTool& Tool() const = 0;

    // A chance to ask for whatever the cursor can't supply. Returning
    // false abandons the command, which is what Cancel in a dialog means.
    virtual bool Prepare(CommandContext& theContext)
    {
        (void)theContext;
        return true;
    }
};

// Same thing, filed under MODIFY instead of CREATE.
class SketchModifyCommand : public SketchToolCommand
{
public:
    std::string Section() const override { return kModifySection; }
};

// Mirror and the patterns work on a selection, so they stay greyed out
// until there is one.
class SketchSelectionCommand : public SketchModifyCommand
{
public:
    bool IsEnabled(const CommandContext& theContext) const override
    {
        SketchFeature* sketch = SketchSession::Instance().ActiveSketch(theContext.document);
        if (sketch == nullptr) {
            return false;
        }
        SketchSelection::Instance().Prune(*sketch);
        return !SketchSelection::Instance().IsEmpty();
    }
};

#define LCAD_TOOL_COMMAND(ClassName, BaseClass, IdText, TitleText, ToolCall, Text)           \
    class ClassName : public BaseClass                                                        \
    {                                                                                         \
    public:                                                                                   \
        std::string Id() const override { return IdText; }                                    \
        std::string Title() const override { return TitleText; }                              \
        std::string Icon() const override { return ":/icons/" IdText ".svg"; }                 \
        std::string Description() const override { return Text; }                             \
                                                                                              \
    protected:                                                                                \
        SketchTool& Tool() const override { return ToolCall; }                                \
    }

// ---- Create ----

class LineToolCommand : public SketchToolCommand
{
public:
    std::string Id() const override { return "sketch.line"; }
    std::string Title() const override { return "Line"; }
    std::string Icon() const override { return ":/icons/sketch.line.svg"; }
    std::string Shortcut() const override { return "L"; }
    std::string Description() const override
    {
        return "Draw connected line segments on the active sketch";
    }

protected:
    SketchTool& Tool() const override { return LineTool(); }
};

class RectangleToolCommand : public SketchToolCommand
{
public:
    std::string Id() const override { return "sketch.rectangle"; }
    std::string Title() const override { return "2-Point Rectangle"; }
    std::string Icon() const override { return ":/icons/sketch.rectangle.svg"; }
    std::string Shortcut() const override { return "R"; }
    std::string Description() const override
    {
        return "Draw a rectangle from two opposite corners";
    }

protected:
    SketchTool& Tool() const override { return RectangleTool(); }
};

LCAD_TOOL_COMMAND(CentreRectangleCommand, SketchToolCommand, "sketch.rectangle.centre",
                  "Center Rectangle", CentreRectangleTool(),
                  "Draw a rectangle from its centre and one corner");
LCAD_TOOL_COMMAND(ThreePointRectangleCommand, SketchToolCommand, "sketch.rectangle.three",
                  "3-Point Rectangle", ThreePointRectangleTool(),
                  "Draw a rectangle at any angle from one edge and a width");

class CircleToolCommand : public SketchToolCommand
{
public:
    std::string Id() const override { return "sketch.circle"; }
    std::string Title() const override { return "Center Diameter Circle"; }
    std::string Icon() const override { return ":/icons/sketch.circle.svg"; }
    std::string Shortcut() const override { return "C"; }
    std::string Description() const override
    {
        return "Draw a circle from its centre and a point on the rim";
    }

protected:
    SketchTool& Tool() const override { return CircleTool(); }
};

LCAD_TOOL_COMMAND(TwoPointCircleCommand, SketchToolCommand, "sketch.circle.two",
                  "2-Point Circle", TwoPointCircleTool(),
                  "Draw a circle from the two ends of a diameter");
LCAD_TOOL_COMMAND(ThreePointCircleCommand, SketchToolCommand, "sketch.circle.three",
                  "3-Point Circle", ThreePointCircleTool(),
                  "Draw a circle through three points");
LCAD_TOOL_COMMAND(ThreePointArcCommand, SketchToolCommand, "sketch.arc.three",
                  "3-Point Arc", ThreePointArcTool(),
                  "Draw an arc from its two ends and a point it passes through");

class ArcToolCommand : public SketchToolCommand
{
public:
    std::string Id() const override { return "sketch.arc"; }
    std::string Title() const override { return "Center Point Arc"; }
    std::string Icon() const override { return ":/icons/sketch.arc.svg"; }
    std::string Shortcut() const override { return "A"; }
    std::string Description() const override
    {
        return "Draw an arc from its centre, start and end";
    }

protected:
    SketchTool& Tool() const override { return ArcTool(); }
};

LCAD_TOOL_COMMAND(TangentArcCommand, SketchToolCommand, "sketch.arc.tangent",
                  "Tangent Arc", TangentArcTool(),
                  "Continue an existing curve with an arc tangent to it");

// The polygon tools need a side count before they can preview anything.
class PolygonCommand : public SketchToolCommand
{
protected:
    bool Prepare(CommandContext& theContext) override
    {
        int sides = PolygonTool().Sides();
        if (!SketchDialogs::AskPolygonSides(theContext.parent, sides)) {
            return false;
        }
        PolygonTool().SetSides(sides);
        return true;
    }

    SketchTool& Tool() const override { return PolygonTool(); }

    virtual SketchPolygonTool& PolygonTool() const = 0;
};

#define LCAD_POLYGON_COMMAND(ClassName, IdText, TitleText, ToolCall, Text)           \
    class ClassName : public PolygonCommand                                           \
    {                                                                                 \
    public:                                                                           \
        std::string Id() const override { return IdText; }                            \
        std::string Title() const override { return TitleText; }                      \
        std::string Icon() const override { return ":/icons/" IdText ".svg"; }         \
        std::string Description() const override { return Text; }                     \
                                                                                      \
    protected:                                                                        \
        SketchPolygonTool& PolygonTool() const override { return ToolCall; }           \
    }

LCAD_POLYGON_COMMAND(CircumscribedPolygonCommand, "sketch.polygon.circumscribed",
                     "Circumscribed Polygon", CircumscribedPolygonTool(),
                     "Draw a regular polygon around a circle, sized by an edge midpoint");
LCAD_POLYGON_COMMAND(InscribedPolygonCommand, "sketch.polygon.inscribed",
                     "Inscribed Polygon", InscribedPolygonTool(),
                     "Draw a regular polygon inside a circle, sized by a vertex");
LCAD_POLYGON_COMMAND(EdgePolygonCommand, "sketch.polygon.edge", "Edge Polygon",
                     EdgePolygonTool(), "Draw a regular polygon from one of its edges");

#undef LCAD_POLYGON_COMMAND

LCAD_TOOL_COMMAND(EllipseCommand, SketchToolCommand, "sketch.ellipse", "Ellipse",
                  EllipseTool(), "Draw an ellipse from its centre and both axes");
LCAD_TOOL_COMMAND(SlotCommand, SketchToolCommand, "sketch.slot", "Center to Center Slot",
                  SlotTool(), "Draw a slot from the two ends of its centre line and a width");
LCAD_TOOL_COMMAND(SplineCommand, SketchToolCommand, "sketch.spline", "Fit Point Spline",
                  SplineTool(),
                  "Draw a spline through fit points; Enter or double-click finishes it");
LCAD_TOOL_COMMAND(ControlPointSplineCommand, SketchToolCommand, "sketch.spline.control_point",
                  "Control Point Spline", ControlPointSplineTool(),
                  "Draw a spline shaped by control points rather than through them");
LCAD_TOOL_COMMAND(ConicCommand, SketchToolCommand, "sketch.conic", "Conic Curve",
                  ConicTool(),
                  "Draw a conic from two ends and a point on it; [ and ] change rho");
LCAD_TOOL_COMMAND(PointCommand, SketchToolCommand, "sketch.point", "Point",
                  PointTool(), "Place a sketch point to constrain other geometry to");

// ---- Modify ----

LCAD_TOOL_COMMAND(FilletCommand, SketchModifyCommand, "sketch.fillet", "Fillet",
                  SketchFilletTool(),
                  "Round the corner between two lines with a tangent arc");

class TrimCommand : public SketchModifyCommand
{
public:
    std::string Id() const override { return "sketch.trim"; }
    std::string Title() const override { return "Trim"; }
    std::string Icon() const override { return ":/icons/sketch.trim.svg"; }
    std::string Shortcut() const override { return "T"; }
    std::string Description() const override
    {
        return "Remove the piece of a curve between the things it crosses";
    }

protected:
    SketchTool& Tool() const override { return SketchTrimTool(); }
};

LCAD_TOOL_COMMAND(ExtendCommand, SketchModifyCommand, "sketch.extend", "Extend",
                  SketchExtendTool(), "Run a curve on until it meets another one");

class OffsetCommand : public SketchModifyCommand
{
public:
    std::string Id() const override { return "sketch.offset"; }
    std::string Title() const override { return "Offset"; }
    std::string Icon() const override { return ":/icons/sketch.offset.svg"; }
    std::string Shortcut() const override { return "O"; }
    std::string Description() const override
    {
        return "Copy a connected chain of curves a set distance to one side";
    }

protected:
    SketchTool& Tool() const override { return SketchOffsetTool(); }
};

LCAD_TOOL_COMMAND(MirrorCommand, SketchSelectionCommand, "sketch.mirror", "Mirror",
                  SketchMirrorTool(), "Mirror the selected sketch geometry about a line");

class RectangularPatternCommand : public SketchSelectionCommand
{
public:
    std::string Id() const override { return "sketch.pattern.rectangular"; }
    std::string Title() const override { return "Rectangular Pattern"; }
    std::string Icon() const override { return ":/icons/sketch.pattern.rectangular.svg"; }
    std::string Description() const override
    {
        return "Copy the selected sketch geometry into a grid";
    }

protected:
    bool Prepare(CommandContext& theContext) override
    {
        int across = 3;
        int down = 1;
        if (!SketchDialogs::AskRectangularPattern(theContext.parent, across, down)) {
            return false;
        }
        SketchRectangularPattern().SetCounts(across, down);
        return true;
    }

    SketchTool& Tool() const override { return SketchRectangularPattern(); }
};

class CircularPatternCommand : public SketchSelectionCommand
{
public:
    std::string Id() const override { return "sketch.pattern.circular"; }
    std::string Title() const override { return "Circular Pattern"; }
    std::string Icon() const override { return ":/icons/sketch.pattern.circular.svg"; }
    std::string Description() const override
    {
        return "Copy the selected sketch geometry around a centre point";
    }

protected:
    bool Prepare(CommandContext& theContext) override
    {
        int count = 6;
        double angle = 360.0;
        if (!SketchDialogs::AskCircularPattern(theContext.parent, count, angle)) {
            return false;
        }
        SketchCircularPattern().SetPattern(count, angle * kPi / 180.0);
        return true;
    }

    SketchTool& Tool() const override { return SketchCircularPattern(); }
};

#undef LCAD_TOOL_COMMAND

// Construction geometry is a property of existing curves rather than a
// tool, so this flips the flag on whatever is selected.
class ToggleConstructionCommand : public Command
{
public:
    std::string Id() const override { return "sketch.construction"; }
    std::string Title() const override { return "Construction"; }
    std::string Group() const override { return kSketchGroup; }
    std::string Section() const override { return kModifySection; }
    std::string Icon() const override { return ":/icons/sketch.construction.svg"; }
    std::string Description() const override
    {
        return "Turn the selected sketch geometry into construction lines, or back again";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        SketchFeature* sketch = SketchSession::Instance().ActiveSketch(theContext.document);
        if (sketch == nullptr) {
            return false;
        }
        SketchSelection::Instance().Prune(*sketch);
        return !SketchSelection::Instance().IsEmpty();
    }

    void Execute(CommandContext& theContext) override
    {
        SketchFeature* sketch = SketchSession::Instance().ActiveSketch(theContext.document);
        if (sketch == nullptr || theContext.document == nullptr) {
            return;
        }

        const std::vector<int> ids = SketchSelection::Instance().EntityIds();
        if (ids.empty()) {
            return;
        }

        // Mixed selections all become construction; a uniform one toggles.
        bool allConstruction = true;
        for (const int id : ids) {
            const SketchEntity* entity = sketch->FindEntity(id);
            if (entity != nullptr && !entity->isConstruction) {
                allConstruction = false;
                break;
            }
        }

        SketchDisplay::Instance().Attach(theContext);
        theContext.document->PushUndoSnapshot();
        for (const int id : ids) {
            if (SketchEntity* entity = sketch->FindEntity(id)) {
                entity->isConstruction = !allConstruction;
            }
        }
        theContext.document->Rebuild();
        ShowStatus(theContext, allConstruction ? "Back to normal geometry."
                                               : "Now construction geometry.");
    }
};

} // namespace

void RegisterSketchCommands(CommandRegistry& theRegistry)
{
    // The Sketch tab appears only while a sketch is open, and disappears
    // again on Finish -- exactly how Fusion's contextual tab behaves.
    theRegistry.SetGroupVisibility(kSketchGroup, [](const CommandContext&) {
        return SketchSession::Instance().IsActive();
    });

    // Leading section, no caption: entering and leaving sketch mode, the
    // way Fusion keeps Create Sketch and Finish Sketch apart from the
    // tools themselves.
    theRegistry.Add(std::make_unique<CreateSketchCommand>());
    theRegistry.Add(std::make_unique<EditSketchCommand>());
    theRegistry.Add(std::make_unique<FinishSketchCommand>());
    theRegistry.Add(std::make_unique<ShowSketchesCommand>());
    theRegistry.Add(std::make_unique<ShowConstraintsCommand>());

    // CREATE
    theRegistry.Add(std::make_unique<LineToolCommand>());
    theRegistry.Add(std::make_unique<RectangleToolCommand>());
    theRegistry.Add(std::make_unique<CentreRectangleCommand>());
    theRegistry.Add(std::make_unique<ThreePointRectangleCommand>());
    theRegistry.Add(std::make_unique<CircleToolCommand>());
    theRegistry.Add(std::make_unique<TwoPointCircleCommand>());
    theRegistry.Add(std::make_unique<ThreePointCircleCommand>());
    theRegistry.Add(std::make_unique<ThreePointArcCommand>());
    theRegistry.Add(std::make_unique<ArcToolCommand>());
    theRegistry.Add(std::make_unique<TangentArcCommand>());
    theRegistry.Add(std::make_unique<CircumscribedPolygonCommand>());
    theRegistry.Add(std::make_unique<InscribedPolygonCommand>());
    theRegistry.Add(std::make_unique<EdgePolygonCommand>());
    theRegistry.Add(std::make_unique<EllipseCommand>());
    theRegistry.Add(std::make_unique<SlotCommand>());
    theRegistry.Add(std::make_unique<SplineCommand>());
    theRegistry.Add(std::make_unique<ControlPointSplineCommand>());
    theRegistry.Add(std::make_unique<ConicCommand>());
    theRegistry.Add(std::make_unique<PointCommand>());

    // MODIFY
    theRegistry.Add(std::make_unique<FilletCommand>());
    theRegistry.Add(std::make_unique<TrimCommand>());
    theRegistry.Add(std::make_unique<ExtendCommand>());
    theRegistry.Add(std::make_unique<OffsetCommand>());
    theRegistry.Add(std::make_unique<MirrorCommand>());
    theRegistry.Add(std::make_unique<RectangularPatternCommand>());
    theRegistry.Add(std::make_unique<CircularPatternCommand>());
    theRegistry.Add(std::make_unique<ToggleConstructionCommand>());

    // CONSTRAINTS and INSPECT
    AddSketchConstraintCommands(theRegistry);
}

} // namespace lcad
