#pragma once

// The axis-drag and pixel-distance helpers are shared with the press/pull
// arrow rather than written twice: "how far along this axis did the drag
// go" and "is the cursor on this line" are the same two questions here.
#include "gizmos/PressPullGizmo.h"

#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>

#include <vector>

namespace lcad {

// The maths behind Fusion's rotate and scale manipulators, kept free of
// AIS, Qt and Document so it can be exercised headlessly. The one step
// that genuinely needs a V3d_View -- turning a pixel into a ray and a 3D
// point back into a pixel -- lives in RotateScaleGizmoTool and is
// deliberately not here. Same split PressPullGizmo.h makes, same reason.

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

// Ring radius for a body whose bounding box measures theBodySize corner
// to corner. Larger than the box's own half-diagonal, so the rings
// ENCIRCLE the body rather than hiding inside it where they could not be
// grabbed.
double RotateRingRadius(double theBodySize);

// One ring handle: a thin torus about theAxis, at theRadius.
TopoDS_Shape MakeRotateRingShape(const gp_Ax1& theAxis, double theRadius);

// Points evenly spaced round the ring, for the pixel hit test. The list
// does not repeat the first point; the caller closes the loop.
std::vector<gp_Pnt> SampleRing(const gp_Ax1& theAxis, double theRadius, int theCount);

// ---- uniform scale ----

// Which way the scale handle stands off from the pivot.
//
// The (1,1,1) diagonal, because it is the one direction that is not
// parallel to the view in ANY of this app's standard views -- front, top,
// right and the isometric home view all leave it at 55 degrees or more to
// the line of sight, so the handle is always draggable without orbiting
// first. A world axis would point straight at the camera in three of them.
gp_Dir ScaleHandleDirection();

// How far the handle sits from the pivot, for a body of theBodySize.
double ScaleHandleOffset(double theBodySize);

// The handle: a stalk from the pivot out to theOffset with a cube on the
// end, the shape everyone reads as "drag me to resize".
TopoDS_Shape MakeScaleHandleShape(const gp_Ax1& theAxis, double theOffset, double theSize);

// The factor a drag of theDragDistance along the handle axis asks for:
// the handle's new distance from the pivot over its old one.
//
// FALSE when that would be zero or negative -- the cursor has been
// dragged onto the pivot or past it. A factor of zero collapses the body
// to nothing and a negative one turns it inside out, and the kernel will
// build both without complaining, so they are refused here and again in
// TransformFeature rather than previewed and committed.
bool ScaleFactorFromDrag(double theRestOffset, double theDragDistance, double& theFactor);

// Below this the drag was a click: a stray pixel of travel must not land
// a 0.0001-degree rotation or a 1.000001x scale on the timeline.
double MinimumDragAngleDegrees();
double MinimumScaleChange();

} // namespace lcad
