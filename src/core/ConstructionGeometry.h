#pragma once

#include "core/Entity.h"

#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>

#include <string>

namespace lcad {

class Feature;
struct ComputeContext;

// The seam for construction geometry, mirroring ProfileProvider.
//
// A construction plane is built in src/features but consumed by the
// sketch subsystem, the solid features and the plane picker, none of
// which should know its concrete type. They hold its NAME and resolve it
// through here, the same bargain everything else in this document makes
// with undo: a pointer would go stale the moment the timeline is cloned.
class ConstructionGeometry
{
public:
    virtual ~ConstructionGeometry() = default;

    // ConstructionPlane, ConstructionAxis or ConstructionPoint.
    virtual EntityType ConstructionType() const = 0;

    // Each returns false unless it is that kind of thing. A plane also
    // answers AsPoint with its origin, and an axis with its root, because
    // "the point on that plane" is a thing a user can mean.
    virtual bool AsPlane(gp_Ax3& thePlane) const { (void)thePlane; return false; }
    virtual bool AsAxis(gp_Ax1& theAxis) const { (void)theAxis; return false; }
    virtual bool AsPoint(gp_Pnt& thePoint) const { (void)thePoint; return false; }
};

ConstructionGeometry* AsConstructionGeometry(Feature* theFeature);

// Resolve a plane, axis or point by name against the origin folder first
// and then the timeline upstream of the caller.
//
// Origin entities win on purpose: "XY" always means the world XY plane,
// and a construction plane a user happened to name "XY" must not quietly
// redefine what every earlier sketch was drawn on.
bool ResolvePlaneByName(const ComputeContext& theContext,
                        const std::string&    theName,
                        gp_Ax3&               theResult);

bool ResolveAxisByName(const ComputeContext& theContext,
                       const std::string&    theName,
                       gp_Ax1&               theResult);

bool ResolvePointByName(const ComputeContext& theContext,
                        const std::string&    theName,
                        gp_Pnt&               theResult);

} // namespace lcad
