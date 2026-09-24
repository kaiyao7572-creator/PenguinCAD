#pragma once

#include "core/Command.h"
#include "core/Document.h"
#include "core/GeometryRef.h"
#include "core/ViewportInteraction.h"
#include "gizmos/PressPullGizmo.h"

#include <AIS_InteractiveObject.hxx>
#include <AIS_Shape.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>

#include <string>
#include <vector>

namespace lcad {

// Fusion's Press/Pull as it actually feels: select one face and an arrow
// appears on it, pointing out of the solid. Pull the arrow and the body
// grows under the cursor; push it in and the body is cut. Let go and the
// drag lands on the timeline as a PressPullFeature with the distance you
// dragged -- so it is undoable and re-editable in the properties panel
// exactly like the typed-in version, never a throwaway visual nudge.
//
// The arrow is drawn by hand rather than with AIS_Manipulator, which only
// translates whole objects along its own axes: what has to move here is
// one FACE, along a direction the face itself decides. The maths that
// falls out of that lives in PressPullGizmo.h, free of AIS and Qt so it
// can be tested headlessly.
//
// One instance for the whole app, same reasoning as MoveGizmoTool: the
// viewport keeps a bare pointer to whichever handler is pushed and does
// not own it, so this must outlive being popped.
class PressPullGizmoTool : public ViewportInteraction, public DocumentObserver
{
public:
    static PressPullGizmoTool& Instance();

    // Show the arrow when exactly one face is selected, drop it when that
    // stops being true. Called from a command's IsEnabled(), which the
    // shell polls both on a timer and immediately on every selection
    // change -- the view cube materialises itself the same way, and for
    // the same reason: src/gizmos/ has no startup hook of its own and the
    // viewport's single selection callback already belongs to MainWindow.
    void Sync(const CommandContext& theContext);

    // Turn the drag arrow on and off. Selecting a face must NOT summon it
    // by itself: a click on a surface means "I am pointing at this", and
    // answering that with a live manipulator takes a plain pick and turns
    // it into a modelling tool the user never asked for. Fusion shows the
    // arrow once Press Pull is invoked, not on selection, and so do we.
    void Start(const CommandContext& theContext);
    void Stop();

    // Whether the user has asked for the arrow at all. Sync() refuses to
    // arm until this is true, whatever is selected.
    bool IsEnabled() const { return myIsEnabled; }

    bool IsArmed() const { return myIsArmed; }

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
    // Any rebuild -- our own commit, an undo, an edit upstream -- moves or
    // destroys the face the arrow is standing on, so the arrow goes and
    // Sync() decides afresh whether a new one is due.
    void OnDocumentChanged(Document& theDocument) override;

private:
    PressPullGizmoTool() = default;

    void Arm(const GeometryRef& theFace, const PressPullArrow& theArrow);
    void Disarm();

    // Screen <-> model. The only two steps that need a live V3d_View, and
    // the reason they are here rather than in PressPullGizmo.h.
    bool RayThrough(const Graphic3d_Vec2i& thePos, gp_Lin& theRay) const;
    bool IsOnArrow(const Graphic3d_Vec2i& thePos) const;

    void MoveArrowTo(double theDistance);
    void UpdatePreview(double theDistance);
    void ClearPreview();

    void HideBodies();
    void RestoreBodies();

    void EndDrag(bool theApply);
    void Commit();

    CommandContext myContext;

    GeometryRef    myFace;      // the face the arrow belongs to
    std::string    myFaceKey;   // its encoding, to notice when the pick changes
    PressPullArrow myArrow;

    Handle(AIS_Shape) myArrowObject;
    Handle(AIS_Shape) myPreviewObject;
    std::vector<Handle(AIS_InteractiveObject)> myHiddenBodies;

    bool   myIsEnabled   = false;   // the user asked for the arrow
    bool   myIsArmed     = false;
    bool   myIsDragging  = false;
    bool   myIsHovering  = false;
    gp_Pnt myDragStart;          // where on the axis the press landed
    double myDistance    = 0.0;

    // Set while we ask MainWindow to redisplay the bodies we hid. That
    // notification comes straight back as OnDocumentChanged() and a
    // command-state refresh, and neither should be read as "the model
    // changed under us" -- it is us.
    bool myIsRestoring = false;
};

} // namespace lcad
