#include "core/ConstructionGeometry.h"

#include "core/Feature.h"
#include "core/Origin.h"

namespace lcad {

ConstructionGeometry* AsConstructionGeometry(Feature* theFeature)
{
    return dynamic_cast<ConstructionGeometry*>(theFeature);
}

bool ResolvePlaneByName(const ComputeContext& theContext,
                        const std::string&    theName,
                        gp_Ax3&               theResult)
{
    if (theName.empty()) {
        return false;
    }
    if (const OriginEntity* origin = FindOriginEntity(theName)) {
        if (origin->type != EntityType::ConstructionPlane) {
            return false;
        }
        theResult = origin->plane;
        return true;
    }
    ConstructionGeometry* geometry = AsConstructionGeometry(theContext.FindFeature(theName));
    return geometry != nullptr && geometry->AsPlane(theResult);
}

bool ResolveAxisByName(const ComputeContext& theContext,
                       const std::string&    theName,
                       gp_Ax1&               theResult)
{
    if (theName.empty()) {
        return false;
    }
    if (const OriginEntity* origin = FindOriginEntity(theName)) {
        if (origin->type != EntityType::ConstructionAxis) {
            return false;
        }
        theResult = origin->axis;
        return true;
    }
    ConstructionGeometry* geometry = AsConstructionGeometry(theContext.FindFeature(theName));
    return geometry != nullptr && geometry->AsAxis(theResult);
}

bool ResolvePointByName(const ComputeContext& theContext,
                        const std::string&    theName,
                        gp_Pnt&               theResult)
{
    if (theName.empty()) {
        return false;
    }
    if (const OriginEntity* origin = FindOriginEntity(theName)) {
        // A plane's or axis's own origin is a legitimate point to mean.
        theResult = origin->point;
        return true;
    }
    ConstructionGeometry* geometry = AsConstructionGeometry(theContext.FindFeature(theName));
    return geometry != nullptr && geometry->AsPoint(theResult);
}

} // namespace lcad
