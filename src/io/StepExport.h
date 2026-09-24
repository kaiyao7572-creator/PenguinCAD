#pragma once

#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace lcad {

// Writes theShape to a STEP file: AP214, millimetres, one STEP product
// per body.
//
// theBodyNames names the bodies in the order SplitIntoBodies() reports
// them, which is the order Document::Bodies() is in -- so a caller can
// hand over the names the user actually sees in the browser. A short list
// (or an empty one) is fine: the rest fall back to "Body<n>".
//
// Returns false with a readable theError on any failure, including a
// translation that only half-succeeded. Never throws.
bool ExportShapeToStep(const TopoDS_Shape&             theShape,
                       const std::vector<std::string>& theBodyNames,
                       const std::string&              thePath,
                       std::string&                    theError);

} // namespace lcad
