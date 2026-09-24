#include "io/ExportDialog.h"

#include "StlExport.h"
#include "core/Document.h"
#include "io/ObjExport.h"
#include "io/StepExport.h"

#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

#include <QFileDialog>
#include <QMessageBox>
#include <QStringList>

namespace lcad {

namespace {

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

using ExportWriter = bool (*)(const TopoDS_Shape&,
                              const std::vector<std::string>&,
                              const std::string&,
                              std::string&);

// STL predates the other two and takes no body names: a mesh file has no
// notion of separate solids to name.
bool WriteStl(const TopoDS_Shape& theShape, const std::vector<std::string>&,
              const std::string& thePath, std::string& theError)
{
    if (!ExportShapeToStl(theShape, thePath)) {
        theError = "the mesh could not be written";
        return false;
    }
    return true;
}

struct ExportFormat
{
    QString      filter;     // the dialog's Type entry
    QStringList  suffixes;   // first is appended when the name has none
    ExportWriter writer;
};

const std::vector<ExportFormat>& Formats()
{
    static const std::vector<ExportFormat> formats = {
        {"STEP Files (*.step *.stp)", {".step", ".stp"}, &ExportShapeToStep},
        {"STL Files (*.stl)", {".stl"}, &WriteStl},
        {"OBJ Files (*.obj)", {".obj"}, &ExportShapeToObj},
    };
    return formats;
}

} // namespace

QString ExportDesign(QWidget* theParent, const Document& theDocument)
{
    if (theDocument.Shape().IsNull()) {
        QMessageBox::information(theParent, "Nothing to Export", "Create or load a shape first.");
        return QString();
    }

    QStringList filters;
    for (const ExportFormat& format : Formats()) {
        filters << format.filter;
    }

    // Remembered for the session: whoever exported STL once will usually
    // want STL again, and the dialog should not make them re-pick it.
    static QString theLastFilter = Formats().front().filter;
    QString chosen = theLastFilter;
    QString path = QFileDialog::getSaveFileName(theParent, "Export", QString(),
                                                filters.join(";;"), &chosen);
    if (path.isEmpty()) {
        return QString();
    }

    const ExportFormat* format = nullptr;
    for (const ExportFormat& candidate : Formats()) {
        for (const QString& suffix : candidate.suffixes) {
            if (path.endsWith(suffix, Qt::CaseInsensitive)) {
                format = &candidate;
            }
        }
    }
    if (format == nullptr) {
        // No extension typed: the dropdown decides, and the file gets that
        // format's extension -- Qt's dialog does not add one, and a STEP
        // file called "bracket" is one another tool will not open.
        for (const ExportFormat& candidate : Formats()) {
            if (candidate.filter == chosen) {
                format = &candidate;
            }
        }
        if (format == nullptr) {
            format = &Formats().front();
        }
        path += format->suffixes.front();
    }
    theLastFilter = format->filter;

    std::string error;
    if (!format->writer(theDocument.Shape(), BodyNames(theDocument), path.toStdString(), error)) {
        QMessageBox::warning(theParent, "Export Failed",
                             "Could not write " + path + ":\n" + QString::fromStdString(error));
        return QString();
    }
    return "Exported " + path;
}

} // namespace lcad
