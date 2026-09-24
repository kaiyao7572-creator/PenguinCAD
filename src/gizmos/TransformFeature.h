#pragma once

#include "core/Feature.h"
#include "features/CombineFeature.h"

#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <cstddef>
#include <vector>

namespace lcad {

// Move / rotate / scale applied through BRepBuilderAPI_Transform. Four
// things create these: the "Move/Rotate" dialog (numeric entry, about the
// world origin), the AIS_Manipulator move gizmo, and the rotate and scale
// gizmos -- the last three all capturing the dragged body's own centre as
// myPivot, so the timeline replay lands the body exactly where the live
// drag left it and there is no visual jump on release.
//
// The components are stored as separate scalars rather than a bare
// gp_Trsf so the properties panel can show and edit "Rotation Z" or
// "Scale" on their own, the way Parameters()/SetParameter() is meant to
// be used; Trsf() is where they get composed back into the transform
// Compute() actually applies, and where the composition ORDER is argued.
//
// TypeName() stays "Move" for every one of them: it is the stable class
// identifier the browser and the properties panel key off, not a label.
// What a rotate drag is CALLED in the timeline is its Name ("Rotate1"),
// which is the seam meant for that.
class TransformFeature : public Feature
{
public:
    TransformFeature() = default;

    // Rotation angles are in DEGREES, about axes through thePivot. The
    // one conversion to radians lives in Trsf().
    TransformFeature(double theTx, double theTy, double theTz,
                     double theRxDeg, double theRyDeg, double theRzDeg,
                     const gp_Pnt& thePivot = gp_Pnt(0.0, 0.0, 0.0));

    std::string TypeName() const override { return "Move"; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    // The transform Compute() would apply right now, given the current
    // parameters -- exposed so callers (a gizmo previewing the drag it is
    // about to commit) don't have to duplicate the composition logic.
    gp_Trsf Trsf() const;

    const gp_Pnt& Pivot() const { return myPivot; }
    void SetPivot(const gp_Pnt& thePivot) { myPivot = thePivot; }

    // UNIFORM scale about the pivot. Non-uniform per-axis scale is not
    // offered: gp_Trsf cannot represent it at all, so it would need a
    // gp_GTrsf and BRepBuilderAPI_GTransform, and half of this class
    // (Trsf(), everything that composes with it) assumes a similarity.
    // Advertising three factors and silently applying the first is the
    // wrong solid nobody notices, so there is one factor.
    double Scale() const { return myScale; }

    // False for zero, negative or non-finite. A factor of zero collapses
    // the body to nothing and a negative one turns it inside out, and
    // neither is an edit anyone means -- so the setter refuses it and
    // Compute() refuses it again, rather than handing the kernel a
    // degenerate similarity and reporting whatever comes back.
    bool SetScale(double theFactor);

    // Which body this acts on. A NULL ref means the whole upstream shape,
    // which is what the numeric dialog means and what this class did
    // before bodies were pickable; a set ref means that body alone, with
    // every other body carried through untouched.
    //
    // A ref, not a name: a feature computing mid-timeline sees only its
    // INPUT shape and there are no bodies at that point, so the pick has
    // to carry its own geometric signature and resolve against that
    // shape. CombineBodyRef deliberately, rather than a new type -- the
    // rules for what counts as the same body are Combine's rules, so a
    // pick behaves identically whichever feature is holding it.
    const CombineBodyRef& Target() const { return myTarget; }
    void SetTarget(CombineBodyRef theTarget) { myTarget = std::move(theTarget); }

private:
    double myTx = 0.0;
    double myTy = 0.0;
    double myTz = 0.0;
    double myRxDeg = 0.0;   // rotation about the pivot's X axis, degrees
    double myRyDeg = 0.0;
    double myRzDeg = 0.0;
    double myScale = 1.0;   // uniform, about the pivot
    gp_Pnt myPivot { 0.0, 0.0, 0.0 };
    CombineBodyRef myTarget;  // null = the whole upstream shape
};

// How a body reference resolved against a list of body shapes.
enum class BodyMatch
{
    Found,
    Missing,
    Ambiguous
};

// Find the one shape of theShapes that theRef names, by volume and
// centre. Refusing beats guessing, the same bargain GeometryRef and
// ProfileRef make: one re-pick is cheap, a rotate that quietly moved to
// the body next door is not.
//
// Exposed rather than file-local because the gizmos need the same
// question answered about the FINISHED document -- "which body is the one
// I just turned?" -- and a second copy of the rule in the tool would be a
// third place for it to drift. The two that already exist (CombineFeature
// and PatternFeatures, both file-local there) have to stay in step with
// BodyTable's numbers, and so does this.
BodyMatch FindBodyForRef(const std::vector<TopoDS_Shape>& theShapes,
                         const CombineBodyRef&            theRef,
                         std::size_t&                     theIndex);

} // namespace lcad
