#pragma once

#include "core/Feature.h"

namespace lcad {

// A feature that contributes a fixed, already-built shape: STEP imports,
// primitives built by a dialog, anything with no parametric recipe behind
// it. Fuses with whatever came before it so multiple bodies coexist.
class ShapeFeature : public Feature
{
public:
    ShapeFeature() = default;
    explicit ShapeFeature(const TopoDS_Shape& theShape, std::string theTypeName = "Body")
        : myShape(theShape), myTypeName(std::move(theTypeName))
    {
    }

    std::string TypeName() const override { return myTypeName; }

    const TopoDS_Shape& Shape() const { return myShape; }
    void SetShape(const TopoDS_Shape& theShape) { myShape = theShape; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

private:
    TopoDS_Shape myShape;
    std::string  myTypeName = "Body";
};

} // namespace lcad
