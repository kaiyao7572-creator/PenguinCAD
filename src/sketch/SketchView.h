#pragma once

#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <Quantity_Color.hxx>
#include <V3d_View.hxx>
#include <gp_Ax3.hxx>

#include <vector>

namespace lcad {

struct CommandContext;

// Everything about the VIEWPORT that changes when a sketch opens, as
// opposed to what gets drawn in it -- that is SketchDisplay's job.
//
// Fusion does three things the instant you commit to a plane: it swings
// the camera round until you are looking straight down at the plane, it
// drops the construction grid onto that plane, and it fades the rest of
// the model back so the sketch is what you see. All three have to be
// undone on Finish Sketch, so they live together behind one switch
// instead of being scattered through the commands.
class SketchView
{
public:
    static SketchView& Instance();

    // Look straight down theSketchPlane, put the grid on it, and push the
    // rest of the model into the background.
    void Enter(const CommandContext& theContext, const gp_Ax3& theSketchPlane);

    // Back to a free 3D orbit over the world XY grid. Safe to call when
    // no sketch was ever entered.
    void Leave(const CommandContext& theContext);

    bool IsActive() const { return myIsActive; }

    // MainWindow throws the model's AIS_Shape away and builds a new one on
    // every document change, so the fade has to be re-applied afterwards
    // or it silently disappears the first time a line is drawn.
    void RefreshDimming();

    // Move the grid onto theSketchPlane without touching the camera. This
    // is what keeps the grid under a sketch whose Offset was typed into
    // the properties panel after the fact; swinging the camera on every
    // keystroke would be unusable. No-op outside sketch mode.
    void Realign(const gp_Ax3& theSketchPlane);

private:
    SketchView() = default;

    void LookAt(const gp_Ax3& thePlane);
    void Undim();

    // In a sketch the grid is paper, not content: it has to sit far enough
    // under the curves that the eye reads the sketch first. Outside a
    // sketch it is the only thing giving the empty viewport a sense of
    // scale, so the viewport's own choice is restored on the way out
    // rather than a guess at what it was.
    void FadeGrid();
    void RestoreGrid();

    // An object we faded, plus the transparency it had before we did, so
    // Finish Sketch puts back what the user actually had rather than an
    // assumed zero.
    struct Dimmed
    {
        Handle(AIS_InteractiveObject) object;
        Standard_Real                 transparency = 0.0;
    };

    Handle(AIS_InteractiveContext) myAisContext;
    Handle(V3d_View)               myView;
    std::vector<Dimmed>            myDimmed;
    bool                           myIsActive = false;

    Quantity_Color myGridColor;
    Quantity_Color myGridTenthColor;
    bool           myHasGridColors = false;
};

} // namespace lcad
