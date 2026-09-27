#pragma once

#include "core/ProfileProvider.h"

#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt2d.hxx>

#include <memory>
#include <string>
#include <vector>

class IntCurvesFace_ShapeIntersector;

namespace lcad {

// Which region of a FINISHED sketch lies under the cursor, and whether it
// is really the thing the cursor is on.
//
// Free of Qt, AIS and the document on purpose, like the rest of the sketch
// model layer: the rule it encodes -- the sketch plane against the bodies
// along one ray -- is the whole of what makes "sketch on a face, click the
// region, press E" feel like Fusion, and it has to be testable without a
// window. The viewport side (ModelProfilePicker) only builds the ray and
// asks.

// One finished sketch the cursor might be over.
struct ProfilePickTarget
{
    // The plane the sketch's entities live on, offset included -- what
    // SketchFeature::Position() returns. Its X and Y directions are the
    // sketch's own 2D axes, which is what the regions are classified in.
    gp_Ax3 position;

    // Owned by the caller and only read here.
    const std::vector<ProfileRegion>* regions = nullptr;
};

struct ProfileHit
{
    int      target = -1;   // index into the targets handed in
    int      region = -1;   // index into that target's regions
    double   depth  = 0.0;  // how far along the ray the plane was met
    gp_Pnt2d point;         // where, in the sketch's own coordinates

    bool IsHit() const { return target >= 0 && region >= 0; }
};

// Where a ray meets a sketch plane: the distance along the ray and the
// point in the plane's own 2D coordinates. False when the ray runs along
// the plane, or meets it behind where the ray starts -- the view's ray
// starts on the near clipping plane, so nothing behind it is on screen.
bool RayMeetsPlane(const gp_Lin& theRay,
                   const gp_Ax3& thePlane,
                   double&       theDepth,
                   gp_Pnt2d&     thePoint);

// The region under the ray, or no hit.
//
// theBodyDepth is how far along the same ray the nearest body (or anything
// else a click could select) was met -- infinity when nothing was. The
// rule, which is Fusion's:
//
//   - A body in FRONT of the sketch plane owns the cursor. The region is
//     hidden behind it, and the click is ordinary face/edge selection.
//   - A body BEHIND the plane, or none at all, and the region wins over
//     both the body and empty space.
//   - A body ON the plane -- the sketch was drawn on that face -- and the
//     region wins inside the region. This is the case that makes sketching
//     on a face and extruding the region work; with a strict "nearest
//     wins" the face would take every click, because float noise decides
//     which of two coplanar things is a hair nearer.
//
// theTolerance is how close in depth counts as "on the plane", in model
// units. The caller sizes it to the screen: a face less than a pixel off
// the sketch plane is, for anything a user can see or mean, on it.
//
// Several sketches under the cursor: the nearest plane wins, and between
// coplanar ones the smaller region, for the same reason ProfileRegionAt
// prefers it -- the smaller is always the more specific thing to mean.
ProfileHit ProfileUnderRay(const gp_Lin&                         theRay,
                           const std::vector<ProfilePickTarget>& theTargets,
                           double                                theBodyDepth,
                           double                                theTolerance);

// How far along a ray the nearest body surface is.
//
// This is the occlusion half of the rule, done against the real geometry
// rather than against what OCCT's selection happens to detect: the
// selection only knows about what the filter lets a click pick, so with
// Edge Priority on, a face in front of a sketch would detect nothing and
// the region behind it would light up through the solid.
//
// Loading the shapes builds a classifier per face, which is far too slow
// to do per mouse move and cheap to keep: SetBodies only reloads when the
// bodies it is handed are not the ones it already has.
class BodyRayCaster
{
public:
    BodyRayCaster();
    ~BodyRayCaster();

    BodyRayCaster(const BodyRayCaster&) = delete;
    BodyRayCaster& operator=(const BodyRayCaster&) = delete;

    void SetBodies(const std::vector<TopoDS_Shape>& theShapes);

    // Distance along the ray to the first surface ahead of where it
    // starts; infinity when it hits nothing.
    double NearestDepth(const gp_Lin& theRay) const;

private:
    std::vector<TopoDS_Shape>                      myShapes;
    std::unique_ptr<IntCurvesFace_ShapeIntersector> myIntersector;
};

// The status-bar line for picked regions, worded the way a picked face is
// ("Face   Area 400 mm²"), so a region reads as the same kind of answer.
// Empty for no regions.
std::string DescribeProfiles(const std::vector<double>& theAreas);

} // namespace lcad
