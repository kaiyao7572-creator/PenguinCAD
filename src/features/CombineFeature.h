#pragma once

#include "core/Feature.h"
#include "features/FeatureUtils.h"

#include <gp_Pnt.hxx>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace lcad {

class Body;

// A durable reference to one solid body.
//
// Same bargain GeometryRef makes, and for the same reason: a feature
// computing mid-timeline sees only its INPUT shape, and there are no
// bodies at that point -- Document's body table is derived from the
// FINISHED document, so by the time a combine recomputes, the table
// describes the model the previous combine already produced and the names
// it was built from are not in it any more. A Join consumes Body1 and
// Body2 and the result is called Body3; looking the picks up by name in
// the table would work exactly once.
//
// So the name is what the user picked and what an error message says,
// while the volume and the centre are what actually find the body again
// in the input shape. Two independent signals, and the same refusal to
// guess: nothing matching, or two things matching equally well, FAILS
// rather than combining whichever came first.
struct CombineBodyRef
{
    std::string name;          // the body's name when it was picked
    gp_Pnt      centre;        // centre of mass
    double      volume = 0.0;  // zero for anything that is not a solid

    // Both halves have to be there: a ref with no signature cannot be
    // resolved, and one with no name cannot be reported.
    bool IsNull() const { return name.empty() || std::fabs(volume) <= 0.0; }

    // "Body1|12.5,0,7.25|1000" -- rides a plain string Parameter, so which
    // bodies a combine was built on is inspectable and editable in the
    // properties panel with no new widget.
    std::string Encode() const;
    static bool Decode(const std::string& theText, CombineBodyRef& theResult);
};

std::string EncodeCombineBodyRefs(const std::vector<CombineBodyRef>& theRefs);
std::vector<CombineBodyRef> DecodeCombineBodyRefs(const std::string& theText);

// How many ';'-separated segments a string holds, parsed or not. Used to
// tell "this text is a typo" from "this text lists fewer bodies".
std::size_t CountCombineBodyRefSegments(const std::string& theText);

// Describe a body as it stands now. Null for a body with no volume:
// Combine is a solid operation, and Join, Cut and Intersect mean nothing
// for a surface or a wire.
CombineBodyRef MakeCombineBodyRef(const Body& theBody);

// The operations Fusion's Combine offers, in dropdown order. NewBody is
// deliberately absent: two bodies that already exist are already separate
// bodies, so it would be a no-op dressed up as a choice.
const std::vector<std::string>& CombineOperationNames();
bool IsCombineOperation(BooleanOp theOperation);
BooleanOp CombineOperationFromChoice(int theChoice);
int CombineOperationChoice(BooleanOp theOperation);

// Fusion's MODIFY > COMBINE: one target body, one or more tool bodies,
// joined / cut / intersected.
//
// Extrude and Revolve already carry the same operation dropdown, but they
// apply it to geometry they are CREATING, against everything upstream of
// them. This is the other half: combining two bodies that already exist,
// choosing which is which. The boolean itself goes through
// ApplyBooleanOperation so the two behave identically.
//
// Bodies the combine does not name are carried through untouched -- a
// document holding four bodies does not lose two to a combine of the
// other two -- and the tools are consumed unless KeepTools is set, which
// is Fusion's "Keep Tools" checkbox. Fusion's "New Component" option is
// not offered: there is no component model here yet.
//
// Several tools are applied one at a time rather than fused into one
// cutter first. For Join and Cut the two are equivalent; for Intersect
// they are not -- (target n t1) n t2 is the material shared by ALL of
// them, which is what Intersect means, while target n (t1 u t2) would
// keep material shared with EITHER. It also means every boolean is handed
// one solid against one solid, never a self-overlapping compound.
class CombineFeature : public Feature
{
public:
    CombineFeature() = default;
    CombineFeature(CombineBodyRef              theTarget,
                   std::vector<CombineBodyRef> theTools,
                   BooleanOp                   theOperation);

    std::string TypeName() const override { return "Combine"; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    const CombineBodyRef& Target() const { return myTarget; }
    void SetTarget(CombineBodyRef theTarget);

    const std::vector<CombineBodyRef>& Tools() const { return myTools; }

    // Drops null and duplicate refs on the way in: the same body twice
    // would be cut away twice, and for Join it would hand the kernel a
    // body fused with itself.
    void SetTools(std::vector<CombineBodyRef> theTools);

    BooleanOp Operation() const { return myOperation; }

    // False for anything Combine does not offer, leaving the stored
    // operation alone. Accepting NewBody and then quietly doing nothing
    // with it is the failure this refuses.
    bool SetOperation(BooleanOp theOperation);

    bool KeepTools() const { return myKeepTools; }
    void SetKeepTools(bool theValue) { myKeepTools = theValue; }

private:
    CombineBodyRef              myTarget;
    std::vector<CombineBodyRef> myTools;  // never holds a null ref
    BooleanOp                   myOperation = BooleanOp::Join;
    bool                        myKeepTools = false;
};

} // namespace lcad
