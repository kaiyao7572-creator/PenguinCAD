#pragma once

// Registration hooks. MainWindow calls every one of these once at
// startup; each subsystem implements its own in its own directory and
// never touches MainWindow.
//
// Each function lives in exactly one .cpp:
//   RegisterSketchCommands   -> src/sketch/SketchCommands.cpp
//   RegisterFeatureCommands  -> src/features/FeatureCommands.cpp
//   RegisterGizmoCommands    -> src/gizmos/GizmoCommands.cpp
//   RegisterViewCommands     -> src/view/ViewCommands.cpp
//   RegisterInspectCommands  -> src/inspect/InspectCommands.cpp
//   CreateDockPanels         -> src/ui/Panels.cpp

class QMainWindow;

namespace lcad {

class CommandRegistry;
struct CommandContext;

void RegisterSketchCommands(CommandRegistry& theRegistry);
void RegisterFeatureCommands(CommandRegistry& theRegistry);
void RegisterSweepCommands(CommandRegistry& theRegistry);    // src/features/SweepCommands.cpp
void RegisterLoftCommands(CommandRegistry& theRegistry);     // src/features/LoftCommands.cpp
void RegisterCombineCommands(CommandRegistry& theRegistry);  // src/features/CombineCommands.cpp
void RegisterPatternCommands(CommandRegistry& theRegistry);  // src/features/PatternCommands.cpp
void RegisterIoCommands(CommandRegistry& theRegistry);       // src/io/IoCommands.cpp
void RegisterGizmoCommands(CommandRegistry& theRegistry);
void RegisterViewCommands(CommandRegistry& theRegistry);
void RegisterUnitsCommand(CommandRegistry& theRegistry);   // src/view/UnitsCommand.cpp
void RegisterInspectCommands(CommandRegistry& theRegistry);

// Builds the browser tree / timeline / properties dock widgets and adds
// them to the main window. Implemented in src/ui/Panels.cpp.
void CreateDockPanels(QMainWindow* theWindow, const CommandContext& theContext);

} // namespace lcad
