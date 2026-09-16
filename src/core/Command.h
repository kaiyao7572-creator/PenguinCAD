#pragma once

#include <AIS_InteractiveContext.hxx>
#include <V3d_View.hxx>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QWidget;
class OcctViewport;

namespace lcad {

class Document;

// Everything a command needs to do its job. Passed by reference into
// Execute() so commands never reach for globals.
struct CommandContext
{
    Document*     document = nullptr;
    OcctViewport* viewport = nullptr;
    QWidget*      parent   = nullptr;   // for dialogs

    // Bring a ribbon tab forward by group name. Fusion switches to the
    // Sketch tab the instant a sketch starts, so a tool needs to be able
    // to ask for that without knowing anything about the window. Set by
    // the shell; always check it before calling.
    std::function<void(const std::string&)> activateTab;

    // Convenience accessors; both may be null before the viewer is up.
    Handle(AIS_InteractiveContext) AisContext() const;
    Handle(V3d_View) View() const;

    // Redraw the viewport now. Commands that change display state call
    // this; ones that change the Document don't need to (the document's
    // observers handle redisplay).
    void Redraw() const;
};

// A toolbar/menu action. One subclass per Fusion-style tool.
class Command
{
public:
    virtual ~Command() = default;

    // Stable unique id, e.g. "solid.extrude".
    virtual std::string Id() const = 0;

    // Button text, e.g. "Extrude".
    virtual std::string Title() const = 0;

    // Tab the button lives under: "Sketch", "Solid", "Modify", "Inspect",
    // "View", "Gizmo". New groups appear automatically as new tabs.
    virtual std::string Group() const = 0;

    // Labeled section within the tab, the way Fusion splits its ribbon
    // into CREATE | MODIFY | CONSTRAINTS | INSPECT. Commands sharing a
    // section are drawn together under one caption, separated from the
    // next section by a divider. Empty means "no caption", and those
    // buttons lead the tab.
    virtual std::string Section() const { return std::string(); }

    // Longer text for tooltips/status bar.
    virtual std::string Description() const { return Title(); }

    // Emoji or short text used as the button icon (no binary icon assets
    // in this project yet).
    virtual std::string Icon() const { return std::string(); }

    // Keyboard shortcut in Qt sequence syntax, e.g. "E" or "Ctrl+Shift+S".
    virtual std::string Shortcut() const { return std::string(); }

    // Grey the button out when this returns false. Re-queried whenever
    // the document changes.
    virtual bool IsEnabled(const CommandContext& theContext) const
    {
        (void)theContext;
        return true;
    }

    // Checkable buttons (display-mode toggles, grid on/off...) return true
    // here and report their state from IsChecked().
    virtual bool IsCheckable() const { return false; }
    virtual bool IsChecked(const CommandContext& theContext) const
    {
        (void)theContext;
        return false;
    }

    // Do the thing.
    virtual void Execute(CommandContext& theContext) = 0;
};

using CommandPtr = std::unique_ptr<Command>;

// Global registry the UI builds its toolbars and menus from.
class CommandRegistry
{
public:
    static CommandRegistry& Instance();

    void Add(CommandPtr theCommand);

    // All commands in a group, in registration order.
    std::vector<Command*> InGroup(const std::string& theGroup) const;

    // Group names in first-registration order, so tab order is
    // determined by the order Register*Commands() are called.
    std::vector<std::string> Groups() const;

    // Section names within a group, in first-registration order.
    std::vector<std::string> SectionsInGroup(const std::string& theGroup) const;

    // Commands in one section of one group, in registration order.
    std::vector<Command*> InSection(const std::string& theGroup,
                                     const std::string& theSection) const;

    Command* Find(const std::string& theId) const;

    // Mark a tab as contextual: it only appears while the predicate holds.
    // Fusion's Sketch tab works this way -- it doesn't exist until you're
    // in a sketch, and vanishes again when you finish. A group with no
    // predicate registered is always visible.
    void SetGroupVisibility(const std::string&                             theGroup,
                             std::function<bool(const CommandContext&)>     thePredicate);
    bool IsGroupVisible(const std::string& theGroup, const CommandContext& theContext) const;

    const std::vector<CommandPtr>& All() const { return myCommands; }

private:
    CommandRegistry() = default;

    std::vector<CommandPtr>  myCommands;
    std::vector<std::string> myGroupOrder;
    std::vector<std::pair<std::string, std::function<bool(const CommandContext&)>>>
        myGroupVisibility;
};

} // namespace lcad
