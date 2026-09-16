#pragma once

class QWidget;
class QString;

namespace lcad {

// The few numbers a sketch tool can't get from the cursor. Everything
// else -- sizes, positions, angles -- is picked in the viewport, so these
// stay deliberately small.
namespace SketchDialogs {

// Side count for the polygon tools.
bool AskPolygonSides(QWidget* theParent, int& theSides);

// Copies across and down for a rectangular pattern; the spacing is then
// dragged in the viewport.
bool AskRectangularPattern(QWidget* theParent, int& theAcross, int& theDown);

// Copy count and the angle they span, in degrees.
bool AskCircularPattern(QWidget* theParent, int& theCount, double& theAngleDegrees);

// A dimension's value, pre-filled with what the geometry currently
// measures so that accepting the dialog changes nothing.
bool AskDimensionValue(QWidget*       theParent,
                       const QString& theTitle,
                       bool           theIsAngle,
                       double&        theValue);

} // namespace SketchDialogs

} // namespace lcad
