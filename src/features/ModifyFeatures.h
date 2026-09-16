#pragma once

#include "core/Feature.h"

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <string>
#include <vector>

namespace lcad {

// Shared base for features that reshape whatever the timeline has built
// so far. Each body in the upstream shape is modified on its own and the
// results are reassembled, so a document holding two bodies doesn't lose
// one to a fillet.
class ModifyFeature : public Feature
{
public:
    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

protected:
    virtual bool ModifySolid(const TopoDS_Shape& theSolid,
                             TopoDS_Shape&       theResult,
                             std::string&        theError) const = 0;

    // What to tell the user when there is no body to work on yet.
    virtual std::string EmptyInputError() const = 0;
};

// Rounds every edge of every body. Edge-by-edge selection would need
// persistent topological names, which this timeline doesn't have yet.
class FilletFeature : public ModifyFeature
{
public:
    FilletFeature() = default;
    explicit FilletFeature(double theRadius);

    std::string TypeName() const override { return "Fillet"; }
    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    double Radius() const { return myRadius; }
    void SetRadius(double theRadius) { myRadius = theRadius; }

protected:
    bool ModifySolid(const TopoDS_Shape& theSolid,
                     TopoDS_Shape&       theResult,
                     std::string&        theError) const override;
    std::string EmptyInputError() const override;

private:
    double myRadius = 2.0;
};

// Flattens every edge of every body by an equal distance on both faces.
class ChamferFeature : public ModifyFeature
{
public:
    ChamferFeature() = default;
    explicit ChamferFeature(double theDistance);

    std::string TypeName() const override { return "Chamfer"; }
    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    double Distance() const { return myDistance; }
    void SetDistance(double theDistance) { myDistance = theDistance; }

protected:
    bool ModifySolid(const TopoDS_Shape& theSolid,
                     TopoDS_Shape&       theResult,
                     std::string&        theError) const override;
    std::string EmptyInputError() const override;

private:
    double myDistance = 1.0;
};

// Which face gets removed to turn a hollow body into an open one. Picking
// a face in the viewport would pin the shell to a face index that every
// upstream edit invalidates, so the opening is described by a rule the
// rebuild can re-evaluate instead.
enum class ShellOpening
{
    None    = 0,
    Top     = 1,
    Bottom  = 2,
    Largest = 3
};

const std::vector<std::string>& ShellOpeningNames();
std::string ShellOpeningName(ShellOpening theOpening);
ShellOpening ShellOpeningFromInt(int theValue);
bool ParseShellOpening(const std::string& theText, ShellOpening& theOpening);

// Hollows every body out to a wall thickness.
class ShellFeature : public ModifyFeature
{
public:
    ShellFeature() = default;
    ShellFeature(double theThickness, ShellOpening theOpening);

    std::string TypeName() const override { return "Shell"; }
    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    double Thickness() const { return myThickness; }
    void SetThickness(double theThickness) { myThickness = theThickness; }

    ShellOpening Opening() const { return myOpening; }
    void SetOpening(ShellOpening theOpening) { myOpening = theOpening; }

    bool IsOutward() const { return myIsOutward; }
    void SetOutward(bool theValue) { myIsOutward = theValue; }

protected:
    bool ModifySolid(const TopoDS_Shape& theSolid,
                     TopoDS_Shape&       theResult,
                     std::string&        theError) const override;
    std::string EmptyInputError() const override;

private:
    double       myThickness = 1.0;
    ShellOpening myOpening   = ShellOpening::Top;
    bool         myIsOutward = false;
};

} // namespace lcad
