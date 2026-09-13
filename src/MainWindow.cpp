#include "MainWindow.h"
#include "OcctViewport.h"
#include "StepImport.h"
#include "StlExport.h"

#include <AIS_Shape.hxx>

#include <QAction>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    m_viewport = new OcctViewport(this);
    setCentralWidget(m_viewport);

    buildMenus();

    resize(1200, 800);
    setWindowTitle("linuxCAD");
    statusBar()->showMessage("Ready");
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

    m_currentShape = shape;
    displayShape(shape);
    statusBar()->showMessage("Loaded " + path);
}

void MainWindow::onExportStl()
{
    if (m_currentShape.IsNull()) {
        QMessageBox::information(this, "Nothing to Export", "Load a shape first.");
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, "Export STL File", QString(), "STL Files (*.stl)");
    if (path.isEmpty()) {
        return;
    }

    if (!ExportShapeToStl(m_currentShape, path.toStdString())) {
        QMessageBox::warning(this, "Export Failed", "Could not write STL file:\n" + path);
        return;
    }

    statusBar()->showMessage("Exported " + path);
}

void MainWindow::displayShape(const TopoDS_Shape& shape)
{
    Handle(AIS_InteractiveContext) context = m_viewport->Context();
    if (context.IsNull()) {
        return;
    }

    context->RemoveAll(Standard_False);

    Handle(AIS_Shape) aisShape = new AIS_Shape(shape);
    context->Display(aisShape, AIS_Shaded, 0, Standard_False);

    m_viewport->FitAll();
}
