#pragma once

#include "core/Command.h"
#include "core/Document.h"

#include <QMainWindow>
#include <QString>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class OcctViewport;
class QAction;
class QToolButton;
class QTabWidget;

// Application shell: owns the document, hosts the 3D viewport, and builds
// its ribbon/menus from whatever subsystems registered themselves in the
// CommandRegistry.
class MainWindow : public QMainWindow, public lcad::DocumentObserver
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    lcad::Document& Document() { return m_document; }
    // Bring a ribbon tab forward by its label; also used by --screenshot-tab.
    void ActivateTab(const QString& theLabel);
    // Run a registered command by id. Used by --run-command so
    // interactive flows can be exercised without a human clicking.
    bool RunCommandById(const QString& theId);
    OcctViewport* Viewport() const { return m_viewport; }

    // lcad::DocumentObserver
    void OnDocumentChanged(lcad::Document& theDocument) override;

private slots:
    void onOpenStep();
    void onExport();
    void onUndo();
    void onRedo();

private:
    void buildRibbon();
    QAction* makeCommandAction(lcad::Command* theCommand);
    void buildMenus();
    void runCommand(lcad::Command* theCommand);
    void refreshCommandStates();
    void refreshRibbonTabs();
    void redisplayDocument();

    // Turn OCCT's picking on for exactly the shape types the selection
    // filter allows, on every displayed body.
    void applySelectionFilters();

    // Read OCCT's settled selection back as durable GeometryRefs.
    void readViewportSelection();

    lcad::CommandContext makeContext();

    // Fusion's right-click marking menu, built for what is going on now:
    // Undo and Redo either side, or Cancel and OK while a tool is running.
    void showMarkingMenu(const QPoint& theGlobalPos);

    OcctViewport* m_viewport = nullptr;
    QTabWidget*   m_ribbon = nullptr;
    lcad::Document m_document;

    // One object per body, so a body can be hidden, coloured or picked on
    // its own. A single AIS_Shape for the whole document made "the model"
    // one anonymous thing the user could not act on part of.
    std::vector<Handle(AIS_InteractiveObject)> m_displayedBodies;

    std::map<lcad::Command*, QAction*> m_commandActions;
    // Flyout buttons drive their primary command directly, so their
    // enabled state has to be refreshed alongside the plain actions.
    std::vector<std::pair<QToolButton*, lcad::Command*>> m_familyButtons;
    // Every ribbon page in registration order, kept even while hidden so a
    // contextual tab can be slotted back at the right position.
    std::vector<std::pair<std::string, QWidget*>> m_ribbonPages;
    std::vector<std::string>                      m_visibleGroups;
    // Last selection-filter generation the viewport was configured for.
    std::size_t m_selectionFilterGeneration = 0;

    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;

    // What the marking menu's Repeat runs. Selection filters and camera
    // commands are not "the last thing I did" in Fusion's sense.
    lcad::Command* m_lastCommand = nullptr;
};
