#pragma once

#include <TopoDS_Shape.hxx>
#include <string>

// Triangulates and writes the given shape to an ASCII STL file.
// Returns true on success.
bool ExportShapeToStl(const TopoDS_Shape& shape, const std::string& path);
