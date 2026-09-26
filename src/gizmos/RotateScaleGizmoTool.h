#pragma once

#include "core/Command.h"
#include "core/Document.h"
#include "core/ViewportInteraction.h"
#include "features/CombineFeature.h"
#include "gizmos/RotateScaleGizmo.h"

#include <AIS_InteractiveObject.hxx>
#include <AIS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>

#include <memory>
#include <string>
#include <vector>

namespace lcad {

class TransformFeature;

// Fusion's rotate and scale manipulators. Select a body and three rings
// appear at its centroid, one per world axis and a fixed size on screen
// at every zoom, as Fusion's are; drag one and the body turns about
// that axis THROUGH ITS OWN CENTROID, with the angle counting up in the
// status bar. Scale is the same tool in its other mode: one handle
// standing off the centroid, dragged out to grow the body and in to
// shrink it. Let go and the drag lands on the timeline as a
// TransformFeature with the angle or the factor you dragged -- undoable,
// and re-editable in the properties panel exactly like a typed-in one.
// A drag that never reaches the timeline is not a parametric edit, it is
// a rumour about one.
//
// One class for both, because the two differ only in what a drag MEANS:
// the selection, the pivot, the hit test, the preview and the commit are
// the same code, and a second class would be that code again with one
// word changed.
//
// Drawn by hand rather than with AIS_Manipulator, which the move gizmo
// uses, for two reasons that both matter here. Its rings are positioned
// by Attach() at the centre of the bounding BOX, not the centroid, so a
// rotation about them is not the rotation this tool advertises. And its
// scaling mode has no route to a timeline entry that is checked before it
// is applied -- a zero or negative factor has to be refused, and a
// manipulator that has already moved the object cannot refuse anything.
//
// The maths is in RotateScaleGizmo.h, free of AIS and Qt so it can be
// tested headlessly -- the hit test and the drag included, driven by the
// view's camera. What is left here is reading that camera off the live
// view, and the AIS bookkeeping.
//
// ONE-SHOT: a committed drag ends the tool. A rotate changes where the
// body is and a scale changes how big it is, so the ref and the pivot
// this tool is holding both describe a body that no longer exists -- and
// re-deriving them from a document that has just renumbered its bodies is
// guesswork of exactly the kind this codebase refuses elsewhere. Invoking
// the command again re-reads the selection and is honest about it.
//
// One instance for the whole app, same reasoning as MoveGizmoTool: the
// viewport keeps a bare pointer to whichever handler is pushed and does
// not own it, so this must outlive being popped.
class RotateScaleGizmoTool : public ViewportInteraction, public DocumentObserver
{
public:
    enum class Mode
    {
        Rotate,
        Scale
    };

    static RotateScaleGizmoTool& Instance();

    // Attaches to the current viewport selection and takes over the
    // mouse. With nothing selected it shows a status hint and does
    // nothing, so Execute() can call it unconditionally.
    void Start(const CommandContext& theContext, Mode theMode);

    // Drops the handles and hands the viewport back. Safe when not running.
    void Stop();

    bool IsActive() const { return myIsActive; }
    Mode ActiveMode() const { return myMode; }

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
    // Any rebuild -- an undo, an edit upstream, a body deleted -- moves or
    // destroys the body the handles are wrapped round, so they go. Our own
    // commit is excluded: it ends the tool deliberately rather than through
    // this door.
    void OnDocumentChanged(Document& theDocument) override;

private:
    RotateScaleGizmoTool() = default;

    bool AttachToSelection();
    void BuildHandles();
    void ResetHandles();
    void ClearHandles();
    void Highlight(int theHandle);

    // Screen <-> model, from the live view. Everything past this is the
    // headless maths in RotateScaleGizmo.h.
    GizmoScreen Screen() const;
    double DevicePixels(double theLogicalPixels) const;

    // How far the handles stand off the pivot, in MODEL units at the zoom
    // the view is at right now. The handles are drawn a fixed size on
    // screen, so this changes with every wheel notch and is never cached.
    double Reach() const;

    int HandleUnder(const Graphic3d_Vec2i& thePos) const;

    gp_Ax1 ScaleAxis() const;

    // The scale handle at theFactor times its rest length, built in device
    // pixels about the ORIGIN: its zoom persistence carries it to the
    // pivot and holds it at that size on screen.
    TopoDS_Shape ScaleHandleShape(double theFactor) const;

    // The feature this drag would commit right now. ONE function, used by
    // both the preview and the commit, so what the drag shows and what
    // release lands cannot drift apart.
    std::shared_ptr<TransformFeature> MakeFeature() const;

    void UpdatePreview();
    void ClearPreview();
    void HideBodies();
    void RestoreBodies();

    void EndDrag(bool theApply);
    void Commit();

    CommandContext myContext;
    Mode           myMode = Mode::Rotate;

    // Null for a body with no volume (a surface, a wire): the transform
    // then acts on the whole model, which is what it always did, and the
    // status bar says so rather than letting it surprise anyone.
    CombineBodyRef myBody;
    std::string    myBodyName;
    gp_Pnt         myPivot;

    std::vector<Handle(AIS_Shape)>             myHandles;
    Handle(AIS_Shape)                          myPreviewObject;
    std::vector<Handle(AIS_InteractiveObject)> myHiddenBodies;

    bool myIsActive    = false;
    bool myIsDragging  = false;
    int  myHoverHandle = -1;
    int  myDragHandle  = -1;   // 0/1/2 = world X/Y/Z for a ring; 0 for the scale handle

    // The drag itself -- pixels to angle or factor -- is the headless half,
    // so a test can push a drag through exactly what this tool runs.
    RingDrag  myRingDrag;
    ScaleDrag myScaleDrag;

    // Set while we ask MainWindow to redisplay bodies we hid, and while
    // our own commit is going through. Both come straight back as
    // OnDocumentChanged(), and neither is "the model changed under us".
    bool myIsRestoring  = false;
    bool myIsCommitting = false;
};

} // namespace lcad
