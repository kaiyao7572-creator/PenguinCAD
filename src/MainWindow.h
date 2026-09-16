#pragma once

#include "core/Command.h"
#include "core/Document.h"

#include <QMainWindow>
#include <QString>

#include <map>
#include <memory>
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
    void onExportStl();
    void onUndo();
    void onRedo();

private:
    void buildRibbon();
    QAction* makeCommandAction(lcad::Command* theCommand);
    void buildMenus();
    void runCommand(lcad::Command* theCommand);
    void refreshCommandStates();
    void redisplayDocument();
    lcad::CommandContext makeContext();

    OcctViewport* m_viewport = nullptr;
    QTabWidget*   m_ribbon = nullptr;
    lcad::Document m_document;

    // Shape currently shown in the viewer, so we can replace it cleanly.
    Handle(AIS_InteractiveObject) m_displayedShape;

    std::map<lcad::Command*, QAction*> m_commandActions;
    // Flyout buttons drive their primary command directly, so their
    // enabled state has to be refreshed alongside the plain actions.
    std::vector<std::pair<QToolButton*, lcad::Command*>> m_familyButtons;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
};
