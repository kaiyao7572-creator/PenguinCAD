#include "core/ShapeFeature.h"

#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>

namespace lcad {

bool ShapeFeature::Compute(const ComputeContext& theContext,
                           const TopoDS_Shape&   theInput,
                           TopoDS_Shape&         theOutput,
                           std::string&          theError)
{
    (void)theContext;

    if (myShape.IsNull()) {
        theError = "no shape";
        return false;
    }

    if (theInput.IsNull()) {
        theOutput = myShape;
        return true;
    }

    // Keep both the upstream model and this shape around as separate
    // bodies rather than booleaning them together.
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    builder.Add(compound, theInput);
    builder.Add(compound, myShape);
    theOutput = compound;
    return true;
}

std::unique_ptr<Feature> ShapeFeature::Clone() const
{
    auto copy = std::make_unique<ShapeFeature>(myShape, myTypeName);
    CopyBaseTo(*copy);
    return copy;
}

} // namespace lcad
