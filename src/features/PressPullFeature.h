#pragma once

#include "core/Feature.h"
#include "core/GeometryRef.h"

#include <string>
#include <vector>

namespace lcad {

// Fusion's Press/Pull (Q), the tool everything else in Modify sits
// downstream of in muscle memory: pick faces and move them.
//
// Every picked face slides along its own outward normal by the same
// signed distance, which is one rule rather than three: a planar face
// pushed OUT adds material and pushed IN removes it; a cylindrical face's
// RADIUS changes, so the wall of a boss grows and the wall of a hole --
// whose outward normal faces the axis -- closes in; and the neighbouring
// faces are EXTENDED to meet whatever moved, so a draft stays a draft
// instead of gaining a step.
//
// One signed distance covers all of that because "out" always means the
// way the face itself looks, which is what makes several faces of
// different orientations sensible in a single feature.
//
// The faces are held as GeometryRefs rather than indices, so the feature
// survives an upstream edit that reorders the body's faces; when a face
// it named is genuinely gone it fails loudly rather than moving a
// different one.
class PressPullFeature : public Feature
{
public:
    PressPullFeature() = default;
    PressPullFeature(GeometryRef theFace, double theDistance);
    PressPullFeature(std::vector<GeometryRef> theFaces, double theDistance);

    std::string TypeName() const override { return "PressPull"; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    const std::vector<GeometryRef>& Faces() const { return myFaces; }
    void SetFaces(std::vector<GeometryRef> theFaces);

    // The first picked face, or a null ref when nothing is picked. Most
    // press/pulls are one face and reading that back should not cost the
    // caller a size check.
    const GeometryRef& Face() const;
    void SetFace(GeometryRef theFace);

    double Distance() const { return myDistance; }
    void SetDistance(double theDistance) { myDistance = theDistance; }

private:
    // Never holds a null ref: an empty list is how "nothing picked" is
    // spelled, so every consumer has one case to test instead of two.
    std::vector<GeometryRef> myFaces;
    double                   myDistance = 10.0;
};

} // namespace lcad
