#include "MainWindow.h"

#include "OcctViewport.h"
#include "StepImport.h"
#include "StlExport.h"
#include "core/Registration.h"
#include "core/ShapeFeature.h"

#include <AIS_Shape.hxx>

#include <QAction>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QScrollArea>
#include <QStatusBar>
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

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    m_viewport = new OcctViewport(this);

    // Every subsystem contributes its tools here. Each of these lives in
    // its own directory and knows nothing about this file.
    CommandRegistry& registry = CommandRegistry::Instance();
    lcad::RegisterSketchCommands(registry);
    lcad::RegisterFeatureCommands(registry);
    lcad::RegisterGizmoCommands(registry);
    lcad::RegisterViewCommands(registry);
    lcad::RegisterInspectCommands(registry);

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

    resize(1400, 900);
    setWindowTitle("linuxCAD");
    statusBar()->showMessage("Ready");
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
                button->setFixedWidth(96);
                button->setMinimumHeight(74);
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
                    const QString icon = QString::fromStdString(primary->Icon());
                    const QString label = QString::fromStdString(family.label);
                    button->setText(icon.isEmpty() ? label : (icon + "\n" + label));
                    button->setToolTip(QString::fromStdString(primary->Description()));
                    connect(button, &QToolButton::clicked, this,
                            [this, primary]() { runCommand(primary); });

                    m_familyButtons.emplace_back(button, primary);
                }

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

        m_ribbon->addTab(scroll, QString::fromStdString(group));
    }
}

namespace {

// Ribbon buttons are narrow, and Qt elides a too-long label into things
// like "2-...le". Wrapping on word boundaries instead keeps every tool
// readable at the same button width.
QString WrapButtonLabel(const QString& theTitle, int theMaxChars)
{
    QString spaced = theTitle;
    spaced.replace('/', " /");          // let "Move/Rotate" break after the slash
    const QStringList words = spaced.split(' ', Qt::SkipEmptyParts);
    QStringList lines;
    QString current;
    for (const QString& word : words) {
        if (current.isEmpty()) {
            current = word;
        } else if (current.length() + 1 + word.length() <= theMaxChars) {
            current += ' ' + word;
        } else {
            lines << current;
            current = word;
        }
    }
    if (!current.isEmpty()) {
        lines << current;
    }
    // Keep it to two lines; anything longer would grow every button.
    while (lines.size() > 2) {
        const QString tail = lines.takeLast();
        lines.last() += ' ' + tail;
    }
    return lines.join('\n');
}

} // namespace

QAction* MainWindow::makeCommandAction(lcad::Command* theCommand)
{
    const QString title = WrapButtonLabel(QString::fromStdString(theCommand->Title()), 9);
    const QString icon = QString::fromStdString(theCommand->Icon());

    QAction* action = new QAction(icon.isEmpty() ? title : (icon + "\n" + title), this);
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

    QAction* openAction = fileMenu->addAction("&Open STEP...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenStep);

    QAction* exportAction = fileMenu->addAction("&Export STL...");
    connect(exportAction, &QAction::triggered, this, &MainWindow::onExportStl);

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
    CommandContext context = makeContext();
    theCommand->Execute(context);
    refreshCommandStates();
}

void MainWindow::refreshCommandStates()
{
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

    if (!m_displayedShape.IsNull()) {
        context->Remove(m_displayedShape, Standard_False);
        m_displayedShape.Nullify();
    }

    const TopoDS_Shape& shape = m_document.Shape();
    if (!shape.IsNull()) {
        Handle(AIS_Shape) aisShape = new AIS_Shape(shape);
        context->Display(aisShape, AIS_Shaded, 0, Standard_False);
        m_displayedShape = aisShape;
    }

    Handle(V3d_View) view = m_viewport->View();
    if (!view.IsNull()) {
        view->Redraw();
    }
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
    m_viewport->FitAll();
    statusBar()->showMessage("Loaded " + path);
}

void MainWindow::onExportStl()
{
    const TopoDS_Shape& shape = m_document.Shape();
    if (shape.IsNull()) {
        QMessageBox::information(this, "Nothing to Export", "Create or load a shape first.");
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, "Export STL File", QString(), "STL Files (*.stl)");
    if (path.isEmpty()) {
        return;
    }

    if (!ExportShapeToStl(shape, path.toStdString())) {
        QMessageBox::warning(this, "Export Failed", "Could not write STL file:\n" + path);
        return;
    }

    statusBar()->showMessage("Exported " + path);
}

void MainWindow::onUndo()
{
    m_document.Undo();
}

void MainWindow::onRedo()
{
    m_document.Redo();
}
