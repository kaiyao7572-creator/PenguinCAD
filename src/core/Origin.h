#pragma once

#include "core/Entity.h"

#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>

#include <string>
#include <vector>

namespace lcad {

// The seven things in every component's Origin folder.
//
// These are not timeline features and never appear in the timeline: in
// Fusion they are intrinsic to a component, exist before anything is
// drawn, and cannot be deleted -- only hidden. That is why they live here
// as a fixed table rather than as Features, and why the sketch plane
// picker and the browser can both rely on them always being present.
struct OriginEntity
{
    EntityType  type = EntityType::ConstructionPlane;
    std::string name;   // "XY", "X", "Origin" -- exactly as Fusion labels them

    // Meaningful for ConstructionPlane. Its X and Y directions are the
    // ones a sketch on that plane inherits, so they are not arbitrary.
    gp_Ax3 plane;

    // Meaningful for ConstructionAxis.
    gp_Ax1 axis;

    // Meaningful for ConstructionPoint, and the location of the others.
    gp_Pnt point;
};

// In the order Fusion's browser draws them: the three planes, then the
// three axes, then the origin point.
const std::vector<OriginEntity>& OriginEntities();

const OriginEntity* FindOriginEntity(const std::string& theName);

} // namespace lcad
