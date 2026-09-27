// Telling a right click from a flick from a hold, without a window. Get a
// threshold wrong and a click that wobbled runs Undo, or a flick opens a
// ring the hand never asked to see.
#include "ui/MarkingMenuGesture.h"

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
    std::cout << "-- the thresholds are what Fusion's hand expects --" << std::endl;
    check(kMarkingGestureTravel == 25.0, "a gesture starts 25 logical pixels out");
    check(kMarkingHoldMs == 350.0, "a hold is 350 ms");

    std::cout << "-- a click: up quickly, without going anywhere --" << std::endl;
    check(ClassifyRightPress(100, 100, 100, 100, 0, true) == RightPress::Click,
          "released where it went down, at once, is a click");
    check(ClassifyRightPress(100, 100, 103, 98, 120, true) == RightPress::Click,
          "a few pixels of wobble is still a click");
    check(ClassifyRightPress(100, 100, 100, 100, 0, false) == RightPress::Pending,
          "still down and still near: not decided yet");

    std::cout << "-- travel: the gesture threshold from both sides --" << std::endl;
    check(ClassifyRightPress(100, 100, 124.9, 100, 50, false) == RightPress::Pending,
          "24.9 px right, still down: not a gesture yet");
    check(ClassifyRightPress(100, 100, 124.9, 100, 50, true) == RightPress::Click,
          "24.9 px right and released quickly: a click, never a command");
    check(ClassifyRightPress(100, 100, 125.0, 100, 50, false) == RightPress::Gesture,
          "25 px right is a gesture");
    check(ClassifyRightPress(100, 100, 100, 74, 50, false) == RightPress::Gesture,
          "26 px up is a gesture");
    // hypot(17.6, 17.6) = 24.89, hypot(17.8, 17.8) = 25.17: distance, not
    // the larger of dx and dy, is what is measured.
    check(ClassifyRightPress(0, 0, 17.6, 17.6, 50, false) == RightPress::Pending,
          "24.9 px along a diagonal is not a gesture");
    check(ClassifyRightPress(0, 0, -17.8, 17.8, 50, false) == RightPress::Gesture,
          "25.2 px along a diagonal is");
    check(ClassifyRightPress(100, 100, 100, 20, 30, true) == RightPress::Gesture,
          "a flick whose only report is its release is still a gesture");

    std::cout << "-- time: the hold threshold from both sides --" << std::endl;
    check(ClassifyRightPress(100, 100, 102, 101, 349, false) == RightPress::Pending,
          "349 ms still is not a hold");
    check(ClassifyRightPress(100, 100, 102, 101, 350, false) == RightPress::Hold,
          "350 ms still is a hold");
    check(ClassifyRightPress(100, 100, 102, 101, 349, true) == RightPress::Click,
          "released at 349 ms is a click");
    check(ClassifyRightPress(100, 100, 102, 101, 351, true) == RightPress::Hold,
          "released at 351 ms is a hold, even if nothing noticed in between");
    check(ClassifyRightPress(100, 100, 120, 100, 600, false) == RightPress::Hold,
          "a hand that drifts 20 px while waiting is still waiting");
    check(ClassifyRightPress(100, 100, 130, 100, 400, false) == RightPress::Gesture,
          "first judged late but already 30 px out: distance wins, a gesture");

    std::cout << "-- which wedge a gesture points into --" << std::endl;
    check(MarkingGestureWedge(0, -60) == 0, "a flick up is wedge 0 (Repeat)");
    check(MarkingGestureWedge(-60, 0) == 6, "a flick left is wedge 6 (Undo / Cancel)");
    check(MarkingGestureWedge(60, 0) == 2, "a flick right is wedge 2 (Redo / OK)");
    check(MarkingGestureWedge(0, 60) == 4, "a flick down is wedge 4 (Sketch)");
    check(MarkingGestureWedge(0, -24.9) == -1, "inside the threshold points nowhere");
    check(MarkingGestureWedge(0, -25.0) == 0, "at the threshold it points");
    check(MarkingGestureWedge(3, 4) == -1, "dragged back to the start points nowhere");

    std::cout << "-- a whole press, followed --" << std::endl;
    {
        RightButtonTracker press;
        check(press.Press(100, 100) == MarkingMenuInput::Pressed, "a press says so");
        check(press.IsActive(), "and is being followed");
        check(press.Release(101, 102, 80) == MarkingMenuInput::Click, "a quick release is a click");
        check(!press.IsActive(), "and the press is over");
        check(press.Release(101, 102, 90) == MarkingMenuInput::None,
              "a second release of the same press does nothing");
    }
    {
        RightButtonTracker flick;
        flick.Press(100, 100);
        check(flick.Move(100, 90, 20) == MarkingMenuInput::None, "10 px up: nothing shown yet");
        check(flick.Move(100, 74, 40) == MarkingMenuInput::GestureMove,
              "26 px up: the trail appears");
        check(flick.Move(100, 40, 60) == MarkingMenuInput::GestureMove, "and follows the hand");
        check(flick.Release(100, 30, 90) == MarkingMenuInput::GestureRelease,
              "the release runs the gesture");
        check(MarkingGestureWedge(flick.LastX() - flick.PressX(), flick.LastY() - flick.PressY()) == 0,
              "measured from the press point, it pointed up");
    }
    {
        RightButtonTracker hold;
        hold.Press(100, 100);
        check(hold.Move(102, 101, 100) == MarkingMenuInput::None, "a still press at 100 ms: nothing");
        check(hold.Move(hold.LastX(), hold.LastY(), 360) == MarkingMenuInput::HoldMove,
              "the hold timer at 360 ms brings the ring up");
        check(hold.Move(200, 100, 500) == MarkingMenuInput::HoldMove,
              "moving 100 px with the ring up is pointing at it, not a gesture");
        check(hold.State() == RightPress::Hold, "the press stays a hold");
        check(hold.Release(200, 100, 700) == MarkingMenuInput::HoldRelease,
              "letting go is a release over the ring");
    }
    {
        RightButtonTracker pause;
        pause.Press(100, 100);
        check(pause.Move(140, 100, 50) == MarkingMenuInput::GestureMove, "a flick right");
        check(pause.Move(140, 100, 2000) == MarkingMenuInput::GestureMove,
              "that then pauses for two seconds stays a gesture -- no ring");
        check(pause.Release(103, 100, 2100) == MarkingMenuInput::GestureRelease,
              "dragged back and released: a gesture release");
        check(MarkingGestureWedge(pause.LastX() - pause.PressX(), pause.LastY() - pause.PressY()) == -1,
              "that points at nothing, so nothing runs");
    }
    {
        RightButtonTracker late;
        late.Press(100, 100);
        check(late.Release(101, 100, 400) == MarkingMenuInput::HoldRelease,
              "released still at 400 ms with no timer seen: a hold, so the ring still comes up");
    }
    {
        RightButtonTracker lost;
        lost.Press(100, 100);
        lost.Move(100, 100, 360);
        check(lost.Abandon() == MarkingMenuInput::Abandon,
              "a button that came up elsewhere abandons the press");
        check(lost.Move(150, 100, 400) == MarkingMenuInput::None, "after which moves do nothing");
        check(lost.Release(150, 100, 450) == MarkingMenuInput::None, "nor does a stray release");
        check(lost.Abandon() == MarkingMenuInput::None, "and abandoning twice says nothing");
        lost.Press(10, 10);
        check(lost.State() == RightPress::Pending && lost.IsActive(),
              "a new press starts from nothing");
    }

    std::cout << (failures == 0 ? "\nALL MARKING GESTURE TESTS PASSED\n" : "\nFAILURES\n");
    return failures == 0 ? 0 : 1;
}
