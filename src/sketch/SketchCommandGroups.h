#pragma once

namespace lcad {

class CommandRegistry;

// RegisterSketchCommands lives in SketchCommands.cpp and is the only
// registration hook MainWindow calls. The sketch tab has far too many
// buttons for one file, so it fills itself in sections; each of these
// adds one ribbon section and is implemented in its own .cpp.
void AddSketchConstraintCommands(CommandRegistry& theRegistry);

} // namespace lcad
