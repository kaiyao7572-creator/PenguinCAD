#pragma once

#include "core/Command.h"
#include "core/Document.h"
#include "ui/MarkingMenu.h"

#include <QByteArray>
#include <QMainWindow>
#include <QString>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class OcctViewport;
class QAction;
class QCloseEvent;
class QMenu;
namespace lcad { class MarkingMenuController; }
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

    // ---- the design's own file (.pcad) ----
    //
    // File > Open and File > Save without their dialogs, for the command
    // line ("penguincad part.pcad") and --script. Open asks about unsaved
    // changes first, exactly as the menu does. Both return false when the
    // file could not be read or written, or the user cancelled; with
    // theError given the message goes there instead of into a message box,
    // so a script is never left parked in front of one.
    bool OpenDesignFile(const QString& thePath, QString* theError = nullptr);
    bool SaveDesignFile(const QString& thePath, QString* theError = nullptr);

    // Empty until the design has been saved or was opened from a file.
    const QString& DesignFile() const { return m_designFile; }

    // lcad::DocumentObserver
    void OnDocumentChanged(lcad::Document& theDocument) override;

protected:
    // Closing with unsaved changes asks first, and Cancel keeps the window.
    void closeEvent(QCloseEvent* theEvent) override;

private slots:
    void onNewDesign();
    void onOpenDesign();
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

    // Draw every construction plane, axis and point the timeline has
    // evaluated, the way Fusion shows them: translucent orange planes
    // sized to the model, dashed axes, point markers.
    void redisplayConstruction();

    // Turn OCCT's picking on for exactly the shape types the selection
    // filter allows, on every displayed body.
    void applySelectionFilters();

    // Read OCCT's settled selection back as durable GeometryRefs.
    void readViewportSelection();

    lcad::CommandContext makeContext();

    // Save, or Save As for a design that has never been saved. False when
    // the user cancelled the dialog or the write failed.
    bool saveDesign();
    bool saveDesignAs();

    // Ask Save / Don't Save / Cancel when the design has unsaved changes.
    // True when it is fine to throw the design away now.
    bool maybeSaveChanges();

    // End whatever is editing the current design -- a tool mid-pick, an
    // open sketch, a selection -- before another design replaces it: they
    // all hold names and picks that would point into the wrong model.
    void leaveEditingModes();

    // The design as it would be saved right now. Unsaved changes are the
    // difference between this and what the file holds, which catches every
    // kind of edit -- in-place sketch changes, renames, a hidden body, the
    // default unit -- and knows an undo back to the saved state is clean.
    QByteArray currentDesignText() const;
    void markSaved();
    void refreshModified();
    void refreshTitle();

    // File > Open Recent, newest first, kept in QSettings.
    void rememberRecentFile(const QString& thePath);
    void rebuildRecentMenu();

    // What Fusion's marking menu holds right now, clockwise from the top:
    // Undo and Redo either side, or Cancel and OK while a tool is running.
    // The ONE list the ring and a right-button gesture both read, so they
    // can never disagree about what is where.
    lcad::MarkingMenu::ItemList markingMenuItems();

    OcctViewport* m_viewport = nullptr;
    QTabWidget*   m_ribbon = nullptr;
    lcad::Document m_document;

    // One object per body, so a body can be hidden, coloured or picked on
    // its own. A single AIS_Shape for the whole document made "the model"
    // one anonymous thing the user could not act on part of.
    std::vector<Handle(AIS_InteractiveObject)> m_displayedBodies;

    // Construction planes, axes and points: drawn, not pickable yet.
    std::vector<Handle(AIS_InteractiveObject)> m_displayedConstruction;

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

    // The right button over the canvas: click, flick or hold.
    std::unique_ptr<lcad::MarkingMenuController> m_markingMenu;

    QString    m_designFile;     // absolute path, empty until saved or opened
    QByteArray m_savedText;      // currentDesignText() as of the last save/open/new
    QMenu*     m_recentMenu = nullptr;
};
