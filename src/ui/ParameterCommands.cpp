#include "core/Command.h"
#include "core/Document.h"
#include "ui/ParametersDialog.h"

#include <memory>
#include <string>

namespace lcad {

namespace {

// Fusion keeps Change Parameters in the Solid tab's MODIFY panel. Spelled
// out rather than shared, like CombineCommands.cpp: these have to match
// the strings the other Modify tools use, or the button lands in a panel
// of its own.
const char* const kSolidGroup = "Solid";
const char* const kModifySection = "Modify";

// Fusion's SOLID > MODIFY > Change Parameters.
class ChangeParametersCommand : public Command
{
public:
    std::string Id() const override { return "modify.change_parameters"; }
    std::string Title() const override { return "Change Parameters"; }
    std::string Group() const override { return kSolidGroup; }
    std::string Section() const override { return kModifySection; }
    std::string Icon() const override { return "🧮"; }
    std::string Description() const override
    {
        return "Define named parameters and drive feature dimensions with expressions";
    }

    // Deliberately not "has features": an empty design is exactly where
    // Fusion users start, defining parameters before drawing anything.
    bool IsEnabled(const CommandContext& theContext) const override
    {
        return theContext.document != nullptr;
    }

    void Execute(CommandContext& theContext) override
    {
        if (theContext.document == nullptr) {
            return;
        }
        // Modal, like Fusion's. Nothing is pending when it closes: every
        // edit made in it was applied, and made undoable, as it happened.
        ParametersDialog dialog(theContext.document, theContext.parent);
        dialog.exec();
    }
};

} // namespace

// MainWindow calls this once at startup, through the declaration in
// core/Registration.h.
void RegisterParameterCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<ChangeParametersCommand>());
}

} // namespace lcad
