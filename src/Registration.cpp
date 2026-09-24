#include "core/Registration.h"

#include "core/Command.h"

namespace lcad {

void RegisterAllCommands(CommandRegistry& theRegistry)
{
    // Every subsystem contributes its tools here, in the order the ribbon
    // shows them. Each lives in its own directory and knows nothing about
    // the window; --check-shortcuts runs this same list with no window at
    // all, so there is one list and the check cannot miss a command.
    RegisterSketchCommands(theRegistry);
    RegisterFeatureCommands(theRegistry);
    RegisterSweepCommands(theRegistry);
    RegisterLoftCommands(theRegistry);
    RegisterCombineCommands(theRegistry);
    RegisterPatternCommands(theRegistry);
    RegisterGizmoCommands(theRegistry);
    // After Move, as Fusion orders its MODIFY panel.
    RegisterParameterCommands(theRegistry);
    RegisterViewCommands(theRegistry);
    RegisterInspectCommands(theRegistry);
}

} // namespace lcad
