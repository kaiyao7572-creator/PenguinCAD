#pragma once

#include <QString>

class MainWindow;

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

// Runs the script asynchronously against the window, stepping through the
// event loop so the app repaints between actions.
void RunInputScript(MainWindow* theWindow, const QString& thePath);

} // namespace lcad
