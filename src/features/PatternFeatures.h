#pragma once

#include "core/Feature.h"
#include "features/CombineFeature.h"
#include "features/FeatureUtils.h"

#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Trsf.hxx>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace lcad {

// Which world axis a pattern marches along, or turns about.
//
// A named set rather than a free vector, in the spirit of RevolveAxis: a
// direction typed in as three numbers can be zero length, and a
// zero-length direction is a pattern that stacks every instance on the
// first -- doubled volume and no error. The world axes all pass through
// the origin, so there is nothing to get wrong. The integer values are
// part of the stored parameter, so don't renumber them.
enum class PatternAxis
{
    WorldX = 0,
    WorldY = 1,
    WorldZ = 2
};

// Labels in enum order; dialogs and the properties panel share them so a
// value typed into one is understood by the other.
const std::vector<std::string>& PatternAxisNames();
std::string PatternAxisName(PatternAxis theAxis);
PatternAxis PatternAxisFromInt(int theValue);
bool ParsePatternAxis(const std::string& theText, PatternAxis& theAxis);
gp_Dir PatternAxisDirection(PatternAxis theAxis);

// Which world plane a mirror reflects across. Same discipline as
// PatternAxis, and the same reason: the integers are stored.
enum class MirrorPlane
{
    XY = 0,
    XZ = 1,
    YZ = 2
};

const std::vector<std::string>& MirrorPlaneNames();
std::string MirrorPlaneName(MirrorPlane thePlane);
MirrorPlane MirrorPlaneFromInt(int theValue);
bool ParseMirrorPlane(const std::string& theText, MirrorPlane& thePlane);

// The plane as gp_Trsf::SetMirror wants it: the world origin, with Z
// along the plane's normal.
gp_Ax2 MirrorPlaneFrame(MirrorPlane thePlane);

// Shared plumbing for the features that COPY bodies rather than create
// geometry: a rectangular grid, a ring, a reflection.
//
// Every one of them does the same three things -- resolve the bodies the
// user picked, place a copy of each at a list of transforms, and combine
// the copies back into the model -- and differ only in what that list of
// transforms is. So the list is all a subclass has to produce.
//
// The picks are CombineBodyRefs, deliberately the type Combine already
// uses rather than a new one. A feature computing mid-timeline sees only
// its INPUT shape and there are no bodies at that point, so a pick has to
// carry its own geometric signature and resolve against that shape; the
// name is what the user chose and what an error message says. The rules
// for what counts as the same body are Combine's rules, so a pick
// behaves identically whichever feature holds it.
//
// The SEED is not copied. Instance one of any pattern is the body that is
// already in the model, so the tool handed to the boolean holds instances
// two upwards -- which is what makes a four-instance pattern come out at
// four times the volume rather than five.
class PatternFeature : public Feature
{
public:
    const std::vector<CombineBodyRef>& Bodies() const { return myBodies; }

    // Drops null and duplicate refs on the way in. The same body twice
    // would be copied to the same places twice, which doubles the
    // reported volume and hands every downstream boolean a tool that
    // overlaps itself -- the failure ProfileFeature::SetProfiles refuses
    // for duplicate regions, arriving through a different door.
    void SetBodies(std::vector<CombineBodyRef> theBodies);

    BooleanOp Operation() const { return myOperation; }
    void SetOperation(BooleanOp theOperation) { myOperation = theOperation; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    // The most instances one feature will place, counting the seeds. A
    // mistyped quantity is much likelier than a genuine ten-thousand-hole
    // grid, and the kernel would be inside the boolean for minutes before
    // anyone found out. Public so the creation dialog and the properties
    // panel can bound their quantity fields by the number Compute
    // enforces, instead of offering a grid that is refused on OK.
    static constexpr std::size_t kMaxInstances = 1000;

protected:
    // Where every instance AFTER the seed goes, in the order the user
    // would count them. An empty list is a pattern of one, which is a
    // no-op rather than an error. Return false with a readable theError
    // for a quantity or spacing that cannot describe a pattern.
    virtual bool InstanceTransforms(std::vector<gp_Trsf>& theTransforms,
                                    std::string&          theError) const = 0;

    // What this feature is called in an error the user reads: "pattern",
    // "mirror".
    virtual std::string Noun() const = 0;

    // Shared "Bodies" and "Operation" rows.
    void AppendCommonParameters(std::vector<Parameter>& theParameters) const;
    bool ApplyCommonParameter(const Parameter& theParameter);

    void CopyPatternTo(PatternFeature& theOther) const;

private:
    std::vector<CombineBodyRef> myBodies;  // never holds a null ref
    BooleanOp                   myOperation = BooleanOp::NewBody;
};

// Fusion's CREATE > PATTERN > RECTANGULAR PATTERN, at the body level.
//
// Two independent directions, each with a quantity and a spacing. The
// spacing is Fusion's "Spacing" distribution: the gap between one
// instance and the next, not the overall extent, so raising the quantity
// makes the grid longer instead of packing more copies into the same
// span.
class RectangularPatternFeature : public PatternFeature
{
public:
    RectangularPatternFeature() = default;
    RectangularPatternFeature(std::vector<CombineBodyRef> theBodies,
                              int                         theCount1,
                              double                      theSpacing1,
                              int                         theCount2,
                              double                      theSpacing2);

    std::string TypeName() const override { return "RectangularPattern"; }

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    PatternAxis Direction1() const { return myDirection1; }
    void SetDirection1(PatternAxis theAxis) { myDirection1 = theAxis; }

    int Count1() const { return myCount1; }
    void SetCount1(int theCount) { myCount1 = theCount; }

    double Spacing1() const { return mySpacing1; }
    void SetSpacing1(double theSpacing) { mySpacing1 = theSpacing; }

    PatternAxis Direction2() const { return myDirection2; }
    void SetDirection2(PatternAxis theAxis) { myDirection2 = theAxis; }

    int Count2() const { return myCount2; }
    void SetCount2(int theCount) { myCount2 = theCount; }

    double Spacing2() const { return mySpacing2; }
    void SetSpacing2(double theSpacing) { mySpacing2 = theSpacing; }

protected:
    bool InstanceTransforms(std::vector<gp_Trsf>& theTransforms,
                            std::string&          theError) const override;
    std::string Noun() const override { return "pattern"; }

private:
    PatternAxis myDirection1 = PatternAxis::WorldX;
    int         myCount1     = 2;
    double      mySpacing1   = 20.0;

    // One by default, so a fresh pattern is a single row: Fusion's second
    // direction is opt-in, and a grid nobody asked for is a surprising
    // thing to have to switch off.
    PatternAxis myDirection2 = PatternAxis::WorldY;
    int         myCount2     = 1;
    double      mySpacing2   = 20.0;
};

// Fusion's CREATE > PATTERN > CIRCULAR PATTERN, at the body level.
//
// An axis, a quantity and a total angle, with the instances spread evenly
// over that angle. How wide a step that is depends on whether the angle
// closes the ring, and getting it backwards is the classic circular
// pattern bug -- see InstanceTransforms, where the choice is made and
// argued.
class CircularPatternFeature : public PatternFeature
{
public:
    CircularPatternFeature() = default;
    CircularPatternFeature(std::vector<CombineBodyRef> theBodies,
                           PatternAxis                 theAxis,
                           int                         theCount,
                           double                      theTotalAngleDegrees);

    std::string TypeName() const override { return "CircularPattern"; }

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    PatternAxis Axis() const { return myAxis; }
    void SetAxis(PatternAxis theAxis) { myAxis = theAxis; }

    int Count() const { return myCount; }
    void SetCount(int theCount) { myCount = theCount; }

    // DEGREES, like every other angle this app stores. The conversion to
    // radians happens once, inside InstanceTransforms.
    double TotalAngleDegrees() const { return myTotalAngleDegrees; }
    void SetTotalAngleDegrees(double theAngle) { myTotalAngleDegrees = theAngle; }

protected:
    bool InstanceTransforms(std::vector<gp_Trsf>& theTransforms,
                            std::string&          theError) const override;
    std::string Noun() const override { return "pattern"; }

private:
    PatternAxis myAxis              = PatternAxis::WorldZ;
    int         myCount             = 4;
    double      myTotalAngleDegrees = 360.0;
};

// Fusion's CREATE > MIRROR, at the body level.
//
// The sketch subsystem already mirrors curves; this is the other half,
// and the one that earns its keep: it reflects the RESULT of a fillet and
// a shell, which is a shape no sketch mirror can reach.
class MirrorFeature : public PatternFeature
{
public:
    MirrorFeature() = default;
    MirrorFeature(std::vector<CombineBodyRef> theBodies, MirrorPlane thePlane);

    std::string TypeName() const override { return "Mirror"; }

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    MirrorPlane Plane() const { return myPlane; }
    void SetPlane(MirrorPlane thePlane) { myPlane = thePlane; }

protected:
    bool InstanceTransforms(std::vector<gp_Trsf>& theTransforms,
                            std::string&          theError) const override;
    std::string Noun() const override { return "mirror"; }

private:
    MirrorPlane myPlane = MirrorPlane::YZ;
};

} // namespace lcad
