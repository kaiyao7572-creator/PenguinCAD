#pragma once

#include "core/Feature.h"
#include "features/FeatureUtils.h"

#include <TopoDS_Face.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pln.hxx>

#include <string>
#include <vector>

namespace lcad {

// Shared base for the features that turn a sketch into a solid.
//
// The sketch is held by NAME and resolved at compute time through
// ProfileProvider: undo clones the whole timeline, so a pointer to the
// sketch would go stale, and this side of the seam never needs to know
// SketchFeature's concrete type.
class ProfileFeature : public Feature
{
public:
    const std::string& SketchName() const { return mySketchName; }
    void SetSketchName(std::string theName) { mySketchName = std::move(theName); }

    BooleanOp Operation() const { return myOperation; }
    void SetOperation(BooleanOp theOperation) { myOperation = theOperation; }

protected:
    // Faces of the named sketch. Fails with a message meant to be shown
    // to the user as-is: a missing sketch and an open sketch are both
    // ordinary mistakes, not kernel errors.
    bool ResolveProfile(const ComputeContext&     theContext,
                        std::vector<TopoDS_Face>& theFaces,
                        gp_Pln&                   thePlane,
                        std::string&              theError) const;

    // Shared "Sketch" and "Operation" rows.
    void AppendCommonParameters(std::vector<Parameter>& theParameters) const;
    bool ApplyCommonParameter(const Parameter& theParameter);

    void CopyProfileTo(ProfileFeature& theOther) const;

private:
    std::string mySketchName;
    BooleanOp   myOperation = BooleanOp::NewBody;
};

// Sketch profile pushed along its own normal.
class ExtrudeFeature : public ProfileFeature
{
public:
    ExtrudeFeature() = default;
    ExtrudeFeature(std::string theSketchName, double theDistance);

    std::string TypeName() const override { return "Extrude"; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    double Distance() const { return myDistance; }
    void SetDistance(double theDistance) { myDistance = theDistance; }

    bool IsReversed() const { return myIsReversed; }
    void SetReversed(bool theValue) { myIsReversed = theValue; }

    bool IsSymmetric() const { return myIsSymmetric; }
    void SetSymmetric(bool theValue) { myIsSymmetric = theValue; }

private:
    double myDistance    = 10.0;
    bool   myIsReversed  = false;
    bool   myIsSymmetric = false;
};

// Which line the profile spins around. The sketch axes pass through the
// sketch origin and always lie in the profile plane, so they are the two
// that can never produce a degenerate sweep; the world axes are offered
// because a profile sketched on XZ is usually turned about world Z.
enum class RevolveAxis
{
    SketchX = 0,
    SketchY = 1,
    WorldX  = 2,
    WorldY  = 3,
    WorldZ  = 4
};

const std::vector<std::string>& RevolveAxisNames();
std::string RevolveAxisName(RevolveAxis theAxis);
RevolveAxis RevolveAxisFromInt(int theValue);
bool ParseRevolveAxis(const std::string& theText, RevolveAxis& theAxis);

// Sketch profile turned about an axis.
class RevolveFeature : public ProfileFeature
{
public:
    RevolveFeature() = default;
    RevolveFeature(std::string theSketchName, double theAngleDegrees, RevolveAxis theAxis);

    std::string TypeName() const override { return "Revolve"; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    double AngleDegrees() const { return myAngleDegrees; }
    void SetAngleDegrees(double theAngle) { myAngleDegrees = theAngle; }

    RevolveAxis Axis() const { return myAxis; }
    void SetAxis(RevolveAxis theAxis) { myAxis = theAxis; }

    bool IsReversed() const { return myIsReversed; }
    void SetReversed(bool theValue) { myIsReversed = theValue; }

private:
    gp_Ax1 AxisOf(const gp_Pln& thePlane) const;

    double      myAngleDegrees = 360.0;
    RevolveAxis myAxis         = RevolveAxis::SketchY;
    bool        myIsReversed   = false;
};

} // namespace lcad
