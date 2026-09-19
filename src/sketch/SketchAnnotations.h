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

    // Which way the text runs, radians CCW from the sketch's X axis. A
    // linear dimension's number lies along its own dimension line, as a
    // drawing has it -- and because the number is centred on that line,
    // running along it is the only way a slanted dimension's text does
    // not sit across it. Never upside down: a direction pointing left is
    // turned round.
    double rotation = 0.0;
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

// The numbers that go with that geometry. theScale is the same glyph
// size DimensionGeometry took, and is what lifts each number clear of the
// dimension line drawn through the spot the user dropped it on.
std::vector<Label> DimensionLabels(const SketchFeature& theSketch, double theScale);

// Formatted value of one dimension in the document's unit, e.g.
// "d1 = 25.4 mm", "d2 = 45 deg", "d3 = D12 mm".
std::string FormatDimension(const SketchConstraint& theConstraint);

} // namespace SketchAnnotations

} // namespace lcad
