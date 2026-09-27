#pragma once

namespace lcad {

struct CommandContext;

// Keep a finished sketch's closed regions pickable from the model view,
// the way Fusion does: hover lights the region under the cursor, a click
// picks it (Ctrl or Shift adds or takes one back out), and E or Shift+R
// build on what was picked. Inside sketch mode SketchSelectTool already
// does this for the open sketch; this is the same thing for every sketch
// that is finished and visible.
//
// It is a permanent background handler, like the view cube's: pushed once
// for the life of the app, beneath every tool, never exclusive. While a
// tool owns the viewport, or a sketch is open, it stands aside entirely.
//
// Call once, from the window, with a context whose viewport and document
// outlive the app's event loop. Safe before the viewer is up: the picker
// does nothing until the viewer exists.
void InstallModelProfilePicker(const CommandContext& theContext);

} // namespace lcad
