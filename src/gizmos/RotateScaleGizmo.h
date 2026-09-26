#pragma once

// The axis-drag and pixel-distance helpers are shared with the press/pull
// arrow rather than written twice: "how far along this axis did the drag
// go" and "is the cursor on this line" are the same two questions here.
#include "gizmos/PressPullGizmo.h"

#include <Graphic3d_Camera.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>

#include <vector>

namespace lcad {

// The maths behind Fusion's rotate and scale manipulators, kept free of
// AIS, Qt and Document so it can be exercised headlessly -- including the
// DRAG: pixels in, angle or factor out. The screen is a Graphic3d_Camera
// plus a viewport size, which is all V3d_View itself uses to turn a pixel
// into a ray, so a test can stand up the app's own home view with no
// window and push a drag through exactly the code the tool runs.

// ---- the screen ----

// Pixels <-> model for one camera and one viewport, in DEVICE pixels with
// (0,0) at the top-left -- the space OcctViewport hands its interactions.
// The mappings are V3d_View::Convert's and ConvertWithProj's.
class GizmoScreen
{
public:
    GizmoScreen() = default;
    GizmoScreen(const Handle(Graphic3d_Camera)& theCamera, int theWidth, int theHeight);

    bool IsValid() const;

    // Where a model point lands on screen.
    bool Project(const gp_Pnt& thePoint, double& theX, double& theY) const;

    // The line of sight through a pixel.
    bool RayThrough(double theX, double theY, gp_Lin& theRay) const;

    // Model units per device pixel at theAnchor's depth.
    //
    // Asked of Graphic3d_TransformPers itself rather than re-derived, so it
    // is by construction the scale a Graphic3d_TMF_ZoomPers object anchored
    // there is drawn at. The handles are drawn that way -- built in pixels,
    // kept a fixed size on screen at every zoom -- and the hit test and the
    // drag work in model units, so the two must agree to the pixel or the
    // user grabs at a ring that is not where it is drawn.
    double ModelPerPixel(const gp_Pnt& theAnchor) const;

private:
    Handle(Graphic3d_Camera) myCamera;
    int myWidth  = 0;
    int myHeight = 0;
};

// ---- sizes ----

// Fusion's manipulators are a fixed size ON SCREEN, whatever the zoom and
// whatever the size of the body. Sized off the body, the handles zoomed
// with it: comfortable at the zoom they were built at, a speck once the
// wheel pulled back, off the edge of the window once it pushed in, and a
// speck at any zoom for a small part beside a big one. LOGICAL pixels --
// the caller multiplies by the device ratio.
double RotateRingRadiusPixels();
double ScaleHandleLengthPixels();

// How close to a handle a press has to land to grab it, in logical pixels.
double GizmoPickRadiusPixels();

// ---- rotation ----

// The three world axes through a pivot, in X, Y, Z order.
//
// The INDEX is what a drag stores and what a commit reads back to decide
// which of Rotation X / Y / Z it fills in, so 0/1/2 meaning X/Y/Z is part
// of the contract, not an implementation detail.
gp_Ax1 GizmoAxis(int theIndex, const gp_Pnt& thePivot);

// The frame an angle about theAxis is measured in: its X direction is the
// zero of the angle, its Y direction is a quarter turn further on by the
// RIGHT-HAND rule about the axis. So a positive angle here is a positive
// angle in gp_Trsf::SetRotation's terms, which is what lets a drag be
// handed to TransformFeature without a sign to remember.
//
// Derived from the axis alone, so two consecutive mouse positions are
// measured against the same zero -- without that, comparing them would be
// meaningless.
gp_Ax2 RotationFrame(const gp_Ax1& theAxis);

// Where the cursor's ray crosses the plane through theAxis' location
// perpendicular to it, as an angle in RADIANS in RotationFrame's terms.
//
// False when the view is looking along the plane -- the ring is edge-on,
// there is no stable point on it under the cursor, and a drag started
// there would swing wildly for every pixel of mouse travel. Refusing is
// the press/pull arrow's answer to the same situation and for the same
// reason. Also false at the pivot itself, where an angle means nothing.
bool AngleOnPlane(const gp_Ax1& theAxis, const gp_Lin& theRay, double& theAngleRadians);

// theAngle folded into [-pi, pi).
//
// Two consecutive mouse positions are never half a turn apart, so the
// FOLDED DIFFERENCE between them is the turn the user actually made.
// Accumulating those differences is what lets a drag pass 180 degrees, or
// cross the frame's zero, without the running total jumping sign -- which
// is what comparing raw angles would do.
double WrapAngle(double theAngleRadians);

// One ring handle: a thin torus about theAxis, at theRadius.
TopoDS_Shape MakeRotateRingShape(const gp_Ax1& theAxis, double theRadius);

// Points evenly spaced round the ring, for the pixel hit test. The list
// does not repeat the first point; the caller closes the loop.
std::vector<gp_Pnt> SampleRing(const gp_Ax1& theAxis, double theRadius, int theCount);

// Which of the three rings round thePivot is under the pixel: 0/1/2 for
// X/Y/Z, -1 for none. theRadius is the rings' radius in MODEL units at the
// current zoom. Where two rings cross on screen the nearer one wins, so
// the click goes to the ring actually under the cursor rather than to
// whichever happens to be tested first.
int PickRotateRing(const GizmoScreen& theScreen, const gp_Pnt& thePivot, double theRadius,
                   double theX, double theY, double thePickPixels);

// One press-drag-release on a ring.
//
// The angle is read off where the cursor's ray crosses the ring's plane,
// so the point of the ring under the cursor at the press STAYS under the
// cursor as it moves: the body turns the way the hand goes, by as much as
// the hand went, with no gain to tune and no sign to get wrong.
class RingDrag
{
public:
    // False when the ring is edge-on to the view: there is nothing
    // stable to drag, and nothing is started.
    bool Begin(const gp_Ax1& theAxis, const gp_Lin& thePressRay);

    // False when the view has gone edge-on mid-drag. The total is held at
    // its last good value rather than jumping to one nobody asked for.
    bool Update(const gp_Lin& theRay);

    bool IsActive() const { return myIsActive; }
    double TotalRadians() const { return myTotal; }
    double TotalDegrees() const;

private:
    gp_Ax1 myAxis;
    double myLast     = 0.0;  // radians, in the ring's own frame
    double myTotal    = 0.0;  // accumulated from wrapped differences
    bool   myIsActive = false;
};

// ---- uniform scale ----

// Which way the scale handle stands off from the pivot.
//
// The (1,1,1) diagonal, because it is the one direction that is not
// parallel to the view in ANY of this app's standard views -- front, top,
// right and the isometric home view all leave it at 55 degrees or more to
// the line of sight, so the handle is always draggable without orbiting
// first. A world axis would point straight at the camera in three of them.
gp_Dir ScaleHandleDirection();

// The handle: a stalk from the pivot out to theOffset with a cube on the
// end, the shape everyone reads as "drag me to resize".
TopoDS_Shape MakeScaleHandleShape(const gp_Ax1& theAxis, double theOffset, double theSize);

// Whether the pixel is on the scale handle standing theLength (MODEL
// units) off theAxis' location. Only the outer half counts: the inner
// half runs through the body, where a click means "select this face" far
// more often than it means "resize".
bool PickScaleHandle(const GizmoScreen& theScreen, const gp_Ax1& theAxis, double theLength,
                     double theX, double theY, double thePickPixels);

// The factor a drag of theDragDistance along the handle axis asks for:
// the handle's new distance from the pivot over its old one.
//
// FALSE when that would be zero or negative -- the cursor has been
// dragged onto the pivot or past it. A factor of zero collapses the body
// to nothing and a negative one turns it inside out, and the kernel will
// build both without complaining, so they are refused here and again in
// TransformFeature rather than previewed and committed.
bool ScaleFactorFromDrag(double theRestOffset, double theDragDistance, double& theFactor);

// One press-drag-release on the scale handle. The factor is the handle
// tip's distance from the pivot over its distance at rest, so the tip
// follows the cursor: drag it out to twice as far and the body doubles.
class ScaleDrag
{
public:
    // theRestLength is where the tip sits at rest, in MODEL units at the
    // zoom the press happened at. False when the handle points along the
    // line of sight.
    bool Begin(const gp_Ax1& theAxis, double theRestLength, const gp_Lin& thePressRay);

    // False when refused -- parallel to the view, or dragged through the
    // pivot. The factor is held at its last good value.
    bool Update(const gp_Lin& theRay);

    bool IsActive() const { return myIsActive; }
    double Factor() const { return myFactor; }

private:
    gp_Ax1 myAxis;
    gp_Pnt myStart;             // where on the axis the press landed
    double myRestLength = 0.0;
    double myFactor     = 1.0;
    bool   myIsActive   = false;
};

// Below this the drag was a click: a stray pixel of travel must not land
// a 0.0001-degree rotation or a 1.000001x scale on the timeline.
double MinimumDragAngleDegrees();
double MinimumScaleChange();

} // namespace lcad
