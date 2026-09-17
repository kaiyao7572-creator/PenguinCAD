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
// Run with:  linuxcad --script path/to/script.txt
//
// Commands, one per line, '#' starts a comment:
//
//   run <command.id>        invoke a registered command (e.g. sketch.create)
//   arm <ms> accept|cancel  answer the next modal dialog after <ms>. Arm
//                           this BEFORE the `run` that opens the dialog:
//                           the script loop parks inside the dialog's own
//                           event loop, so only a timer can reach in.
//   tab <Name>              bring a ribbon tab forward
//   move <x> <y>            move the cursor in the viewport
//   click <x> <y>           press+release left button
//   rclick <x> <y>          press+release right button
//   press <x> <y>           left button down
//   release <x> <y>         left button up
//   drag <x1> <y1> <x2> <y2>   press, several moves, release
//   key <Name>              Escape, Return, Delete, or a single character
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
