// The marking menu's geometry, without a window: which wedge a pointer
// offset lands in, and where each label is anchored. A wedge off by one is
// a menu that runs Undo when the hand reached for Redo.
#include "ui/MarkingMenuGeometry.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace lcad;

static int failures = 0;

static void check(bool theOk, const std::string& theWhat)
{
    std::cout << (theOk ? "  PASS  " : "  FAIL  ") << theWhat << std::endl;
    if (!theOk) {
        ++failures;
    }
}

int main()
{
    const double dead = 22.0;

    std::cout << "-- the eight directions, clockwise from up, y growing DOWN --" << std::endl;
    check(MarkingMenuWedgeAt(0, -100, dead) == 0, "straight up is 0 (Repeat)");
    check(MarkingMenuWedgeAt(70, -70, dead) == 1, "up-right is 1 (Press Pull)");
    check(MarkingMenuWedgeAt(100, 0, dead) == 2, "right is 2 (Redo / OK)");
    check(MarkingMenuWedgeAt(70, 70, dead) == 3, "down-right is 3 (Hole)");
    check(MarkingMenuWedgeAt(0, 100, dead) == 4, "straight down is 4 (Sketch)");
    check(MarkingMenuWedgeAt(-70, 70, dead) == 5, "down-left is 5 (Move/Copy)");
    check(MarkingMenuWedgeAt(-100, 0, dead) == 6, "left is 6 (Undo / Cancel)");
    check(MarkingMenuWedgeAt(-70, -70, dead) == 7, "up-left is 7 (Delete)");

    std::cout << "-- edges and the dead zone --" << std::endl;
    const double r = 100.0;
    const double justLeftOfEdge = (22.5 - 0.5) * 3.14159265358979323846 / 180.0;
    const double justRightOfEdge = (22.5 + 0.5) * 3.14159265358979323846 / 180.0;
    check(MarkingMenuWedgeAt(std::sin(justLeftOfEdge) * r, -std::cos(justLeftOfEdge) * r, dead) == 0,
          "22 degrees right of up is still north");
    check(MarkingMenuWedgeAt(std::sin(justRightOfEdge) * r, -std::cos(justRightOfEdge) * r, dead) == 1,
          "23 degrees is north-east");
    check(MarkingMenuWedgeAt(-5, -99, dead) == 0, "a little left of up is north, not north-west");
    check(MarkingMenuWedgeAt(0, 0, dead) == -1, "the centre picks nothing");
    check(MarkingMenuWedgeAt(15, -15, dead) == -1, "nor does a wobble inside the dead zone");
    check(MarkingMenuWedgeAt(0, -23, dead) == 0, "just outside it counts");

    std::cout << "-- labels sit out along their own wedge --" << std::endl;
    for (int i = 0; i < kMarkingMenuWedges; ++i) {
        double x = 0.0;
        double y = 0.0;
        MarkingMenuWedgeAnchor(i, 105.0, x, y);
        check(MarkingMenuWedgeAt(x, y, dead) == i && std::fabs(std::hypot(x, y) - 105.0) < 1e-9,
              "wedge " + std::to_string(i) + "'s label anchor points back into wedge " +
                  std::to_string(i));
    }

    std::cout << (failures == 0 ? "\nALL MARKING MENU TESTS PASSED\n" : "\nFAILURES\n");
    return failures == 0 ? 0 : 1;
}
