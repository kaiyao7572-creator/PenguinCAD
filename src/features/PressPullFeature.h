#pragma once

#include "core/Feature.h"
#include "core/GeometryRef.h"

#include <string>

namespace lcad {

// Fusion's Press/Pull (Q), the tool everything else in Modify sits
// downstream of in muscle memory: pick a face and move it.
//
// A planar face pushed OUT along its own normal adds material, and pushed
// IN removes it -- which is the whole interaction, and why one distance
// with a sign covers both. The face is held as a GeometryRef rather than
// an index, so the feature survives an upstream edit that reorders the
// body's faces; when the face it named is genuinely gone it fails loudly
// rather than moving a different one.
class PressPullFeature : public Feature
{
public:
    PressPullFeature() = default;
    PressPullFeature(GeometryRef theFace, double theDistance);

    std::string TypeName() const override { return "PressPull"; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    const GeometryRef& Face() const { return myFace; }
    void SetFace(GeometryRef theFace) { myFace = std::move(theFace); }

    double Distance() const { return myDistance; }
    void SetDistance(double theDistance) { myDistance = theDistance; }

private:
    GeometryRef myFace;
    double      myDistance = 10.0;
};

} // namespace lcad
