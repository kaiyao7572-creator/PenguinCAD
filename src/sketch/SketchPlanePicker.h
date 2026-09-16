#pragma once

#include "core/Command.h"
#include "core/ViewportInteraction.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <AIS_Shape.hxx>
#include <TColStd_ListOfInteger.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>

#include <functional>
#include <vector>

namespace lcad {

// The gp_Ax3 a planar face should be sketched on, with the normal pointing
// the way the face actually faces -- the direction an extrude off it wants
// to go. Returns false for anything that isn't a planar face.
//
// Shared because two paths need to agree on it: picking a face in the
// viewport, and the shortcut that skips the pick when one is already
// selected.
bool SketchPlaneFromFace(const TopoDS_Shape& theShape, gp_Ax3& theResult);

// Fusion's first sketch step, and the reason there is no dialog: click
// Create Sketch and the three origin planes appear as translucent
// rectangles at the origin, every planar face of the model becomes
// pickable alongside them, and you click the one you want.
//
// Nothing is created until a plane is picked, so Escape genuinely leaves
// the document untouched.
class SketchPlanePicker : public ViewportInteraction
{
public:
    // Called once, with the plane the user committed to, after the pickers
    // have already been cleaned up.
    using Handler = std::function<void(const gp_Ax3&)>;

    static SketchPlanePicker& Instance();

    void Start(const CommandContext& theContext, Handler theOnPicked);

    // Tear the pick down without creating anything. Safe when idle.
    void Stop();

    bool IsRunning() const { return myIsRunning; }

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

private:
    SketchPlanePicker() = default;

    // One of the three origin rectangles and the frame it stands for.
    struct Candidate
    {
        Handle(AIS_Shape) object;
        gp_Ax3            position;
    };

    // What an existing object's selection looked like before we switched
    // it to faces, so it can be put back exactly.
    struct SelectionState
    {
        Handle(AIS_InteractiveObject) object;
        TColStd_ListOfInteger         modes;
    };

    void ShowPlanes();
    void HidePlanes();
    void ActivateModelFaces();
    void RestoreModelSelection();

    // Which plane is under thePos, if any. Also refreshes OCCT's dynamic
    // highlighting, which is what makes hover work.
    bool Resolve(const Graphic3d_Vec2i& thePos, gp_Ax3& theResult);

    CommandContext                myContext;
    Handler                       myOnPicked;
    std::vector<Candidate>        myPlanes;
    std::vector<SelectionState>   myRestore;
    bool                          myIsRunning     = false;
    bool                          myPressConsumed = false;
};

} // namespace lcad
