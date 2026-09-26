#pragma once

#include <cmath>

// The geometry of a marking menu, kept free of Qt so it can be tested
// without a window: which wedge the pointer is in, and where each wedge's
// label sits.
//
// Eight wedges, counted CLOCKWISE FROM STRAIGHT UP -- 0 is north, 2 east,
// 4 south, 6 west -- in screen coordinates, where y grows downward. Each
// wedge is the 45 degrees centred on its direction, so north runs from
// -22.5 to +22.5 degrees and a pointer exactly between two wedges belongs
// to the clockwise one.
namespace lcad {

constexpr int kMarkingMenuWedges = 8;

// Which wedge (theDx, theDy) from the centre points into, or -1 inside the
// dead zone. The dead zone is what lets a click on the centre mean "never
// mind" instead of picking whichever wedge a one-pixel wobble pointed at.
inline int MarkingMenuWedgeAt(double theDx, double theDy, double theDeadZone)
{
    if (std::hypot(theDx, theDy) < theDeadZone) {
        return -1;
    }
    // atan2(dx, -dy) is zero pointing up and grows clockwise on screen.
    double degrees = std::atan2(theDx, -theDy) * 180.0 / 3.14159265358979323846;
    if (degrees < 0.0) {
        degrees += 360.0;
    }
    return static_cast<int>(std::floor((degrees + 22.5) / 45.0)) % kMarkingMenuWedges;
}

// Where wedge theIndex's label is anchored: theRadius out along its
// direction, as an offset from the centre in screen coordinates.
inline void MarkingMenuWedgeAnchor(int theIndex, double theRadius, double& theX, double& theY)
{
    const double radians = theIndex * 45.0 * 3.14159265358979323846 / 180.0;
    theX = std::sin(radians) * theRadius;
    theY = -std::cos(radians) * theRadius;
}

} // namespace lcad
