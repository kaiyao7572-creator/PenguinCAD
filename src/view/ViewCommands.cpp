#include "core/Command.h"
#include "core/Document.h"
#include "core/Registration.h"
#include "core/ViewportInteraction.h"

#include "OcctViewport.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <AIS_Shape.hxx>
#include <AIS_ViewCube.hxx>
#include <AIS_AnimationCamera.hxx>
#include <AIS_DisplayMode.hxx>
#include <Aspect_Grid.hxx>
#include <Aspect_TypeOfTriedronPosition.hxx>
#include <Graphic3d_Camera.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Graphic3d_TransModeFlags.hxx>
#include <Graphic3d_Vec2.hxx>
#include <Prs3d_Drawer.hxx>
#include <V3d_TypeOfOrientation.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include <memory>
#include <string>
#include <utility>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

const char* const kViewGroup = "View";

// ---- display mode (Shaded / Wireframe / Shaded+Edges) ----
//
// MainWindow::redisplayDocument() throws away the old AIS_Shape and builds
// a brand new one -- always AIS_Shaded -- on every document change. A
// chosen Wireframe/Shaded+Edges preference would silently revert the
// instant the user edited anything unless something re-asserts it, so this
// registers itself as a DocumentObserver (a public, intended extension
// point) and reapplies the current mode after every rebuild.
class DisplayModeController : public DocumentObserver
{
public:
    enum class Mode { Shaded, Wireframe, ShadedWithEdges };

    static DisplayModeController& Instance()
    {
        static DisplayModeController theInstance;
        return theInstance;
    }

    Mode Current() const { return myMode; }

    void SetMode(const CommandContext& theContext, Mode theMode)
    {
        EnsureObserving(theContext);
        myMode = theMode;
        Apply(theContext.AisContext());
    }

    void OnDocumentChanged(Document& /*theDocument*/) override { Apply(myContext.AisContext()); }

private:
    DisplayModeController() = default;

    void EnsureObserving(const CommandContext& theContext)
    {
        myContext = theContext;
        if (myObserving || theContext.document == nullptr) {
            return;
        }
        theContext.document->AddObserver(this);
        myObserving = true;
    }

    void Apply(const Handle(AIS_InteractiveContext)& theAisContext)
    {
        if (theAisContext.IsNull()) {
            return;
        }

        const Standard_Integer dispMode = (myMode == Mode::Wireframe) ? AIS_WireFrame : AIS_Shaded;
        const Standard_Boolean boundaries = (myMode == Mode::ShadedWithEdges);

        // The context-wide default only benefits objects displayed from now
        // on; walking what's already displayed makes the change visible
        // immediately too (and re-asserting both here covers a freshly
        // rebuilt AIS_Shape either way).
        if (!theAisContext->DefaultDrawer().IsNull()) {
            theAisContext->DefaultDrawer()->SetFaceBoundaryDraw(boundaries);
        }
        theAisContext->SetDisplayMode(dispMode, Standard_False);

        AIS_ListOfInteractive objects;
        theAisContext->DisplayedObjects(objects);
        for (AIS_ListOfInteractive::Iterator it(objects); it.More(); it.Next()) {
            const Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(it.Value());
            if (shape.IsNull()) {
                continue;   // leave the ground grid / origin axes alone
            }
            if (!shape->Attributes().IsNull()) {
                shape->Attributes()->SetFaceBoundaryDraw(boundaries);
            }
            theAisContext->SetDisplayMode(shape, dispMode, Standard_False);
            theAisContext->Redisplay(shape, Standard_False);
        }
        theAisContext->UpdateCurrentViewer();
    }

    CommandContext myContext;
    Mode           myMode       = Mode::Shaded;
    bool           myObserving  = false;
};

// ---- view cube ----
//
// AIS_ViewCube only detects clicks (it deliberately never enters the
// context's normal "selected" list -- see its header comment), so the app
// is expected to notice the click itself and hand the detected owner back
// to HandleClick(). This is that noticing: a ViewportInteraction that sits
// at the BOTTOM of the interaction stack for the app's lifetime. Tools
// push above it, get first refusal on every event, and anything they do
// not consume falls through to here -- so the cube keeps working while a
// tool is up rather than going quiet.
//
// It reports IsExclusive() == false because it is not a tool the user
// started: it is always pushed. Tools asking "is the user in the middle of
// something?" must not be answered "yes, the view cube".
class ViewCubeInteraction : public ViewportInteraction
{
public:
    // Permanently pushed background handler, not a tool -- see above.
    bool IsExclusive() const override { return false; }

    static ViewCubeInteraction& Instance()
    {
        static ViewCubeInteraction theInstance;
        return theInstance;
    }

    bool IsVisible() const { return myVisible; }

    // There is no "on startup" hook src/view/ is allowed to reach (that's
    // MainWindow, off limits, and it builds the ribbon before the GL
    // window's first expose event anyway, so the AIS context is still null
    // at that point). IsEnabled()/IsChecked() are polled constantly once
    // the app is alive, so this is called from both of them to lazily
    // create and show the cube -- shown by default -- the moment a real
    // AIS context exists; it quietly no-ops and gets retried on the next
    // poll until then.
    void EnsureMaterialized(const CommandContext& theContext)
    {
        if (myMaterialized) {
            return;
        }
        const Handle(AIS_InteractiveContext) aisContext = theContext.AisContext();
        const Handle(V3d_View) view = theContext.View();
        if (aisContext.IsNull() || view.IsNull()) {
            return;
        }

        myContext = theContext;

        myCube = new AIS_ViewCube();
        myCube->SetViewAnimation(new AIS_AnimationCamera("ViewCubeAnimation", view));
        myCube->SetFixedAnimationLoop(false);   // yield frames instead of jumping to the end
        myCube->SetTransformPersistence(
            new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER,
                                        Graphic3d_Vec2i(85, 85)));

        if (myVisible) {
            aisContext->Display(myCube, Standard_False);
        }
        if (theContext.viewport != nullptr) {
            theContext.viewport->PushInteraction(this);
        }
        myMaterialized = true;
    }

    void SetVisible(const CommandContext& theContext, bool theVisible)
    {
        EnsureMaterialized(theContext);
        myContext = theContext;
        myVisible = theVisible;

        const Handle(AIS_InteractiveContext) aisContext = theContext.AisContext();
        if (aisContext.IsNull() || myCube.IsNull()) {
            return;
        }
        if (myVisible) {
            aisContext->Display(myCube, Standard_False);
        } else {
            aisContext->Erase(myCube, Standard_False);
        }
    }

    bool OnMousePress(const Graphic3d_Vec2i& thePos, Qt::MouseButton theButton,
                      Qt::KeyboardModifiers theModifiers) override
    {
        (void)thePos;
        (void)theModifiers;
        myPressOnCube = false;
        if (!myVisible || theButton != Qt::LeftButton) {
            return false;
        }
        if (DetectedCubeOwner().IsNull()) {
            return false;   // let the click fall through to normal select/orbit
        }
        myPressOnCube = true;
        return true;
    }

    bool OnMouseRelease(const Graphic3d_Vec2i& thePos, Qt::MouseButton theButton,
                        Qt::KeyboardModifiers theModifiers) override
    {
        (void)thePos;
        (void)theModifiers;
        // The press was consumed above whenever this fires with
        // myPressOnCube set, so the viewport's own controller never saw the
        // button go down -- handing it the release would leave it in a
        // state it can't make sense of (same reasoning the sketch tools'
        // OnMouseRelease documents).
        if (!myPressOnCube) {
            return false;
        }
        myPressOnCube = false;
        if (theButton != Qt::LeftButton) {
            return true;
        }

        const Handle(AIS_ViewCubeOwner) owner = DetectedCubeOwner();
        if (!owner.IsNull() && !myCube.IsNull()) {
            // Straight out of AIS_ViewCube.hxx's own usage note: the app
            // detects the click and hands the owner back in; HandleClick()
            // runs the camera animation to the new orientation itself.
            myCube->HandleClick(owner);
            myContext.Redraw();
        }
        return true;
    }

private:
    ViewCubeInteraction() = default;

    Handle(AIS_ViewCubeOwner) DetectedCubeOwner() const
    {
        const Handle(AIS_InteractiveContext) ctx = myContext.AisContext();
        if (ctx.IsNull()) {
            return Handle(AIS_ViewCubeOwner)();
        }
        return Handle(AIS_ViewCubeOwner)::DownCast(ctx->DetectedOwner());
    }

    CommandContext       myContext;
    Handle(AIS_ViewCube) myCube;
    bool                 myVisible      = true;   // shown by default
    bool                 myMaterialized = false;
    bool                 myPressOnCube  = false;
};

// ---- commands ----

// One command per standard orientation; they differ only in id/label/icon
// and which V3d_TypeOfOrientation they apply, so a single parameterized
// class stands in for all seven (same idea as SketchToolCommand).
class StandardViewCommand : public Command
{
public:
    StandardViewCommand(std::string theId, std::string theTitle, std::string theIcon,
                        std::string theShortcut, std::string theDescription,
                        V3d_TypeOfOrientation theOrientation)
        : myId(std::move(theId)),
          myTitle(std::move(theTitle)),
          myIcon(std::move(theIcon)),
          myShortcut(std::move(theShortcut)),
          myDescription(std::move(theDescription)),
          myOrientation(theOrientation)
    {
    }

    std::string Id() const override { return myId; }
    std::string Title() const override { return myTitle; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Orientation"; }
    std::string Icon() const override { return myIcon; }
    std::string Shortcut() const override { return myShortcut; }
    std::string Description() const override { return myDescription; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return !theContext.View().IsNull();
    }

    void Execute(CommandContext& theContext) override
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull()) {
            return;
        }
        view->SetProj(myOrientation);
        // OcctViewport::FitAll() already does FitAll + ZFitAll + Redraw --
        // the exact sequence asked for -- so lean on it instead of
        // reimplementing it.
        if (theContext.viewport != nullptr) {
            theContext.viewport->FitAll();
        }
    }

private:
    std::string           myId;
    std::string           myTitle;
    std::string           myIcon;
    std::string           myShortcut;
    std::string           myDescription;
    V3d_TypeOfOrientation myOrientation;
};

class FitAllCommand : public Command
{
public:
    std::string Id() const override { return "view.fit_all"; }
    std::string Title() const override { return "Fit All"; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Orientation"; }
    std::string Icon() const override { return "🔍"; }
    // F6, as in Fusion. F belongs to Fillet, and a key bound twice fires neither.
    std::string Shortcut() const override { return "F6"; }
    std::string Description() const override { return "Frame the entire model in the viewport"; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return !theContext.View().IsNull();
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.viewport != nullptr) {
            theContext.viewport->FitAll();
        }
    }
};

class DisplayModeCommand : public Command
{
public:
    DisplayModeCommand(std::string theId, std::string theTitle, std::string theIcon,
                       std::string theDescription, DisplayModeController::Mode theMode)
        : myId(std::move(theId)),
          myTitle(std::move(theTitle)),
          myIcon(std::move(theIcon)),
          myDescription(std::move(theDescription)),
          myMode(theMode)
    {
    }

    std::string Id() const override { return myId; }
    std::string Title() const override { return myTitle; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Display"; }
    std::string Icon() const override { return myIcon; }
    std::string Description() const override { return myDescription; }

    bool IsCheckable() const override { return true; }
    bool IsChecked(const CommandContext& theContext) const override
    {
        (void)theContext;
        return DisplayModeController::Instance().Current() == myMode;
    }

    void Execute(CommandContext& theContext) override
    {
        DisplayModeController::Instance().SetMode(theContext, myMode);
        theContext.Redraw();
    }

private:
    std::string                 myId;
    std::string                 myTitle;
    std::string                 myIcon;
    std::string                 myDescription;
    DisplayModeController::Mode myMode;
};

class GridToggleCommand : public Command
{
public:
    std::string Id() const override { return "view.grid_toggle"; }
    std::string Title() const override { return "Grid"; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Show"; }
    std::string Icon() const override { return "🔲"; }
    std::string Shortcut() const override { return "G"; }
    std::string Description() const override { return "Show or hide the ground grid"; }

    bool IsCheckable() const override { return true; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return !theContext.View().IsNull();
    }

    bool IsChecked(const CommandContext& theContext) const override
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull() || view->Viewer().IsNull()) {
            return false;
        }
        return view->Viewer()->IsGridActive();
    }

    void Execute(CommandContext& theContext) override
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull() || view->Viewer().IsNull()) {
            return;
        }
        const Handle(V3d_Viewer) viewer = view->Viewer();
        if (viewer->IsGridActive()) {
            viewer->DeactivateGrid();
        } else {
            // Type/draw mode chosen to match what OcctViewport activates at
            // startup -- this only ever flips the same grid on and off.
            viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
        }
        theContext.Redraw();
    }
};

class ProjectionToggleCommand : public Command
{
public:
    std::string Id() const override { return "view.projection_toggle"; }
    std::string Title() const override { return "Perspective"; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Display"; }
    std::string Icon() const override { return "🎥"; }
    std::string Shortcut() const override { return "P"; }
    std::string Description() const override
    {
        return "Toggle between perspective and orthographic projection";
    }

    bool IsCheckable() const override { return true; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return !theContext.View().IsNull();
    }

    bool IsChecked(const CommandContext& theContext) const override
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull() || view->Camera().IsNull()) {
            return false;
        }
        return !view->Camera()->IsOrthographic();
    }

    void Execute(CommandContext& theContext) override
    {
        const Handle(V3d_View) view = theContext.View();
        if (view.IsNull() || view->Camera().IsNull()) {
            return;
        }
        const bool isOrthographic = view->Camera()->IsOrthographic();
        view->Camera()->SetProjectionType(isOrthographic ? Graphic3d_Camera::Projection_Perspective
                                                          : Graphic3d_Camera::Projection_Orthographic);
        theContext.Redraw();
    }
};

class ViewCubeCommand : public Command
{
public:
    std::string Id() const override { return "view.view_cube"; }
    std::string Title() const override { return "View Cube"; }
    std::string Group() const override { return kViewGroup; }
    std::string Section() const override { return "Show"; }
    std::string Icon() const override { return "🎲"; }
    std::string Description() const override
    {
        return "Show or hide the corner navigation cube (click a face, edge or "
               "corner to orient the view)";
    }

    bool IsCheckable() const override { return true; }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        ViewCubeInteraction::Instance().EnsureMaterialized(theContext);
        return theContext.viewport != nullptr;
    }

    bool IsChecked(const CommandContext& theContext) const override
    {
        ViewCubeInteraction::Instance().EnsureMaterialized(theContext);
        return ViewCubeInteraction::Instance().IsVisible();
    }

    void Execute(CommandContext& theContext) override
    {
        ViewCubeInteraction& cube = ViewCubeInteraction::Instance();
        cube.SetVisible(theContext, !cube.IsVisible());
        theContext.Redraw();
    }
};

} // namespace

void RegisterViewCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.front", "Front", "⬆️", "Ctrl+1", "Look straight at the front (XZ) of the model",
        V3d_TypeOfOrientation_Zup_Front));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.back", "Back", "⬇️", "Ctrl+2", "Look straight at the back of the model",
        V3d_TypeOfOrientation_Zup_Back));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.left", "Left", "⬅️", "Ctrl+3", "Look straight at the left side of the model",
        V3d_TypeOfOrientation_Zup_Left));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.right", "Right", "➡️", "Ctrl+4", "Look straight at the right side (YZ) of the model",
        V3d_TypeOfOrientation_Zup_Right));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.top", "Top", "🔼", "Ctrl+5", "Look straight down at the top (XY) of the model",
        V3d_TypeOfOrientation_Zup_Top));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.bottom", "Bottom", "🔽", "Ctrl+6", "Look straight up at the bottom of the model",
        V3d_TypeOfOrientation_Zup_Bottom));
    theRegistry.Add(std::make_unique<StandardViewCommand>(
        "view.isometric", "Isometric", "🏠", "Home",
        "Return to the default home/isometric view", V3d_TypeOfOrientation_Zup_AxoRight));

    theRegistry.Add(std::make_unique<FitAllCommand>());

    theRegistry.Add(std::make_unique<DisplayModeCommand>(
        "view.display_shaded", "Shaded", "🧱", "Solid shaded display with no face boundaries",
        DisplayModeController::Mode::Shaded));
    theRegistry.Add(std::make_unique<DisplayModeCommand>(
        "view.display_wireframe", "Wireframe", "🕸", "Edges only, no shaded surfaces",
        DisplayModeController::Mode::Wireframe));
    theRegistry.Add(std::make_unique<DisplayModeCommand>(
        "view.display_shaded_edges", "Shaded + Edges", "🔷",
        "Shaded surfaces with face boundaries drawn on top",
        DisplayModeController::Mode::ShadedWithEdges));

    theRegistry.Add(std::make_unique<GridToggleCommand>());
    theRegistry.Add(std::make_unique<ProjectionToggleCommand>());
    theRegistry.Add(std::make_unique<ViewCubeCommand>());

    // Lives in its own file but belongs on the same tab.
    RegisterUnitsCommand(theRegistry);
}

} // namespace lcad
