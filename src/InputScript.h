#pragma once

#include <QString>

class MainWindow;
class OcctViewport;
class QString;

// Replays a text script of synthetic input against the running app, so
// interactive behaviour can be exercised and screenshotted without a human
// at the mouse. This exists because sketching is all feel -- chaining,
// previews, snapping -- and none of it can be judged from source code.
//
// Run with:  penguincad --script path/to/script.txt
//
// Commands, one per line, '#' starts a comment:
//
//   run <command.id>        invoke a registered command (e.g. sketch.create)
//   popup dump|shot <path>  list or photograph the open popup (the
//                           right-click marking menu)
//   popup move|click <dx> <dy>
//                           point or click at an offset from its centre
//   popup release <dx> <dy> let the held right button go there, delivered
//                           to the ring the way a real display does once
//                           the ring has grabbed the pointer
//   menu <Menu> > <Item>    trigger a menu-bar item by its text, e.g.
//                           "menu File > Export..." or "menu Edit > Undo"
//   arm <ms> accept|cancel  answer the next modal dialog after <ms>. Arm
//                           this BEFORE the `run` that opens the dialog:
//                           the script loop parks inside the dialog's own
//                           event loop, so only a timer can reach in.
//   arm <ms> shot <path>    photograph the open dialog
//   arm <ms> type <Label> = <text>
//                           type into the dialog field with that label
//   arm <ms> dump           print its fields, tree rows and messages, and
//                           which buttons are enabled
//   arm <ms> click <Text>   press the dialog button with that text
//   arm <ms> cell <Row> / <Column> = <text>
//                           edit the tree cell in that row and column
//   tab <Name>              bring a ribbon tab forward
//   click <x> <y> [ctrl]    press+release left button; trailing "ctrl",
//                           "shift" or "alt" are held for the click, which
//                           is how multi-select is driven
//   rclick <x> <y>          press+release right button: the marking menu
//   press <x> <y> [right|middle] [shift|ctrl|alt]
//                           a button down (left unless named); `move` then
//                           drags with it, and with those modifiers held
//   move <x> <y> [shift|ctrl|alt]
//                           move the cursor in the viewport; after a `press`
//                           it drags, adding any modifiers named here
//   release <x> <y> [right|middle]
//                           that button up (the one `press` held if not
//                           named). "press ... right", "wait 500", "release"
//                           is a HOLD: the ring comes up in between
//   drag <x1> <y1> <x2> <y2> [right|middle] [shift|ctrl|alt]
//                           press, several moves, release. "middle" pans,
//                           "middle shift" orbits, and "right" is a marking-
//                           menu gesture toward (x2, y2)
//   trail dump|shot <path>  what the gesture trail points at, or a picture
//                           of it, while a right drag is held
//   key <Name>              Escape, Return, Delete, or a single character,
//                           sent to the viewport as a tool would see it
//   hotkey <Seq>            a key as the SHORTCUT map sees it ("F6",
//                           "Shift+W", "Ctrl+R"): proves a binding fires
//   wheel <x> <y> <notches> wheel notches at a point. NEGATIVE zooms in,
//                           which is what this app's wheel handler does
//                           with a positive angleDelta -- the verb stays
//                           faithful to the event rather than flipping it
//   wait <ms>               let the event loop settle
//   shot <path>             screenshot the window (+ <path>-viewport.png)
//   echo <text>             print a marker to stdout
//   quit                    exit the app
//
// Coordinates are in viewport LOGICAL pixels with (0,0) at the viewport's
// top-left, matching what a user's cursor would report.
namespace lcad {

// Writes the 3D viewport to a PNG with correct colours.
//
// V3d_View::Dump writes the colour channels in the wrong order on this
// setup -- verified directly by setting the view background to pure red
// (1,0,0) and reading pure blue (0,0,255) back out of the file. The
// background has no lighting or material applied, so the swap can only be
// in the dump itself, not in the rendering: the app on screen is correct.
// Left unfixed, every screenshot lies about colour and any UI work done
// against these images is being judged on false evidence.
//
// Re-test with that same pure-red background if you ever suspect OCCT has
// changed behaviour.
bool DumpViewportImage(OcctViewport* theViewport, const QString& thePath);

// Runs the script asynchronously against the window, stepping through the
// event loop so the app repaints between actions.
void RunInputScript(MainWindow* theWindow, const QString& thePath);

} // namespace lcad
