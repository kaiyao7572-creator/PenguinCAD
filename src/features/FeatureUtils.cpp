#include "features/FeatureUtils.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRep_Builder.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopTools_ListOfShape.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace lcad {

namespace {

std::string ToLower(const std::string& theText)
{
    std::string lower = theText;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char theChar) {
        return static_cast<char>(std::tolower(theChar));
    });
    return lower;
}

std::string OcctMessage(const Standard_Failure& theFailure, const char* theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message)
                                                      : std::string(theFallback);
}

} // namespace

const std::vector<std::string>& BooleanOpNames()
{
    static const std::vector<std::string> names = {"New Body", "Join", "Cut", "Intersect"};
    return names;
}

std::string BooleanOpName(BooleanOp theOperation)
{
    const std::vector<std::string>& names = BooleanOpNames();
    const std::size_t index = static_cast<std::size_t>(theOperation);
    return index < names.size() ? names[index] : names.front();
}

BooleanOp BooleanOpFromInt(int theValue)
{
    if (theValue < 0 || theValue > static_cast<int>(BooleanOp::Intersect)) {
        return BooleanOp::NewBody;
    }
    return static_cast<BooleanOp>(theValue);
}

bool ParseBooleanOp(const std::string& theText, BooleanOp& theOperation)
{
    const std::string wanted = ToLower(theText);
    if (wanted.empty()) {
        return false;
    }

    const std::vector<std::string>& names = BooleanOpNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::string candidate = ToLower(names[i]);
        if (candidate == wanted) {
            theOperation = BooleanOpFromInt(static_cast<int>(i));
            return true;
        }
        // "newbody" and "new body" should both work; users retype these
        // by hand in the properties panel.
        candidate.erase(std::remove(candidate.begin(), candidate.end(), ' '), candidate.end());
        if (candidate == wanted) {
            theOperation = BooleanOpFromInt(static_cast<int>(i));
            return true;
        }
    }

    if (wanted.size() == 1 && wanted[0] >= '0' && wanted[0] <= '3') {
        theOperation = BooleanOpFromInt(wanted[0] - '0');
        return true;
    }
    return false;
}

bool IsEmptyShape(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return true;
    }
    if (theShape.ShapeType() == TopAbs_COMPOUND || theShape.ShapeType() == TopAbs_COMPSOLID) {
        TopoDS_Iterator iterator(theShape);
        return !iterator.More();
    }
    return false;
}

bool HasSolid(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return false;
    }
    TopExp_Explorer explorer(theShape, TopAbs_SOLID);
    return explorer.More() == Standard_True;
}

std::vector<TopoDS_Shape> CollectSolids(const TopoDS_Shape& theShape)
{
    std::vector<TopoDS_Shape> solids;
    if (theShape.IsNull()) {
        return solids;
    }
    for (TopExp_Explorer explorer(theShape, TopAbs_SOLID); explorer.More(); explorer.Next()) {
        solids.push_back(explorer.Current());
    }
    return solids;
}

TopoDS_Shape MakeCompoundOf(const std::vector<TopoDS_Shape>& theShapes)
{
    if (theShapes.empty()) {
        return TopoDS_Shape();
    }
    if (theShapes.size() == 1) {
        return theShapes.front();
    }

    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    for (const TopoDS_Shape& shape : theShapes) {
        if (!shape.IsNull()) {
            builder.Add(compound, shape);
        }
    }
    return compound;
}

bool FuseProfileSolids(const std::vector<TopoDS_Shape>& theSolids,
                       TopoDS_Shape&                    theResult,
                       std::string&                     theError)
{
    if (theSolids.size() < 2) {
        theResult = MakeCompoundOf(theSolids);
        return true;
    }
    try {
        TopTools_ListOfShape arguments;
        TopTools_ListOfShape tools;
        arguments.Append(theSolids.front());
        for (std::size_t i = 1; i < theSolids.size(); ++i) {
            tools.Append(theSolids[i]);
        }
        BRepAlgoAPI_Fuse fuse;
        fuse.SetArguments(arguments);
        fuse.SetTools(tools);
        fuse.Build();
        if (!fuse.IsDone()) {
            theError = "the profiles could not be joined into one body";
            return false;
        }
        // The fuse leaves the wall between two regions behind as a seam
        // splitting what is one flat face into two; Fusion shows one face,
        // and a user picking it for press/pull expects the whole of it.
        ShapeUpgrade_UnifySameDomain unify(fuse.Shape(), Standard_True, Standard_True,
                                           Standard_False);
        unify.Build();
        theResult = unify.Shape();
    } catch (const Standard_Failure& failure) {
        const Standard_CString message = failure.GetMessageString();
        theError = (message != nullptr && message[0] != '\0')
                       ? std::string(message)
                       : std::string("the profiles could not be joined into one body");
        return false;
    }
    return true;
}

bool ApplyBooleanOperation(BooleanOp           theOperation,
                           const TopoDS_Shape& theBase,
                           const TopoDS_Shape& theTool,
                           TopoDS_Shape&       theResult,
                           std::string&        theError)
{
    if (IsEmptyShape(theTool)) {
        theError = "no geometry to combine";
        return false;
    }

    const bool hasBase = !IsEmptyShape(theBase);

    // With nothing upstream there is nothing to modify: a new body is the
    // only sensible reading of any operation, except the two that are
    // meaningless without a base and are better reported than silently
    // downgraded.
    if (!hasBase) {
        if (theOperation == BooleanOp::Cut) {
            theError = "nothing to cut from -- create a body first";
            return false;
        }
        if (theOperation == BooleanOp::Intersect) {
            theError = "nothing to intersect with -- create a body first";
            return false;
        }
        theResult = theTool;
        return true;
    }

    if (theOperation == BooleanOp::NewBody) {
        // Separate bodies rather than one welded lump, matching the way
        // ShapeFeature stacks imported shapes.
        theResult = MakeCompoundOf({theBase, theTool});
        return true;
    }

    try {
        TopoDS_Shape result;
        switch (theOperation) {
            case BooleanOp::Join: {
                BRepAlgoAPI_Fuse fuse(theBase, theTool);
                fuse.Build();
                if (!fuse.IsDone() || fuse.HasErrors()) {
                    theError = "join failed -- the bodies may not overlap or touch cleanly";
                    return false;
                }
                result = fuse.Shape();
                break;
            }
            case BooleanOp::Cut: {
                BRepAlgoAPI_Cut cut(theBase, theTool);
                cut.Build();
                if (!cut.IsDone() || cut.HasErrors()) {
                    theError = "cut failed -- check that the tool body overlaps the target";
                    return false;
                }
                result = cut.Shape();
                break;
            }
            case BooleanOp::Intersect: {
                BRepAlgoAPI_Common common(theBase, theTool);
                common.Build();
                if (!common.IsDone() || common.HasErrors()) {
                    theError = "intersect failed -- check that the bodies overlap";
                    return false;
                }
                result = common.Shape();
                break;
            }
            case BooleanOp::NewBody:
                break;  // handled above
        }

        if (IsEmptyShape(result)) {
            theError = BooleanOpName(theOperation) + " left nothing behind";
            return false;
        }

        theResult = result;
        return true;
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "boolean operation failed");
        return false;
    }
}

std::string FormatNumber(double theValue)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4g", theValue);
    return std::string(buffer);
}

} // namespace lcad
