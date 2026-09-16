#pragma once

#include "sketch/SketchFeature.h"

#include <gp_Pnt2d.hxx>

#include <string>
#include <vector>

namespace lcad {

// Turns a sketch's constraints and dimensions into drawable geometry.
//
// Everything comes back as ordinary SketchEntity values in sketch
// coordinates, which means the sketch's own 2D-to-3D machinery draws the
// annotations exactly as it draws the curves -- no second projection path
// to keep in step with the plane.
namespace SketchAnnotations {

// A dimension's number, and where it goes.
struct Label
{
    gp_Pnt2d    position;
    std::string text;
};

// Little marks showing which relations are in force -- the horizontals,
// coincidences and tangents Fusion scatters over a sketch.
//
// theScale is the size one glyph should be, in sketch units; the display
// passes the model size of a dozen or so screen pixels so the glyphs stay
// readable at any zoom.
std::vector<SketchEntity> ConstraintGlyphs(const SketchFeature& theSketch, double theScale);

// Witness lines, dimension lines and arrowheads for every driving
// dimension.
std::vector<SketchEntity> DimensionGeometry(const SketchFeature& theSketch, double theScale);

// The numbers that go with that geometry.
std::vector<Label> DimensionLabels(const SketchFeature& theSketch);

// Formatted value of one dimension, e.g. "25.40" or "45.0 deg".
std::string FormatDimension(const SketchConstraint& theConstraint);

} // namespace SketchAnnotations

} // namespace lcad
