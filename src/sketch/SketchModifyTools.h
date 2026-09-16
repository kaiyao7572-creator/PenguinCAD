#pragma once

#include "sketch/SketchTools.h"

namespace lcad {

// Pattern tools need their counts before they can preview anything, so
// the command asks for them in a dialog and hands them over here.
class SketchRectangularPatternTool : public SketchTool
{
public:
    void SetCounts(int theAcross, int theDown);

protected:
    int myAcross = 3;
    int myDown   = 1;
};

class SketchCircularPatternTool : public SketchTool
{
public:
    void SetPattern(int theCount, double theTotalAngle);

protected:
    int    myCount = 6;
    double myTotalAngle = 6.283185307179586;  // a full turn
};

// ---- Modify ----

// Rounds the corner between two lines: pick one line, pick the other,
// then drag to size the arc.
SketchTool& SketchFilletTool();

// Deletes the piece of a curve between its neighbours' intersections --
// hover to see what would go, click to remove it.
SketchTool& SketchTrimTool();

// Runs a curve on to the first thing it meets.
SketchTool& SketchExtendTool();

// Offsets a connected chain; the cursor picks the side and the distance.
SketchTool& SketchOffsetTool();

// Mirrors the current selection about a line picked in the viewport.
SketchTool& SketchMirrorTool();

SketchRectangularPatternTool& SketchRectangularPattern();
SketchCircularPatternTool&    SketchCircularPattern();

// ---- Inspect ----

// Fusion's one dimension command: what it measures depends on what is
// picked -- a line's length, a circle's diameter, the gap between two
// points, the angle between two lines.
SketchTool& SketchDimensionTool();

} // namespace lcad
