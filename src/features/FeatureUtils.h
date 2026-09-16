#pragma once

#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace lcad {

// How a feature's new geometry combines with whatever the timeline built
// before it. Mirrors Fusion's operation dropdown. The integer values are
// part of the stored parameter, so don't renumber them.
enum class BooleanOp
{
    NewBody   = 0,
    Join      = 1,
    Cut       = 2,
    Intersect = 3
};

// Labels in enum order; dialogs and the properties panel share them so a
// value typed into one is understood by the other.
const std::vector<std::string>& BooleanOpNames();
std::string BooleanOpName(BooleanOp theOp);
BooleanOp BooleanOpFromInt(int theValue);

// Accepts a label ("Cut", case-insensitive) or the bare index, which is
// what makes the operation editable as a plain string parameter.
bool ParseBooleanOp(const std::string& theText, BooleanOp& theOperation);

// True when a shape carries no geometry: a null shape, or the empty
// compound a boolean leaves behind when the tool cancels the base out.
bool IsEmptyShape(const TopoDS_Shape& theShape);

// True when the shape contains at least one solid. Modify features gate
// on this -- filleting a bare sketch wire is meaningless.
bool HasSolid(const TopoDS_Shape& theShape);

// Every solid in the shape, so a modify feature can treat a compound of
// bodies one body at a time.
std::vector<TopoDS_Shape> CollectSolids(const TopoDS_Shape& theShape);

// One shape from many: the shape itself when there is only one, a
// compound otherwise, a null shape when the list is empty.
TopoDS_Shape MakeCompoundOf(const std::vector<TopoDS_Shape>& theShapes);

// Combine theTool with theBase according to theOperation. Returns false
// with a readable theError on failure -- including "nothing to cut from"
// when the timeline has no body yet, which is a user mistake rather than
// a kernel failure.
bool ApplyBooleanOperation(BooleanOp           theOperation,
                           const TopoDS_Shape& theBase,
                           const TopoDS_Shape& theTool,
                           TopoDS_Shape&       theResult,
                           std::string&        theError);

// Trim a number for error messages: 3.5 rather than 3.500000.
std::string FormatNumber(double theValue);

} // namespace lcad
