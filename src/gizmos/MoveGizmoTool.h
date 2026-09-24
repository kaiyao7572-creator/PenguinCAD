#pragma once

#include "core/Command.h"
#include "core/Document.h"
#include "core/ViewportInteraction.h"
#include "features/CombineFeature.h"

#include <AIS_Manipulator.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

namespace lcad {

// Interactive move/rotate gizmo built on OCCT's AIS_Manipulator. "Move"
// attaches it to whatever is currently selected in the viewport; dragging
// a handle previews the transform live and, on release, lands it on the
// timeline as a TransformFeature -- so a gizmo drag is exactly as
// undoable and as editable afterwards (properties panel) as any other
// feature, never just a throwaway visual nudge.
//
// One instance for the whole app (see Instance()), matching how the
// sketch tools do it: the viewport keeps a bare pointer to whichever
// handler is pushed and does not own it, so this has to outlive being
// popped -- see ARCHITECTURE.md's ViewportInteraction lifetime note.
class MoveGizmoTool : public ViewportInteraction, public DocumentObserver
{
public:
    static MoveGizmoTool& Instance();

    // Attaches to the current AIS selection and takes over the viewport.
    // If nothing is selected, shows a status hint and does nothing --
    // Execute() can call this unconditionally.
    void Start(const CommandContext& theContext);

    // Detaches the manipulator and hands the viewport back. Safe to call
    // when not running.
    void Stop();

    bool IsActive() const { return myIsAttached; }

    // ---- ViewportInteraction ----
    bool OnMousePress(const Graphic3d_Vec2i& thePos,
                      Qt::MouseButton        theButton,
                      Qt::KeyboardModifiers  theModifiers) override;
    bool OnMouseMove(const Graphic3d_Vec2i& thePos,
                     Qt::MouseButtons       theButtons,
                     Qt::KeyboardModifiers  theModifiers) override;
    bool OnMouseRelease(const Graphic3d_Vec2i& thePos,
                        Qt::MouseButton        theButton,
                        Qt::KeyboardModifiers  theModifiers) override;
    bool OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers) override;
    void OnDeactivated() override;

    // ---- DocumentObserver ----
    // MainWindow displays one AIS_Shape per body, and removes every one of
    // them and creates fresh ones on every rebuild -- including the one our
    // own committed drag triggers -- so the object we were attached to is
    // gone the instant that happens. Re-find and re-attach to the new one
    // here; if there's no body left, detach gracefully.
    void OnDocumentChanged(Document& theDocument) override;

private:
    MoveGizmoTool() = default;

    void AttachTo(const Handle(AIS_InteractiveObject)& theObject);
    void Detach();
    void EndDrag(bool theApply);
    void CommitTransform(const gp_Trsf& theTrsf);

    CommandContext          myContext;
    Handle(AIS_Manipulator) myManipulator;

    bool    myIsAttached   = false;
    bool    myIsDragging   = false;
    bool    myDidTransform = false;
    gp_Trsf myLastTrsf;

    // Snapshotted at the start of each drag: StopTransform()/Attach() can
    // both move the manipulator afterward (it follows the object it just
    // transformed), so reading these back out at drag-end would no longer
    // describe the drag that actually happened.
    AIS_ManipulatorMode myDragMode  = AIS_MM_None;
    Standard_Integer    myDragAxis  = -1;
    gp_Pnt               myDragPivot;

    // Which body the manipulator is sitting on, resolved when we attach.
    // Without it the committed TransformFeature has no target and moves
    // the WHOLE upstream shape -- invisible while a document holds one
    // body, and every body at once as soon as Combine or a pattern has
    // made a second.
    CombineBodyRef myTargetRef;
};

} // namespace lcad
