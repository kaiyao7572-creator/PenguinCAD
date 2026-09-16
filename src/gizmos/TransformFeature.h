#pragma once

#include "core/Feature.h"

#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

namespace lcad {

// Rigid-body move/rotate applied to the whole upstream shape via
// BRepBuilderAPI_Transform. Two things create these: the "Move/Rotate"
// dialog (numeric entry, rotates about the world origin) and the
// interactive AIS_Manipulator gizmo (rotates about the dragged body's own
// centre, captured as myPivot, so the timeline replay lands the body
// exactly where the live drag left it -- no visual jump on release).
//
// Translation and rotation are stored as separate scalars rather than a
// bare gp_Trsf so the properties panel can show and edit "Move X" / "Rotate
// Z" independently, the way Parameters()/SetParameter() is meant to be
// used; Trsf() is where they get composed back into the transform Compute()
// actually applies.
class TransformFeature : public Feature
{
public:
    TransformFeature() = default;

    // Rotation angles are in degrees, about axes through thePivot.
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
    // parameters -- exposed so callers (the gizmo re-centring on a body
    // it just moved) don't have to duplicate the composition logic.
    gp_Trsf Trsf() const;

    const gp_Pnt& Pivot() const { return myPivot; }

private:
    double myTx = 0.0;
    double myTy = 0.0;
    double myTz = 0.0;
    double myRxDeg = 0.0;   // rotation about the pivot's X axis, degrees
    double myRyDeg = 0.0;
    double myRzDeg = 0.0;
    gp_Pnt myPivot { 0.0, 0.0, 0.0 };
};

} // namespace lcad
