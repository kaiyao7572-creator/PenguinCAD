#pragma once

#include "ui/MarkingMenuGeometry.h"

#include <cmath>

// What a right press over the canvas turns out to be, Fusion's way, kept
// free of Qt so every threshold can be tested from both sides without a
// window:
//
//   a CLICK   -- up again quickly, without going anywhere: the ring opens;
//   a GESTURE -- dragged past kMarkingGestureTravel: a flick toward a wedge,
//                which runs that wedge on release without the ring ever
//                appearing, because a hand that knows where Undo is does
//                not need to be shown it;
//   a HOLD    -- kept still past kMarkingHoldMs: the ring appears under the
//                button, and letting go over a wedge runs it.
//
// Coordinates are LOGICAL pixels, in any one frame (the viewport's own is
// what the viewport uses); only differences between them matter.
namespace lcad {

// How far a right drag must travel before it is a gesture. Far enough that
// the wobble of a click never runs a command; near enough that a flick gets
// there well before the hand stops.
constexpr double kMarkingGestureTravel = 25.0;

// How long a still right press waits before the ring comes up under it. A
// deliberate click is well under this; a hand that is waiting to be shown
// the menu is well over it.
constexpr double kMarkingHoldMs = 350.0;

enum class RightPress
{
    Pending,   // still down, still near, not held long: could be any of them
    Click,     // released quickly without going anywhere
    Gesture,   // travelled past kMarkingGestureTravel
    Hold       // stayed near past kMarkingHoldMs
};

// Classify a right press from where it went down, where the pointer is now,
// how long ago it went down, and whether the button has come back up.
//
// Distance wins over time: a pointer that is already past the threshold when
// the press is first judged late (a slow flick, or a busy event loop that
// delivered the first move after the hold time) was a hand heading
// somewhere, not a hand waiting.
inline RightPress ClassifyRightPress(double thePressX, double thePressY,
                                     double theX, double theY,
                                     double theElapsedMs, bool theReleased)
{
    if (std::hypot(theX - thePressX, theY - thePressY) >= kMarkingGestureTravel) {
        return RightPress::Gesture;
    }
    if (theElapsedMs >= kMarkingHoldMs) {
        return RightPress::Hold;
    }
    return theReleased ? RightPress::Click : RightPress::Pending;
}

// The wedge a gesture from the press point to (theDx, theDy) away points
// into, or -1 while it is still inside the threshold. The threshold doubles
// as the dead zone, so a flick that is dragged back to where it started
// and released there means "never mind", as it does in the ring.
inline int MarkingGestureWedge(double theDx, double theDy)
{
    return MarkingMenuWedgeAt(theDx, theDy, kMarkingGestureTravel);
}

// What the marking menu should do next, as the right button goes.
enum class MarkingMenuInput
{
    None,            // nothing to show yet
    Pressed,         // a new press: forget the last one
    Click,           // open the ring at the press point
    GestureMove,     // draw the trail from the press point to the pointer
    GestureRelease,  // run the wedge the gesture points into, if any
    HoldMove,        // the ring is (or now comes) up; point at it
    HoldRelease,     // let go over the ring
    Abandon          // the button came up somewhere we never heard about
};

// One right press, followed from press to release. The classification is
// STICKY once decided: a flick that pauses does not turn into a hold, and a
// ring that came up does not vanish because the hand then moved a long way
// -- that is the hand going to a wedge, which is the point of the ring.
class RightButtonTracker
{
public:
    MarkingMenuInput Press(double theX, double theY)
    {
        myPressX = theX;
        myPressY = theY;
        myLastX = theX;
        myLastY = theY;
        myState = RightPress::Pending;
        myIsActive = true;
        return MarkingMenuInput::Pressed;
    }

    // The button is still down at (theX, theY), theElapsedMs after the
    // press. Also what the hold timer calls, with the last position.
    MarkingMenuInput Move(double theX, double theY, double theElapsedMs)
    {
        if (!myIsActive) {
            return MarkingMenuInput::None;
        }
        myLastX = theX;
        myLastY = theY;
        if (myState == RightPress::Pending) {
            myState = ClassifyRightPress(myPressX, myPressY, theX, theY, theElapsedMs, false);
        }
        switch (myState) {
            case RightPress::Gesture: return MarkingMenuInput::GestureMove;
            case RightPress::Hold:    return MarkingMenuInput::HoldMove;
            default:                  return MarkingMenuInput::None;
        }
    }

    MarkingMenuInput Release(double theX, double theY, double theElapsedMs)
    {
        if (!myIsActive) {
            return MarkingMenuInput::None;
        }
        myLastX = theX;
        myLastY = theY;
        myIsActive = false;
        if (myState == RightPress::Pending) {
            myState = ClassifyRightPress(myPressX, myPressY, theX, theY, theElapsedMs, true);
        }
        switch (myState) {
            case RightPress::Gesture: return MarkingMenuInput::GestureRelease;
            case RightPress::Hold:    return MarkingMenuInput::HoldRelease;
            default:                  return MarkingMenuInput::Click;
        }
    }

    // The button is no longer down, but its release never arrived here: a
    // ring that came up under it took the pointer, or it was let go outside
    // the window. Nothing runs.
    MarkingMenuInput Abandon()
    {
        if (!myIsActive) {
            return MarkingMenuInput::None;
        }
        myIsActive = false;
        return MarkingMenuInput::Abandon;
    }

    bool       IsActive() const { return myIsActive; }
    RightPress State() const { return myState; }
    double     PressX() const { return myPressX; }
    double     PressY() const { return myPressY; }
    double     LastX() const { return myLastX; }
    double     LastY() const { return myLastY; }

private:
    double     myPressX = 0.0;
    double     myPressY = 0.0;
    double     myLastX = 0.0;
    double     myLastY = 0.0;
    RightPress myState = RightPress::Pending;
    bool       myIsActive = false;
};

} // namespace lcad
