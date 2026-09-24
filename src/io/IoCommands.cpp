#include "core/Command.h"
#include "core/Document.h"
#include "io/ObjExport.h"
#include "io/StepExport.h"

#include <TopoDS_Shape.hxx>

#include <memory>
#include <string>
#include <vector>

#include <QFileDialog>
#include <QMainWindow>
#include <QMessageBox>
#include <QStatusBar>
#include <QString>
#include <QStringList>

namespace lcad {

namespace {

// Fusion keeps the make-and-hand-off tools in their own tab rather than
// among the modelling ones, and so does this: nothing here changes the
// model, which is exactly why it does not belong beside Extrude.
const char* const kUtilitiesGroup = "Utilities";
const char* const kExportSection  = "Export";

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

// The names the user sees in the browser, in the order the exporters
// split the document shape back apart -- Document::Bodies() is rebuilt
// from SplitIntoBodies(), so the two lists line up entry for entry.
std::vector<std::string> BodyNames(const Document& theDocument)
{
    std::vector<std::string> names;
    names.reserve(theDocument.Bodies().size());
    for (const BodyPtr& body : theDocument.Bodies()) {
        names.push_back(body ? body->Name() : std::string());
    }
    return names;
}

bool HasShape(const CommandContext& theContext)
{
    return theContext.document != nullptr && !theContext.document->Shape().IsNull();
}

// Both writers take the same arguments, so the dialog, the suffix and the
// error reporting are written once. MainWindow::onExportStl is the idiom
// being followed here; it stays where it is and this does not touch it.
using ExportWriter = bool (*)(const TopoDS_Shape&,
                              const std::vector<std::string>&,
                              const std::string&,
                              std::string&);

void RunExport(CommandContext&    theContext,
               const QString&     theTitle,
               const QString&     theFilter,
               const QStringList& theSuffixes,
               ExportWriter       theWriter)
{
    if (!HasShape(theContext)) {
        QMessageBox::information(theContext.parent, "Nothing to Export",
                                 "Create or load a shape first.");
        return;
    }

    QString path = QFileDialog::getSaveFileName(theContext.parent, theTitle, QString(), theFilter);
    if (path.isEmpty()) {
        return;
    }

    // Qt's own save dialog does not append the filter's extension, and a
    // STEP file called "bracket" is one another tool will not open.
    bool named = false;
    for (const QString& suffix : theSuffixes) {
        named = named || path.endsWith(suffix, Qt::CaseInsensitive);
    }
    if (!named) {
        path += theSuffixes.front();
    }

    std::string error;
    if (!theWriter(theContext.document->Shape(), BodyNames(*theContext.document),
                   path.toStdString(), error)) {
        QMessageBox::warning(theContext.parent, "Export Failed",
                             "Could not write " + path + ":\n"
                                 + QString::fromStdString(error));
        return;
    }

    ShowStatus(theContext, "Exported " + path);
}

// STEP is how the model leaves as a model: real B-rep solids another CAD
// tool can keep working on, which is the half of the round trip
// src/StepImport.cpp already had on its own.
class ExportStepCommand : public Command
{
public:
    std::string Id() const override { return "export.step"; }
    std::string Title() const override { return "Export STEP"; }
    std::string Group() const override { return kUtilitiesGroup; }
    std::string Section() const override { return kExportSection; }
    std::string Icon() const override { return "📤"; }
    std::string Description() const override
    {
        return "Write the design as a STEP file another CAD tool can open";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return HasShape(theContext);
    }

    void Execute(CommandContext& theContext) override
    {
        RunExport(theContext, "Export STEP File", "STEP Files (*.step *.stp)",
                  QStringList{".step", ".stp"}, &ExportShapeToStep);
    }
};

// OBJ is how it leaves as a picture: triangles for a renderer or a game
// engine, with the bodies kept apart as groups so they can still be
// textured and moved one at a time.
class ExportObjCommand : public Command
{
public:
    std::string Id() const override { return "export.obj"; }
    std::string Title() const override { return "Export OBJ"; }
    std::string Group() const override { return kUtilitiesGroup; }
    std::string Section() const override { return kExportSection; }
    std::string Icon() const override { return "🌐"; }  // meridians: a faceted mesh
    std::string Description() const override
    {
        return "Write the design as a Wavefront OBJ mesh, one group per body";
    }

    bool IsEnabled(const CommandContext& theContext) const override
    {
        return HasShape(theContext);
    }

    void Execute(CommandContext& theContext) override
    {
        RunExport(theContext, "Export OBJ File", "Wavefront OBJ Files (*.obj)",
                  QStringList{".obj"}, &ExportShapeToObj);
    }
};

} // namespace

// Declared in core/Registration.h alongside the other subsystems'
// registration hooks; MainWindow calls it once at startup.
void RegisterIoCommands(CommandRegistry& theRegistry)
{
    theRegistry.Add(std::make_unique<ExportStepCommand>());
    theRegistry.Add(std::make_unique<ExportObjCommand>());
}

} // namespace lcad
