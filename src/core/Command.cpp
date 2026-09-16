#include "core/Command.h"

#include "OcctViewport.h"

#include <algorithm>

namespace lcad {

Handle(AIS_InteractiveContext) CommandContext::AisContext() const
{
    if (viewport == nullptr) {
        return Handle(AIS_InteractiveContext)();
    }
    return viewport->Context();
}

Handle(V3d_View) CommandContext::View() const
{
    if (viewport == nullptr) {
        return Handle(V3d_View)();
    }
    return viewport->View();
}

void CommandContext::Redraw() const
{
    Handle(V3d_View) view = View();
    if (!view.IsNull()) {
        view->Redraw();
    }
}

CommandRegistry& CommandRegistry::Instance()
{
    static CommandRegistry theInstance;
    return theInstance;
}

void CommandRegistry::Add(CommandPtr theCommand)
{
    if (!theCommand) {
        return;
    }

    const std::string group = theCommand->Group();
    if (std::find(myGroupOrder.begin(), myGroupOrder.end(), group) == myGroupOrder.end()) {
        myGroupOrder.push_back(group);
    }

    myCommands.push_back(std::move(theCommand));
}

std::vector<Command*> CommandRegistry::InGroup(const std::string& theGroup) const
{
    std::vector<Command*> result;
    for (const CommandPtr& command : myCommands) {
        if (command->Group() == theGroup) {
            result.push_back(command.get());
        }
    }
    return result;
}

std::vector<std::string> CommandRegistry::Groups() const
{
    return myGroupOrder;
}

std::vector<std::string> CommandRegistry::SectionsInGroup(const std::string& theGroup) const
{
    std::vector<std::string> sections;
    for (const CommandPtr& command : myCommands) {
        if (command->Group() != theGroup) {
            continue;
        }
        const std::string section = command->Section();
        if (std::find(sections.begin(), sections.end(), section) == sections.end()) {
            sections.push_back(section);
        }
    }
    return sections;
}

std::vector<Command*> CommandRegistry::InSection(const std::string& theGroup,
                                                  const std::string& theSection) const
{
    std::vector<Command*> result;
    for (const CommandPtr& command : myCommands) {
        if (command->Group() == theGroup && command->Section() == theSection) {
            result.push_back(command.get());
        }
    }
    return result;
}

Command* CommandRegistry::Find(const std::string& theId) const
{
    for (const CommandPtr& command : myCommands) {
        if (command->Id() == theId) {
            return command.get();
        }
    }
    return nullptr;
}

} // namespace lcad
