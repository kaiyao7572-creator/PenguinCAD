#include "MainWindow.h"

#include "core/GeometrySelection.h"

#include "OcctViewport.h"
#include "StepImport.h"
#include "core/ConstructionGeometry.h"
#include "core/ProfileSelection.h"
#include "core/Units.h"
#include "io/ExportDialog.h"
#include "io/NativeFormat.h"
#include "ui/CommandIcon.h"
#include "ui/MarkingMenu.h"
#include "ui/MarkingMenuController.h"
#include "core/Registration.h"
#include "core/ShapeFeature.h"
#include "sketch/ModelProfilePicker.h"

#include <AIS_Point.hxx>
#include <AIS_Shape.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <Bnd_Box.hxx>
#include <Geom_CartesianPoint.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>
#include <StdSelect_BRepOwner.hxx>

#include <QAction>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cctype>

using lcad::Command;
using lcad::CommandContext;
using lcad::CommandRegistry;

namespace {

// A set of tool variants that share one ribbon button, e.g. the three
// rectangle tools or all twelve constraints.
struct CommandFamily
{
    std::string           key;
    std::string           label;
    Command*              primary = nullptr;   // the button's default action
    std::vector<Command*> members;
};

// Command ids are dotted and encode their family: "sketch.rectangle",
// "sketch.rectangle.centre", "sketch.rectangle.three". Everything sharing
// the first two components belongs on one button, which is what keeps the
// ribbon the width of a screen instead of the width of three.
std::string FamilyKeyOf(const std::string& theId)
{
    const std::size_t first = theId.find('.');
    if (first == std::string::npos) {
        return theId;
    }
    const std::size_t second = theId.find('.', first + 1);
    return second == std::string::npos ? theId : theId.substr(0, second);
}

std::string FamilyLabelFor(const std::string& theKey)
{
    const std::size_t dot = theKey.rfind('.');
    std::string word = (dot == std::string::npos) ? theKey : theKey.substr(dot + 1);
    if (!word.empty()) {
        word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
    }
    return word;
}

std::vector<CommandFamily> GroupIntoFamilies(const std::vector<Command*>& theCommands)
{
    std::vector<CommandFamily> families;
    for (Command* command : theCommands) {
        const std::string key = FamilyKeyOf(command->Id());

        auto found = std::find_if(families.begin(), families.end(),
                                  [&key](const CommandFamily& f) { return f.key == key; });
        if (found == families.end()) {
            CommandFamily family;
            family.key = key;
            family.label = FamilyLabelFor(key);
            families.push_back(family);
            found = std::prev(families.end());
        }
        found->members.push_back(command);

        // The variant whose id IS the family key (e.g. "sketch.rectangle")
        // is the one Fusion puts on the button face by default.
        if (command->Id() == key) {
            found->primary = command;
        }
    }
    return families;
}

// The text fallback for a command without a bundled icon: whatever it
// gave as its icon on a line above the label, the way the ribbon looked
// before it had pictures. A resource that failed to load shows the label
// alone rather than its own path.
QString IconTextAbove(const std::string& theIcon, const QString& theLabel)
{
    if (theIcon.empty() || lcad::IsIconResource(theIcon)) {
        return theLabel;
    }
    return QString::fromStdString(theIcon) + "\n" + theLabel;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    m_viewport = new OcctViewport(this);
    // OCCT fires this once the selection has actually settled, which is
    // after the mouse handler has returned -- reading it from the click
    // itself would read the previous selection.
    m_viewport->SetSelectionCallback([this]() { readViewportSelection(); });

    lcad::RegisterAllCommands(CommandRegistry::Instance());

    m_markingMenu = std::make_unique<lcad::MarkingMenuController>(
        this, [this]() { return markingMenuItems(); });
    m_viewport->SetMarkingMenuCallback(
        [this](lcad::MarkingMenuInput theInput, const QPoint& thePress, const QPoint& theCursor) {
            m_markingMenu->Feed(theInput, thePress, theCursor);
        });

    buildRibbon();

    // Ribbon on top, viewport filling the rest.
    QWidget* central = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (m_ribbon != nullptr) {
        layout->addWidget(m_ribbon);
    }
    layout->addWidget(m_viewport, 1);
    setCentralWidget(central);

    buildMenus();

    m_document.AddObserver(this);
    lcad::CreateDockPanels(this, makeContext());

    // A finished sketch's regions stay pickable from the model view, as in
    // Fusion -- click one, press E. A permanent background handler at the
    // bottom of the viewport's interaction stack, beneath every tool.
    lcad::InstallModelProfilePicker(makeContext());

    resize(1400, 900);
    statusBar()->showMessage("Ready");

    // A new window is an untitled design with nothing to save yet.
    markSaved();
    refreshTitle();

    // Button states depend on things that appear after this constructor
    // runs -- the OCCT view is created lazily on first paint, and tools
    // like the view cube materialise themselves the first time they are
    // polled. A light periodic refresh keeps enable/checked state and the
    // contextual tabs honest without every subsystem needing to signal us.
    QTimer* stateTimer = new QTimer(this);
    stateTimer->setInterval(300);
    connect(stateTimer, &QTimer::timeout, this, &MainWindow::refreshCommandStates);
    stateTimer->start();

    refreshCommandStates();
}

MainWindow::~MainWindow()
{
    m_document.RemoveObserver(this);
}

lcad::CommandContext MainWindow::makeContext()
{
    CommandContext context;
    context.document = &m_document;
    context.viewport = m_viewport;
    context.parent = this;
    // Let tools bring their own ribbon tab forward (Create Sketch does).
    context.activateTab = [this](const std::string& theGroup) {
        if (m_ribbon == nullptr) {
            return;
        }
        const QString wanted = QString::fromStdString(theGroup);
        for (int i = 0; i < m_ribbon->count(); ++i) {
            if (m_ribbon->tabText(i) == wanted) {
                m_ribbon->setCurrentIndex(i);
                return;
            }
        }
    };
    return context;
}

void MainWindow::ActivateTab(const QString& theLabel)
{
    if (m_ribbon == nullptr) {
        return;
    }
    for (int i = 0; i < m_ribbon->count(); ++i) {
        if (m_ribbon->tabText(i) == theLabel) {
            m_ribbon->setCurrentIndex(i);
            return;
        }
    }
}

bool MainWindow::RunCommandById(const QString& theId)
{
    Command* command = CommandRegistry::Instance().Find(theId.toStdString());
    if (command == nullptr) {
        return false;
    }
    runCommand(command);
    return true;
}

void MainWindow::buildRibbon()
{
    CommandRegistry& registry = CommandRegistry::Instance();
    const std::vector<std::string> groups = registry.Groups();
    if (groups.empty()) {
        return;
    }

    m_ribbon = new QTabWidget(this);
    m_ribbon->setDocumentMode(true);
    m_ribbon->setMinimumHeight(158);
    m_ribbon->setMaximumHeight(186);

    for (const std::string& group : groups) {
        // Each tab is a row of labeled sections, Fusion-style: the buttons
        // sit above a small caption, with a divider between sections.
        QWidget* page = new QWidget(m_ribbon);
        QHBoxLayout* pageLayout = new QHBoxLayout(page);
        pageLayout->setContentsMargins(4, 2, 4, 2);
        pageLayout->setSpacing(2);

        for (const std::string& section : registry.SectionsInGroup(group)) {
            QWidget* sectionWidget = new QWidget(page);
            QVBoxLayout* sectionLayout = new QVBoxLayout(sectionWidget);
            sectionLayout->setContentsMargins(2, 0, 2, 0);
            sectionLayout->setSpacing(0);

            // Plain tool buttons rather than a QToolBar: a QToolBar quietly
            // hides overflow behind an extension arrow when it's squeezed,
            // which silently swallowed most of the tools here.
            QWidget* buttonRow = new QWidget(sectionWidget);
            QHBoxLayout* buttonLayout = new QHBoxLayout(buttonRow);
            buttonLayout->setContentsMargins(0, 0, 0, 0);
            buttonLayout->setSpacing(1);

            for (const CommandFamily& family : GroupIntoFamilies(registry.InSection(group, section))) {
                QToolButton* button = new QToolButton(buttonRow);
                button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
                button->setAutoRaise(true);
                button->setMinimumHeight(74);
                // Fusion's ribbon draws its tools at 32 px; the menus mirror
                // the same actions at the style's small size.
                button->setIconSize(QSize(32, 32));
                QFont buttonFont = button->font();
                buttonFont.setPointSizeF(buttonFont.pointSizeF() * 0.76);
                button->setFont(buttonFont);

                if (family.members.size() == 1) {
                    button->setDefaultAction(makeCommandAction(family.members.front()));
                } else {
                    // Fusion collapses tool variants (all three rectangles,
                    // every constraint) behind one button with a flyout.
                    // Without this the ribbon runs several screens wide.
                    QMenu* menu = new QMenu(button);
                    Command* primary = family.primary;
                    for (Command* member : family.members) {
                        menu->addAction(makeCommandAction(member));
                        if (primary == nullptr) {
                            primary = member;
                        }
                    }
                    button->setMenu(menu);
                    button->setPopupMode(QToolButton::MenuButtonPopup);

                    // Deliberately NOT setDefaultAction: a default action
                    // re-syncs the button's text from itself whenever its
                    // enabled state changes, which would overwrite the
                    // family label with the variant's own name.
                    const QString label = QString::fromStdString(family.label);
                    const QIcon icon = lcad::CommandIcon(primary->Icon(), palette());
                    if (!icon.isNull()) {
                        button->setIcon(icon);
                        button->setText(label);
                    } else {
                        button->setText(IconTextAbove(primary->Icon(), label));
                    }
                    button->setToolTip(QString::fromStdString(primary->Description()));
                    connect(button, &QToolButton::clicked, this,
                            [this, primary]() { runCommand(primary); });

                    m_familyButtons.emplace_back(button, primary);
                }

                // A common width keeps the row even; a label too long for
                // it even on two lines widens its own button rather than
                // being elided. Fixed, not a minimum: the tab's scroll area
                // squeezes its page down to each button's minimum width,
                // which is how a 96 px minimum still drew "Plan...ugh".
                button->setFixedWidth(std::max(96, button->sizeHint().width()));

                buttonLayout->addWidget(button);
            }

            sectionLayout->addWidget(buttonRow, 1);

            if (!section.empty()) {
                QLabel* caption = new QLabel(QString::fromStdString(section).toUpper(),
                                             sectionWidget);
                caption->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
                QFont captionFont = caption->font();
                captionFont.setPointSizeF(captionFont.pointSizeF() * 0.8);
                caption->setFont(captionFont);
                // Dimmed caption, taken from the palette so it themes with
                // the rest of the app rather than being a hardcoded grey.
                QPalette captionPalette = caption->palette();
                captionPalette.setColor(QPalette::WindowText,
                                        captionPalette.color(QPalette::Disabled,
                                                             QPalette::WindowText));
                caption->setPalette(captionPalette);
                sectionLayout->addWidget(caption, 0);
            }

            pageLayout->addWidget(sectionWidget, 0);

            QFrame* divider = new QFrame(page);
            divider->setFrameShape(QFrame::VLine);
            divider->setFrameShadow(QFrame::Sunken);
            pageLayout->addWidget(divider, 0);
        }

        pageLayout->addStretch(1);

        // Tabs can hold more tools than fit; let the row scroll rather
        // than squeezing buttons into unreadable slivers.
        QScrollArea* scroll = new QScrollArea(m_ribbon);
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        // Parented to the tab widget but not yet in it, a page would show
        // as a stray child painted over the ribbon. refreshRibbonTabs()
        // decides which ones actually get shown.
        scroll->hide();
        m_ribbonPages.emplace_back(group, scroll);
    }
}

namespace {

// Ribbon buttons are narrow, and Qt elides a too-long label into things
// like "2-...le". A label longer than theMaxChars is split onto two lines
// at the word break that makes them most even -- "Plane Through / Three
// Points", where filling the first line greedily gave "Plane / Through
// Three Points" and a second line no button could hold. What still does
// not fit widens its button rather than being cut (see buildRibbon).
QString WrapButtonLabel(const QString& theTitle, int theMaxChars)
{
    // "Move/Rotate" may break after its slash, but on one line it keeps no
    // space there: splitting at " /" drew "Move /Rotate" on the ribbon.
    QString spaced = theTitle;
    spaced.replace('/', "/ ");
    const QStringList words = spaced.split(' ', Qt::SkipEmptyParts);
    const auto joined = [](const QStringList& theWords) {
        return theWords.join(' ').replace("/ ", "/");
    };
    const QString oneLine = joined(words);
    if (words.size() < 2 || oneLine.length() <= theMaxChars) {
        return oneLine;
    }
    qsizetype best = 1;
    qsizetype bestLongest = oneLine.length();
    for (qsizetype split = 1; split < words.size(); ++split) {
        const qsizetype longest = std::max(joined(words.mid(0, split)).length(),
                                           joined(words.mid(split)).length());
        // On a tie the later break, so "Press Pull / Arrow" keeps its pair.
        if (longest <= bestLongest) {
            bestLongest = longest;
            best = split;
        }
    }
    return joined(words.mid(0, best)) + '\n' + joined(words.mid(best));
}

} // namespace

// Adds or removes contextual tabs so the ribbon matches the current mode.
// Fusion's Sketch tab only exists while a sketch is open; this is what
// makes that true here.
void MainWindow::refreshRibbonTabs()
{
    if (m_ribbon == nullptr) {
        return;
    }

    CommandRegistry& registry = CommandRegistry::Instance();
    CommandContext context = makeContext();

    std::vector<std::string> shouldBeVisible;
    for (const auto& page : m_ribbonPages) {
        if (registry.IsGroupVisible(page.first, context)) {
            shouldBeVisible.push_back(page.first);
        }
    }
    if (shouldBeVisible == m_visibleGroups) {
        return;
    }

    // A tab that has just appeared is the one the user wants to be on --
    // entering a sketch should land you in the sketch tools. This only
    // applies to a genuine transition; on the very first build every tab
    // is "new", and jumping to the last one would be wrong.
    std::string newlyShown;
    if (!m_visibleGroups.empty()) {
        for (const std::string& group : shouldBeVisible) {
            if (std::find(m_visibleGroups.begin(), m_visibleGroups.end(), group)
                == m_visibleGroups.end()) {
                newlyShown = group;
            }
        }
    }

    // Rebuild the tab bar in registration order. Pages are kept alive, not
    // destroyed, so their buttons and state survive being hidden.
    //
    // removeTab() only detaches a page from the tab bar -- it stays a child
    // of the QTabWidget and would otherwise keep painting itself on top of
    // everything as a stray floating widget, so hide it explicitly.
    while (m_ribbon->count() > 0) {
        QWidget* detached = m_ribbon->widget(0);
        m_ribbon->removeTab(0);
        if (detached != nullptr) {
            detached->hide();
        }
    }
    for (const auto& page : m_ribbonPages) {
        if (std::find(shouldBeVisible.begin(), shouldBeVisible.end(), page.first)
            != shouldBeVisible.end()) {
            m_ribbon->addTab(page.second, QString::fromStdString(page.first));
            page.second->show();
        }
    }
    m_visibleGroups = shouldBeVisible;

    if (!newlyShown.empty()) {
        ActivateTab(QString::fromStdString(newlyShown));
    }
}

QAction* MainWindow::makeCommandAction(lcad::Command* theCommand)
{
    const QString title = WrapButtonLabel(QString::fromStdString(theCommand->Title()), 9);
    const QIcon icon = lcad::CommandIcon(theCommand->Icon(), palette());

    QAction* action = new QAction(icon.isNull() ? IconTextAbove(theCommand->Icon(), title) : title,
                                  this);
    action->setIcon(icon);
    action->setToolTip(QString::fromStdString(theCommand->Description()));
    action->setStatusTip(QString::fromStdString(theCommand->Description()));
    action->setCheckable(theCommand->IsCheckable());

    const std::string shortcut = theCommand->Shortcut();
    if (!shortcut.empty()) {
        action->setShortcut(QKeySequence(QString::fromStdString(shortcut)));
        // Shortcuts must work while the 3D window has focus.
        action->setShortcutContext(Qt::ApplicationShortcut);
        addAction(action);
    }

    connect(action, &QAction::triggered, this,
            [this, theCommand]() { runCommand(theCommand); });

    m_commandActions[theCommand] = action;
    return action;
}

void MainWindow::buildMenus()
{
    QMenu* fileMenu = menuBar()->addMenu("&File");

    // The desktop's keys, not Fusion's defaults, where the two differ:
    // Ctrl+Shift+S is Save As in every other Linux app. Application-wide,
    // like the ribbon's, so they work while the 3D view has the focus.
    // main.cpp's --check-shortcuts lists the same four keys.
    const auto fileAction = [this, fileMenu](const QString& theText, const QKeySequence& theKey,
                                             auto theSlot) {
        QAction* action = fileMenu->addAction(theText);
        action->setShortcut(theKey);
        action->setShortcutContext(Qt::ApplicationShortcut);
        connect(action, &QAction::triggered, this, theSlot);
        return action;
    };
    fileAction("&New Design", QKeySequence(Qt::CTRL | Qt::Key_N), [this]() { onNewDesign(); });
    fileAction("&Open...", QKeySequence(Qt::CTRL | Qt::Key_O), [this]() { onOpenDesign(); });

    m_recentMenu = fileMenu->addMenu("Open &Recent");
    // Rebuilt as it opens, so a file deleted since, or a list another
    // window grew, is shown as it is now.
    connect(m_recentMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
    rebuildRecentMenu();

    fileMenu->addSeparator();
    fileAction("&Save", QKeySequence(Qt::CTRL | Qt::Key_S), [this]() { saveDesign(); });
    fileAction("Save &As...", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S),
               [this]() { saveDesignAs(); });

    fileMenu->addSeparator();
    // A STEP file is geometry with no recipe, so it joins the open design
    // as one imported body rather than being opened as a design of its own.
    QAction* importAction = fileMenu->addAction("&Import STEP...");
    connect(importAction, &QAction::triggered, this, &MainWindow::onOpenStep);

    // One Export for every format, as in Fusion's File menu; the type is
    // picked inside the dialog.
    QAction* exportAction = fileMenu->addAction("&Export...");
    connect(exportAction, &QAction::triggered, this, &MainWindow::onExport);

    fileMenu->addSeparator();

    QAction* quitAction = fileMenu->addAction("&Quit");
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu* editMenu = menuBar()->addMenu("&Edit");
    m_undoAction = editMenu->addAction("&Undo");
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::onUndo);

    m_redoAction = editMenu->addAction("&Redo");
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::onRedo);

    // Mirror every registered command into menus grouped by tab name, so
    // tools are reachable even when the ribbon is crowded.
    CommandRegistry& registry = CommandRegistry::Instance();
    for (const std::string& group : registry.Groups()) {
        QMenu* menu = menuBar()->addMenu(QString::fromStdString(group));
        for (Command* command : registry.InGroup(group)) {
            auto found = m_commandActions.find(command);
            if (found != m_commandActions.end()) {
                menu->addAction(found->second);
            }
        }
    }
}

void MainWindow::runCommand(Command* theCommand)
{
    if (theCommand == nullptr) {
        return;
    }
    const std::string& id = theCommand->Id();
    if (id.rfind("select.", 0) != 0 && id.rfind("view.", 0) != 0) {
        m_lastCommand = theCommand;
    }
    CommandContext context = makeContext();
    theCommand->Execute(context);
    refreshCommandStates();
    // Not every command changes the document through a rebuild: View >
    // Units changes what the design saves without the model noticing.
    refreshModified();
}

void MainWindow::refreshCommandStates()
{
    refreshRibbonTabs();

    // A filter command has no handle on the window or the body list, so
    // it just bumps a counter and the change is picked up here.
    if (m_selectionFilterGeneration != lcad::GeometrySelection::Instance().FilterGeneration()) {
        applySelectionFilters();
    }

    CommandContext context = makeContext();
    for (auto& entry : m_commandActions) {
        Command* command = entry.first;
        QAction* action = entry.second;
        action->setEnabled(command->IsEnabled(context));
        if (command->IsCheckable()) {
            const bool checked = command->IsChecked(context);
            if (action->isChecked() != checked) {
                QSignalBlocker blocker(action);
                action->setChecked(checked);
            }
        }
    }

    for (const auto& entry : m_familyButtons) {
        entry.first->setEnabled(entry.second->IsEnabled(context));
    }

    if (m_undoAction != nullptr) {
        m_undoAction->setEnabled(m_document.CanUndo());
    }
    if (m_redoAction != nullptr) {
        m_redoAction->setEnabled(m_document.CanRedo());
    }
}

void MainWindow::OnDocumentChanged(lcad::Document& /*theDocument*/)
{
    redisplayDocument();
    refreshCommandStates();
    refreshModified();

    const std::vector<std::string>& errors = m_document.Errors();
    if (!errors.empty()) {
        statusBar()->showMessage(QString::fromStdString(errors.front()));
    }
}

void MainWindow::redisplayDocument()
{
    Handle(AIS_InteractiveContext) context = m_viewport->Context();
    if (context.IsNull()) {
        return;
    }

    for (const Handle(AIS_InteractiveObject)& object : m_displayedBodies) {
        if (!object.IsNull()) {
            context->Remove(object, Standard_False);
        }
    }
    m_displayedBodies.clear();

    // One AIS object per body rather than one for the whole document.
    // Selection mode 0 is the object itself, so clicking anywhere on a
    // body picks that body -- which is what makes the Bodies folder, and
    // eventually per-body appearance and Combine, mean anything.
    for (const lcad::BodyPtr& body : m_document.Bodies()) {
        if (!body || !body->IsVisible() || body->Shape().IsNull()) {
            continue;
        }
        Handle(AIS_Shape) aisShape = new AIS_Shape(body->Shape());
        // Displayed with no selection mode, then LOADED into the selection
        // manager so applySelectionFilters can activate the modes the
        // filter allows. The Load is not optional: Display with -1 skips
        // it, and Activate on an unloaded object reports the mode as
        // active while never building the selection primitives -- the body
        // looks armed and is completely unpickable.
        context->Display(aisShape, AIS_Shaded, -1, Standard_False);
        context->Load(aisShape);
        m_displayedBodies.push_back(aisShape);
    }

    applySelectionFilters();
    redisplayConstruction();

    Handle(V3d_View) view = m_viewport->View();
    if (!view.IsNull()) {
        view->Redraw();
    }
}

void MainWindow::redisplayConstruction()
{
    Handle(AIS_InteractiveContext) context = m_viewport->Context();
    if (context.IsNull()) {
        return;
    }
    for (const Handle(AIS_InteractiveObject)& object : m_displayedConstruction) {
        context->Remove(object, Standard_False);
    }
    m_displayedConstruction.clear();

    // Fusion sizes a construction plane to the model it sits in, so it
    // reads as a plane through the part rather than a speck beside it or a
    // sheet swallowing the view.
    double half = 25.0;
    if (!m_document.Shape().IsNull()) {
        Bnd_Box bounds;
        BRepBndLib::Add(m_document.Shape(), bounds);
        if (!bounds.IsVoid()) {
            double x0, y0, z0, x1, y1, z1;
            bounds.Get(x0, y0, z0, x1, y1, z1);
            half = std::max(half, 0.6 * std::max({x1 - x0, y1 - y0, z1 - z0}));
        }
    }

    const Quantity_Color orange(0.96, 0.66, 0.20, Quantity_TOC_sRGB);
    const Quantity_Color edge(0.85, 0.50, 0.10, Quantity_TOC_sRGB);

    // Decorations: displayed with selection mode -1, or the two-argument
    // Display would make them steal clicks from the bodies behind them --
    // the trap the origin axes fell into (docs/ARCHITECTURE.md).
    const auto show = [&](const Handle(AIS_InteractiveObject)& theObject, int theMode) {
        context->Display(theObject, theMode, -1, Standard_False);
        m_displayedConstruction.push_back(theObject);
    };

    const std::size_t limit = std::min(m_document.RollbackIndex(), m_document.FeatureCount());
    for (std::size_t i = 0; i < limit; ++i) {
        const lcad::FeaturePtr& feature = m_document.Features()[i];
        lcad::ConstructionGeometry* geometry =
            feature ? lcad::AsConstructionGeometry(feature.get()) : nullptr;
        // A failed or suppressed one has nothing current to show; its last
        // result would be a plane the model no longer has.
        if (geometry == nullptr || feature->IsSuppressed() || !feature->LastError().empty()) {
            continue;
        }
        try {
            gp_Ax3 plane;
            gp_Ax1 axis;
            gp_Pnt point;
            if (geometry->AsPlane(plane)) {
                const TopoDS_Face face =
                    BRepBuilderAPI_MakeFace(gp_Pln(plane), -half, half, -half, half).Face();
                Handle(AIS_Shape) sheet = new AIS_Shape(face);
                // Colour BEFORE display: an AIS_Shape only has a shading
                // aspect once a colour is set (docs/HANDOFF.md 1.2c).
                sheet->SetColor(orange);
                sheet->SetTransparency(0.75);
                show(sheet, AIS_Shaded);

                Handle(AIS_Shape) rim = new AIS_Shape(face);
                rim->SetColor(edge);
                rim->SetWidth(1.5);
                show(rim, AIS_WireFrame);
            } else if (geometry->AsAxis(axis)) {
                const gp_Pnt a = axis.Location().Translated(gp_Vec(axis.Direction()) * -half);
                const gp_Pnt b = axis.Location().Translated(gp_Vec(axis.Direction()) * half);
                Handle(AIS_Shape) line = new AIS_Shape(BRepBuilderAPI_MakeEdge(a, b).Edge());
                line->SetColor(edge);
                line->SetWidth(2.0);
                line->Attributes()->WireAspect()->SetTypeOfLine(Aspect_TOL_DASH);
                show(line, AIS_WireFrame);
            } else if (geometry->AsPoint(point)) {
                Handle(AIS_Point) marker = new AIS_Point(new Geom_CartesianPoint(point));
                // Big enough to find: at OCCT's default scale a point is a
                // speck a user cannot tell from a grid crossing.
                marker->SetMarker(Aspect_TOM_BALL);
                marker->SetColor(edge);
                marker->Attributes()->PointAspect()->SetScale(3.0);
                show(marker, 0);
            }
        } catch (const Standard_Failure&) {
            // A degenerate plane or a zero-length axis draws nothing
            // rather than taking the redisplay down with it.
        }
    }
}

void MainWindow::applySelectionFilters()
{
    Handle(AIS_InteractiveContext) context = m_viewport->Context();
    if (context.IsNull()) {
        return;
    }

    m_selectionFilterGeneration = lcad::GeometrySelection::Instance().FilterGeneration();

    for (const Handle(AIS_InteractiveObject)& object : m_displayedBodies) {
        if (object.IsNull()) {
            continue;
        }
        context->Deactivate(object);
        for (const lcad::EntityType type : lcad::GeometrySelection::Instance().PickableTypes()) {
            context->Activate(object, AIS_Shape::SelectionMode(lcad::TopAbsTypeOf(type)),
                              Standard_False);
        }
    }
}

void MainWindow::readViewportSelection()
{
    Handle(AIS_InteractiveContext) context = m_viewport->Context();
    if (context.IsNull()) {
        return;
    }

    std::vector<lcad::GeometryRef> picked;
    for (context->InitSelected(); context->MoreSelected(); context->NextSelected()) {
        // A sub-shape pick arrives as a BRep owner carrying the shape;
        // asked for through the owner rather than the context's own
        // DetectedShape(), which OCCT 7.9 deprecates with local context.
        const Handle(StdSelect_BRepOwner) owner =
            Handle(StdSelect_BRepOwner)::DownCast(context->SelectedOwner());
        if (owner.IsNull() || !owner->HasShape()) {
            continue;
        }
        lcad::GeometryRef ref;
        if (lcad::MakeGeometryRefIn(m_document, owner->Shape(), ref)) {
            picked.push_back(ref);
        }
    }

    // Say what was picked and how big it is, the way Fusion answers a
    // click on a face or an edge. Cleared when nothing is selected rather
    // than left claiming the last thing.
    const std::string description = lcad::DescribeSelection(picked);
    if (description.empty()) {
        statusBar()->clearMessage();
    } else {
        statusBar()->showMessage(QString::fromStdString(description));
    }

    lcad::GeometrySelection::Instance().Set(std::move(picked));
    refreshCommandStates();
}

void MainWindow::onOpenStep()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Open STEP File", QString(), "STEP Files (*.step *.stp);;All Files (*)");
    if (path.isEmpty()) {
        return;
    }

    const TopoDS_Shape shape = ImportStepFile(path.toStdString());
    if (shape.IsNull()) {
        QMessageBox::warning(this, "Import Failed", "Could not read STEP file:\n" + path);
        return;
    }

    // Imports join the timeline like any other feature.
    auto feature = std::make_shared<lcad::ShapeFeature>(shape, "Import");
    m_document.AddFeature(feature);
    // Fit All as the View command does it -- framing the part. The
    // viewport's own FitAll frames the 360 mm origin axes too, which left
    // an imported part a speck in the middle of the view.
    RunCommandById("view.fit_all");
    statusBar()->showMessage("Loaded " + path);
}

lcad::MarkingMenu::ItemList MainWindow::markingMenuItems()
{
    using lcad::MarkingMenu;
    const CommandContext context = makeContext();

    const auto commandItem = [this, &context](const char* theId, const QString& theLabel) {
        MarkingMenu::Item item;
        item.label = theLabel;
        Command* command = CommandRegistry::Instance().Find(theId);
        item.enabled = command != nullptr && command->IsEnabled(context);
        item.tooltip = command != nullptr ? QString::fromStdString(command->Description()) : QString();
        item.action = [this, command]() { runCommand(command); };
        return item;
    };
    // Shown where Fusion shows them, greyed, so the hand learns the right
    // place now and the wedge lights up the day the command exists.
    const auto notYet = [](const QString& theLabel) {
        MarkingMenu::Item item;
        item.label = theLabel;
        item.enabled = false;
        item.tooltip = theLabel + " is not available yet";
        return item;
    };
    // Cancel and OK go to whichever tool is running when they are CHOSEN,
    // looked up then rather than now: the menu is open in between.
    const auto toolKey = [this](int theKey) {
        return [this, theKey]() {
            if (lcad::ViewportInteraction* tool = m_viewport->ExclusiveInteraction()) {
                tool->OnKeyPress(theKey, Qt::NoModifier);
                makeContext().Redraw();
                refreshCommandStates();
            }
        };
    };

    // Clockwise from the top, in Fusion's order: Repeat, Press Pull, Redo,
    // Hole, Sketch, Move/Copy, Undo, Delete.
    MarkingMenu::ItemList items;

    if (m_lastCommand != nullptr) {
        items[0].label = "Repeat " + QString::fromStdString(m_lastCommand->Title());
        items[0].enabled = m_lastCommand->IsEnabled(context);
        Command* last = m_lastCommand;
        items[0].action = [this, last]() { runCommand(last); };
    } else {
        items[0] = notYet("Repeat");
        items[0].tooltip = "Nothing to repeat yet";
    }
    items[1] = commandItem("modify.press_pull", "Press Pull");
    items[3] = notYet("Hole");
    items[4] = commandItem("sketch.create", "Sketch");
    items[5] = commandItem("gizmo.move", "Move/Copy");
    items[7] = notYet("Delete");

    if (m_viewport->ExclusiveInteraction() != nullptr) {
        items[6].label = "Cancel";
        items[6].action = toolKey(Qt::Key_Escape);
        items[2].label = "OK";
        items[2].action = toolKey(Qt::Key_Return);
    } else {
        items[6].label = "Undo";
        items[6].enabled = m_document.CanUndo();
        items[6].action = [this]() { onUndo(); };
        items[2].label = "Redo";
        items[2].enabled = m_document.CanRedo();
        items[2].action = [this]() { onRedo(); };
    }
    return items;
}

void MainWindow::onExport()
{
    const QString status = lcad::ExportDesign(this, m_document);
    if (!status.isEmpty()) {
        statusBar()->showMessage(status);
    }
}

void MainWindow::onUndo()
{
    m_document.Undo();
}

void MainWindow::onRedo()
{
    m_document.Redo();
}

// ---- the design's own file ----

namespace {

constexpr int kMaxRecentFiles = 10;
const char* const kRecentFilesKey = "recentFiles";

QSettings Settings()
{
    return QSettings(QStringLiteral("PenguinCAD"), QStringLiteral("PenguinCAD"));
}

QString DesignNameOf(const QString& theFile)
{
    return theFile.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(theFile).completeBaseName();
}

} // namespace

QByteArray MainWindow::currentDesignText() const
{
    lcad::DesignExtras extras;
    extras.units = lcad::DefaultLengthUnit();
    QByteArray text;
    std::string error;
    if (!lcad::WriteNativeText(m_document, extras, text, error)) {
        // A design this version cannot write is never "saved": an empty
        // text differs from anything a real save recorded.
        return QByteArray();
    }
    return text;
}

void MainWindow::markSaved()
{
    m_savedText = currentDesignText();
    m_document.SetModified(false);
    setWindowModified(false);
}

void MainWindow::refreshModified()
{
    const bool modified = m_savedText.isEmpty() || currentDesignText() != m_savedText;
    m_document.SetModified(modified);
    setWindowModified(modified);
}

void MainWindow::refreshTitle()
{
    // Qt's [*] turns into an asterisk while isWindowModified(), the way a
    // desktop title shows unsaved work.
    setWindowTitle(QStringLiteral("%1[*] — PenguinCAD").arg(DesignNameOf(m_designFile)));
}

void MainWindow::closeEvent(QCloseEvent* theEvent)
{
    if (maybeSaveChanges()) {
        theEvent->accept();
    } else {
        theEvent->ignore();
    }
}

bool MainWindow::maybeSaveChanges()
{
    refreshModified();
    if (!isWindowModified()) {
        return true;
    }
    QMessageBox box(QMessageBox::Warning, QStringLiteral("PenguinCAD"),
                    QStringLiteral("Save changes to \"%1\"?").arg(DesignNameOf(m_designFile)),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setInformativeText(QStringLiteral("Your changes will be lost if you don't save them."));
    box.button(QMessageBox::Discard)->setText(QStringLiteral("Don't Save"));
    box.setDefaultButton(QMessageBox::Save);
    switch (box.exec()) {
        case QMessageBox::Save:    return saveDesign();
        case QMessageBox::Discard: return true;
        default:                   return false;
    }
}

void MainWindow::leaveEditingModes()
{
    // Escape until no tool holds the mouse: some take two (the first drops
    // the curve being drawn, the second the tool itself).
    for (int i = 0; i < 4 && m_viewport->ExclusiveInteraction() != nullptr; ++i) {
        m_viewport->ExclusiveInteraction()->OnKeyPress(Qt::Key_Escape, Qt::NoModifier);
    }
    CommandContext context = makeContext();
    Command* finish = CommandRegistry::Instance().Find("sketch.finish");
    if (finish != nullptr && finish->IsEnabled(context)) {
        finish->Execute(context);
    }
    lcad::GeometrySelection::Instance().Clear();
    lcad::ProfileSelection::Instance().Clear();
    Handle(AIS_InteractiveContext) ais = m_viewport->Context();
    if (!ais.IsNull()) {
        ais->ClearSelected(Standard_False);
    }
}

void MainWindow::onNewDesign()
{
    if (!maybeSaveChanges()) {
        return;
    }
    leaveEditingModes();
    std::string error;
    m_document.ReplaceDesign(lcad::Document::DesignState(), error);   // an empty design: cannot fail
    lcad::SetDefaultLengthUnit(lcad::LengthUnit::Millimeter);
    m_designFile.clear();
    m_document.SetName(DesignNameOf(m_designFile).toStdString());
    markSaved();
    refreshTitle();
    RunCommandById("view.isometric");
    statusBar()->showMessage("New design");
}

void MainWindow::onOpenDesign()
{
    const QString start = m_designFile.isEmpty() ? QDir::homePath() : QFileInfo(m_designFile).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, "Open", start, "PenguinCAD Designs (*.pcad);;All Files (*)");
    if (!path.isEmpty()) {
        OpenDesignFile(path);
    }
}

bool MainWindow::OpenDesignFile(const QString& thePath, QString* theError)
{
    const QString name = QFileInfo(thePath).fileName();
    const auto fail = [this, theError, &name](const std::string& theWhy) {
        const QString message = "Could not open " + name + ":\n" + QString::fromStdString(theWhy);
        if (theError != nullptr) {
            *theError = message;
        } else {
            QMessageBox::warning(this, "Open Failed", message);
        }
        return false;
    };

    // Read and check the whole file BEFORE asking about unsaved changes: a
    // file that turns out to be broken should say so, not first make the
    // user decide the fate of the design they have open.
    QFile file(thePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(file.errorString().toStdString());
    }
    const QByteArray text = file.readAll();
    lcad::Document::DesignState design;
    lcad::DesignExtras extras;
    std::string error;
    if (!lcad::ReadNativeText(text, design, extras, error)) {
        return fail(error);
    }

    if (!maybeSaveChanges()) {
        if (theError != nullptr) {
            *theError = "cancelled";
        }
        return false;
    }
    leaveEditingModes();
    if (!m_document.ReplaceDesign(std::move(design), error)) {
        return fail(error);
    }

    // The unit the design was made in, not the one the last design left.
    lcad::SetDefaultLengthUnit(extras.units);
    m_designFile = QFileInfo(thePath).absoluteFilePath();
    m_document.SetName(DesignNameOf(m_designFile).toStdString());
    markSaved();
    refreshTitle();
    rememberRecentFile(m_designFile);
    RunCommandById("view.fit_all");

    // A feature that failed to rebuild is part of the design as saved, and
    // the timeline shows it red; the status bar says how many.
    const std::size_t failed = m_document.Errors().size();
    statusBar()->showMessage(failed == 0 ? "Opened " + m_designFile
                                         : QString("Opened %1 -- %2 feature(s) failed to rebuild: %3")
                                               .arg(m_designFile)
                                               .arg(static_cast<int>(failed))
                                               .arg(QString::fromStdString(m_document.Errors().front())));
    return true;
}

bool MainWindow::saveDesign()
{
    if (m_designFile.isEmpty()) {
        return saveDesignAs();
    }
    return SaveDesignFile(m_designFile);
}

bool MainWindow::saveDesignAs()
{
    const QString start = m_designFile.isEmpty()
                              ? QDir::home().filePath(DesignNameOf(m_designFile) + ".pcad")
                              : m_designFile;
    QString path = QFileDialog::getSaveFileName(this, "Save As", start,
                                                "PenguinCAD Designs (*.pcad)");
    if (path.isEmpty()) {
        return false;
    }
    // The desktop's file chooser does not add the extension, and a design
    // saved as "bracket" would not show up in the next Open dialog. Adding
    // it after the chooser means the chooser never asked about replacing
    // THAT name, so ask here.
    if (QFileInfo(path).suffix().compare(lcad::kNativeFileSuffix, Qt::CaseInsensitive) != 0) {
        path += '.';
        path += lcad::kNativeFileSuffix;
        if (QFileInfo::exists(path)
            && QMessageBox::question(this, "Save As",
                                     QFileInfo(path).fileName()
                                         + " already exists.\nDo you want to replace it?",
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                   != QMessageBox::Yes) {
            return false;
        }
    }
    return SaveDesignFile(path);
}

bool MainWindow::SaveDesignFile(const QString& thePath, QString* theError)
{
    lcad::DesignExtras extras;
    extras.units = lcad::DefaultLengthUnit();
    std::string error;
    if (!lcad::SaveNativeFile(thePath, m_document, extras, error)) {
        const QString message =
            "Could not save " + QFileInfo(thePath).fileName() + ":\n" + QString::fromStdString(error);
        if (theError != nullptr) {
            *theError = message;
        } else {
            QMessageBox::warning(this, "Save Failed", message);
        }
        return false;
    }
    m_designFile = QFileInfo(thePath).absoluteFilePath();
    // Named before markSaved: SetName notifies, and the observer compares
    // against the saved text.
    m_document.SetName(DesignNameOf(m_designFile).toStdString());
    markSaved();
    refreshTitle();
    rememberRecentFile(m_designFile);
    statusBar()->showMessage("Saved " + m_designFile);
    return true;
}

void MainWindow::rememberRecentFile(const QString& thePath)
{
    QSettings settings = Settings();
    QStringList files = settings.value(kRecentFilesKey).toStringList();
    files.removeAll(thePath);
    files.prepend(thePath);
    while (files.size() > kMaxRecentFiles) {
        files.removeLast();
    }
    settings.setValue(kRecentFilesKey, files);
    // Queued: this runs inside the triggered() of an Open Recent entry,
    // and rebuilding the menu now would delete that action mid-signal.
    QMetaObject::invokeMethod(this, &MainWindow::rebuildRecentMenu, Qt::QueuedConnection);
}

void MainWindow::rebuildRecentMenu()
{
    if (m_recentMenu == nullptr) {
        return;
    }
    m_recentMenu->clear();
    const QStringList files = Settings().value(kRecentFilesKey).toStringList();
    for (const QString& path : files) {
        // "&" doubled, or "R&D.pcad" would underline the D and lose the &.
        QAction* action = m_recentMenu->addAction(QFileInfo(path).fileName().replace('&', "&&"));
        action->setToolTip(path);
        action->setStatusTip(path);
        // Kept in the list but greyed: a file on a drive that is not
        // plugged in right now is still a file the user will want back.
        action->setEnabled(QFileInfo::exists(path));
        connect(action, &QAction::triggered, this, [this, path]() { OpenDesignFile(path); });
    }
    if (files.isEmpty()) {
        m_recentMenu->addAction("No Recent Designs")->setEnabled(false);
        return;
    }
    m_recentMenu->addSeparator();
    connect(m_recentMenu->addAction("Clear Recent"), &QAction::triggered, this, [this]() {
        Settings().remove(kRecentFilesKey);
        QMetaObject::invokeMethod(this, &MainWindow::rebuildRecentMenu, Qt::QueuedConnection);
    });
}
