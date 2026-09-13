#pragma once

#include <TopoDS_Shape.hxx>
#include <string>

// Reads a STEP file and returns the combined shape. Returns a null
// TopoDS_Shape (IsNull() == true) on failure.
TopoDS_Shape ImportStepFile(const std::string& path);
