#pragma once

#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace lcad {

// Writes theShape to a Wavefront .obj file as a triangle mesh, one OBJ
// group per body.
//
// theBodyNames follows the same contract as ExportShapeToStep: names in
// SplitIntoBodies() order, which is Document::Bodies() order, short lists
// filled in with "Body<n>".
//
// OBJ is a mesh format, so this is a one-way door -- curved faces leave as
// facets at the same deflection src/StlExport.cpp uses. Bodies with no
// faces (a loose wire) carry no mesh and are named in a comment rather
// than dropped without trace.
//
// Returns false with a readable theError on any failure. Never throws.
bool ExportShapeToObj(const TopoDS_Shape&             theShape,
                      const std::vector<std::string>& theBodyNames,
                      const std::string&              thePath,
                      std::string&                    theError);

} // namespace lcad
