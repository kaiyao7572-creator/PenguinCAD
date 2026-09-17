#pragma once

#include "core/ProfileProvider.h"

#include <gp_Pnt2d.hxx>

#include <vector>

namespace lcad {

class SketchFeature;

// Splitting a sketch into the regions a user can point at.
//
// This is deliberately free of UI, AIS and Document, like the rest of the
// sketch model layer, so the region maths can be tested without a display.
//
// "Minimal" is the whole point, and it is what makes a profile a Fusion
// profile rather than just a closed loop: a rectangle with a circle drawn
// inside it is TWO regions -- the ring and the disc -- not one face with a
// hole punched in it. Two overlapping circles are three regions. A line
// drawn across a rectangle splits it into two. Anything coarser and the
// user cannot say which part of their sketch they meant.
std::vector<ProfileRegion> ComputeProfileRegions(const SketchFeature& theSketch);

} // namespace lcad
