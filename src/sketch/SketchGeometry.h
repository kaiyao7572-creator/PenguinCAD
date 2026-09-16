#pragma once

#include "sketch/SketchEntity.h"

#include <Geom2d_Curve.hxx>
#include <Geom_Curve.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec2d.hxx>

#include <vector>

namespace lcad {

// Pure 2D geometry for the sketch subsystem: no UI, no document, no AIS.
//
// Every entity is described here by one unbounded-where-possible curve
// plus a parameter window on it. That single representation is what lets
// trim, extend, fillet, offset and the wire assembler treat a line, an
// arc, an ellipse and a spline the same way instead of each carrying six
// special cases.
namespace SketchGeometry {

// Micron tolerance: far tighter than anything a user can click, loose
// enough to absorb the float noise two different tools produce for one
// corner.
constexpr double kTolerance = 1.0e-6;

// The entity's underlying curve, untrimmed where the kind allows it (a
// line comes back infinite, a circle whole). Null for Kind::Point and for
// degenerate entities.
Handle(Geom2d_Curve) Curve2dOf(const SketchEntity& theEntity);

// Parameter window the entity actually occupies on that curve.
bool ParamRange(const SketchEntity& theEntity, double& theFirst, double& theLast);

// Lift a 2D curve onto a sketch plane. Poles and axes are mapped through
// the plane frame, so the 3D curve keeps the 2D parameterisation exactly
// -- which is what makes a parameter measured in 2D usable to trim the
// 3D edge.
Handle(Geom_Curve) To3dCurve(const Handle(Geom2d_Curve)& theCurve, const gp_Ax3& thePosition);

gp_Pnt2d PointAt(const SketchEntity& theEntity, double theParam);
gp_Vec2d TangentAt(const SketchEntity& theEntity, double theParam);

// Parameter of the point on the entity nearest thePoint. theClamp keeps
// the result inside the entity's own window; false lets it run out onto
// the unbounded part of the curve, which is what extend needs.
bool NearestParam(const SketchEntity& theEntity,
                  const gp_Pnt2d&     thePoint,
                  double&             theParam,
                  bool                theClamp = true);

// Distance from thePoint to the entity, and where on it the closest point
// lies. Used for picking and for trim's "which piece is under the cursor".
double DistanceTo(const SketchEntity& theEntity, const gp_Pnt2d& thePoint, double& theParam);

// Intersections between two entities, as parameter pairs on each. Only
// intersections lying inside both entities' windows are reported unless
// theWholeCurves is set, which is what fillet and extend need.
struct Intersection
{
    double   paramA = 0.0;
    double   paramB = 0.0;
    gp_Pnt2d point;
};

std::vector<Intersection> Intersect(const SketchEntity& theFirst,
                                    const SketchEntity& theSecond,
                                    bool                theWholeCurves = false);

// The stretch of theEntity lying between the crossings either side of
// theParam -- exactly the piece Trim deletes. On a closed curve with
// fewer than two crossings the whole thing comes back, because there are
// no ends to stop at.
bool PieceBetweenCrossings(const SketchEntity&              theEntity,
                           const std::vector<SketchEntity>& theOthers,
                           double                           theParam,
                           double&                          theFrom,
                           double&                          theTo);

// Narrow the entity to [theFirst, theLast] of its own curve parameter.
// A circle or a full ellipse becomes an arc; a spline records the window
// as a fraction of its range. Returns false if the result is degenerate.
bool SetParamWindow(SketchEntity& theEntity, double theFirst, double theLast);

// Cut [theFrom, theTo] out of the entity, returning what is left: one
// piece when the cut reaches an end, two when it takes a bite out of the
// middle, none when it consumes the whole thing. This is what Trim does.
std::vector<SketchEntity> RemoveRange(const SketchEntity& theEntity,
                                      double              theFrom,
                                      double              theTo);

// ---- transforms ----

SketchEntity Translated(const SketchEntity& theEntity, const gp_Vec2d& theOffset);
SketchEntity Rotated(const SketchEntity& theEntity, const gp_Pnt2d& theCentre, double theAngle);

// Reflect across the line through theFirst and theSecond. Arcs and
// ellipses come back with their sweep reversed, as a mirror must.
SketchEntity Mirrored(const SketchEntity& theEntity,
                      const gp_Pnt2d&     theFirst,
                      const gp_Pnt2d&     theSecond);

// Offset to the left of the curve direction for a positive distance.
// Lines and conics offset exactly; splines are re-fitted through offset
// sample points, which is the only way to keep them a spline.
bool Offset(const SketchEntity& theEntity, double theDistance, SketchEntity& theResult);

// ---- constructions the Create tools need ----

std::vector<SketchEntity> RectangleTwoPoint(const gp_Pnt2d& theCorner,
                                            const gp_Pnt2d& theOpposite);
std::vector<SketchEntity> RectangleCentre(const gp_Pnt2d& theCentre, const gp_Pnt2d& theCorner);

// theFirst -> theSecond is one full edge; theThird sets the width on the
// side it falls, so the rectangle can sit at any angle.
std::vector<SketchEntity> RectangleThreePoint(const gp_Pnt2d& theFirst,
                                              const gp_Pnt2d& theSecond,
                                              const gp_Pnt2d& theThird);

// theReference is a vertex for an inscribed polygon and an edge midpoint
// for a circumscribed one -- the two ways Fusion asks for the size.
std::vector<SketchEntity> PolygonInscribed(const gp_Pnt2d& theCentre,
                                           const gp_Pnt2d& theReference,
                                           int             theSides);
std::vector<SketchEntity> PolygonCircumscribed(const gp_Pnt2d& theCentre,
                                               const gp_Pnt2d& theReference,
                                               int             theSides);
// One edge given; the polygon is built to its left.
std::vector<SketchEntity> PolygonOnEdge(const gp_Pnt2d& theFirst,
                                        const gp_Pnt2d& theSecond,
                                        int             theSides);

// Two straight flanks capped by semicircles -- Fusion's centre-to-centre
// slot. theWidth is the full width, so the cap radius is half of it.
std::vector<SketchEntity> Slot(const gp_Pnt2d& theFirstCentre,
                               const gp_Pnt2d& theSecondCentre,
                               double          theWidth);

// Circle through the two ends of a diameter.
bool CircleTwoPoint(const gp_Pnt2d& theFirst, const gp_Pnt2d& theSecond, SketchEntity& theResult);

// Circumcircle; fails on (near-)collinear points.
bool CircleThreePoint(const gp_Pnt2d& theFirst,
                      const gp_Pnt2d& theSecond,
                      const gp_Pnt2d& theThird,
                      SketchEntity&   theResult);

// Arc from theFirst to theThird passing through theSecond.
bool ArcThreePoint(const gp_Pnt2d& theFirst,
                   const gp_Pnt2d& theSecond,
                   const gp_Pnt2d& theThird,
                   SketchEntity&   theResult);

// Arc leaving theStart along theDirection and ending at theEnd -- the
// unique tangent arc, which is what makes Fusion's tangent-arc tool feel
// like it continues the curve you came from.
bool ArcTangent(const gp_Pnt2d& theStart,
                const gp_Vec2d& theDirection,
                const gp_Pnt2d& theEnd,
                SketchEntity&   theResult);

// ---- fillet ----

// Round the corner where two lines meet. theNear* say which side of each
// line to keep, so clicking near the two ends the user wants preserved
// picks the corner they mean out of the four an intersection creates.
struct FilletResult
{
    SketchEntity arc;
    SketchEntity firstTrimmed;
    SketchEntity secondTrimmed;
};

bool FilletLines(const SketchEntity& theFirst,
                 const SketchEntity& theSecond,
                 const gp_Pnt2d&     theNearFirst,
                 const gp_Pnt2d&     theNearSecond,
                 double              theRadius,
                 FilletResult&       theResult);

// Largest radius those two kept halves can actually be rounded with. A
// reference point that lands on the corner itself means "whichever half
// there is", which is the case for two lines already sharing an end.
double MaximumFilletRadius(const SketchEntity& theFirst,
                           const SketchEntity& theSecond,
                           const gp_Pnt2d&     theNearFirst,
                           const gp_Pnt2d&     theNearSecond);

} // namespace SketchGeometry

} // namespace lcad
