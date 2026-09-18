#include "core/Origin.h"

#include <gp_Dir.hxx>

namespace lcad {

namespace {

OriginEntity MakePlane(std::string theName, const gp_Dir& theNormal, const gp_Dir& theX)
{
    OriginEntity entity;
    entity.type = EntityType::ConstructionPlane;
    entity.name = std::move(theName);
    entity.point = gp_Pnt(0.0, 0.0, 0.0);
    entity.plane = gp_Ax3(entity.point, theNormal, theX);
    return entity;
}

OriginEntity MakeAxis(std::string theName, const gp_Dir& theDirection)
{
    OriginEntity entity;
    entity.type = EntityType::ConstructionAxis;
    entity.name = std::move(theName);
    entity.point = gp_Pnt(0.0, 0.0, 0.0);
    entity.axis = gp_Ax1(entity.point, theDirection);
    return entity;
}

} // namespace

const std::vector<OriginEntity>& OriginEntities()
{
    static const std::vector<OriginEntity> theEntities = [] {
        std::vector<OriginEntity> entities;

        // The X direction of each plane is the one a sketch drawn on it
        // inherits as its own X, so these match the axes a user expects to
        // be horizontal when they look straight at the plane.
        entities.push_back(MakePlane("XY", gp_Dir(0.0, 0.0, 1.0), gp_Dir(1.0, 0.0, 0.0)));
        entities.push_back(MakePlane("XZ", gp_Dir(0.0, -1.0, 0.0), gp_Dir(1.0, 0.0, 0.0)));
        entities.push_back(MakePlane("YZ", gp_Dir(1.0, 0.0, 0.0), gp_Dir(0.0, 1.0, 0.0)));

        entities.push_back(MakeAxis("X", gp_Dir(1.0, 0.0, 0.0)));
        entities.push_back(MakeAxis("Y", gp_Dir(0.0, 1.0, 0.0)));
        entities.push_back(MakeAxis("Z", gp_Dir(0.0, 0.0, 1.0)));

        OriginEntity origin;
        origin.type = EntityType::ConstructionPoint;
        origin.name = "Origin";
        origin.point = gp_Pnt(0.0, 0.0, 0.0);
        entities.push_back(origin);

        return entities;
    }();
    return theEntities;
}

const OriginEntity* FindOriginEntity(const std::string& theName)
{
    for (const OriginEntity& entity : OriginEntities()) {
        if (entity.name == theName) {
            return &entity;
        }
    }
    return nullptr;
}

} // namespace lcad
