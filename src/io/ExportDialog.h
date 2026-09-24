#pragma once

#include <QString>

class QWidget;

namespace lcad {

class Document;

// File > Export: one save dialog whose type list carries every format this
// app writes -- STEP, STL, OBJ -- the way Fusion's Export dialog has one
// Type dropdown rather than a command per format. The extension typed into
// the name wins over the dropdown when they disagree, since that is what
// the user will look for on disk.
//
// Returns the line to show in the status bar, empty when the user
// cancelled. Failures are reported in a message box before returning.
QString ExportDesign(QWidget* theParent, const Document& theDocument);

} // namespace lcad
