#pragma once

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>

#include <string>

namespace lcad {

// The maths behind Fusion's Press/Pull drag, kept free of AIS, Qt and
// Document so it can be exercised headlessly. The one step that genuinely
// needs a V3d_View -- turning a pixel into a ray, and a 3D point back into
// a pixel -- lives in PressPullGizmoTool and is deliberately not here.

// Where the arrow sits for one face, which way it points, and how long it
// is drawn.
struct PressPullArrow
{
    gp_Ax1 axis;            // at the face's centre of mass, along its OUTWARD normal
    double length = 0.0;    // tail to tip, in model units
};

// Place the arrow on a face.
//
// The outward normal has to be derived exactly the way PressPullFeature
// derives it -- surface normal, reversed when the face's orientation in
// the solid is REVERSED -- or the arrow would point one way while the
// feature it commits pushes the other. A test asserts the two agree by
// volume rather than by inspection, because that is the only way to
// catch them drifting apart.
//
// Planar faces only. PressPullFeature can move a cylindrical face too,
// by changing its radius, but "which way is out" for a cylinder is a
// radial direction that depends where on the wall you grabbed it -- so
// the arrow refuses rather than guessing, and the typed-distance command
// still handles that case.
bool ComputePressPullArrow(const TopoDS_Face& theFace,
                           PressPullArrow&    theArrow,
                           std::string&       theError);

// How long an arrow is drawn on a face of a given representative size
// (sqrt of its area -- the only length scale a face offers on its own).
double PressPullArrowLength(double theFaceSize);

// The arrow solid: a shaft and a conical head along theAxis, starting at
// its location.
TopoDS_Shape MakePressPullArrowShape(const gp_Ax1& theAxis, double theLength);

// How far the drag has gone: the component of (current - start) ALONG the
// axis. Motion ACROSS the axis contributes nothing, which is the whole
// point -- the raw distance between the two points would make a sideways
// mouse sweep push the face out.
double DragDistanceAlongAxis(const gp_Ax1& theAxis,
                             const gp_Pnt& theStart,
                             const gp_Pnt& theCurrent);

// The point on theAxis' infinite line closest to theRay: where the cursor
// "is" on the drag axis. False when the ray is parallel to the axis --
// there is no unique answer then, and dragging along the line of sight
// means nothing anyway.
bool ClosestPointOnAxis(const gp_Ax1& theAxis,
                        const gp_Lin& theRay,
                        gp_Pnt&       theResult);

// Pixel distance from a point to a segment. The arrow's hit test, once
// its two ends have been projected to the screen: a fixed pixel radius is
// what the user actually perceives as "on the arrow", where a fixed model
// radius would grow and shrink with the zoom.
double DistanceToSegment2d(double thePointX, double thePointY,
                           double theAx, double theAy,
                           double theBx, double theBy);

} // namespace lcad
