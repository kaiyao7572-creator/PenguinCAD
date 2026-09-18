#pragma once

#include "core/ConstructionGeometry.h"
#include "core/Feature.h"

#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>

#include <string>
#include <vector>

namespace lcad {

// Construction geometry: the planes, axes and points a user builds to
// position things no existing geometry gives them a handle on. Fusion
// puts these under CONSTRUCT and files them in the Construction folder.
//
// They produce no solid -- like a sketch, they pass the upstream shape
// straight through -- but they take part in the timeline, so moving one
// rebuilds everything downstream of it.

// How a construction plane is defined. Fusion offers more ways than
// these, but every one it offers that needs a PICKED face or edge needs
// selection plumbing that does not exist yet; the ones here are fully
// parametric, which is also what makes them testable without a display.
enum class PlaneKind
{
    Offset = 0,      // parallel to a base plane, a distance from it
    AtAngle = 1,     // the base plane rotated about one of its own axes
    Midplane = 2,    // halfway between two named planes
    ThreePoints = 3  // through three points
};

const std::vector<std::string>& PlaneKindNames();
std::string PlaneKindName(PlaneKind theKind);
bool ParsePlaneKind(const std::string& theText, PlaneKind& theKind);

class ConstructionPlaneFeature : public Feature, public ConstructionGeometry
{
public:
    ConstructionPlaneFeature() = default;
    ConstructionPlaneFeature(std::string theBasePlane, double theOffset);

    std::string TypeName() const override { return "ConstructionPlane"; }
    EntityType ConstructionType() const override { return EntityType::ConstructionPlane; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    bool AsPlane(gp_Ax3& thePlane) const override;
    bool AsPoint(gp_Pnt& thePoint) const override;

    PlaneKind Kind() const { return myKind; }
    void SetKind(PlaneKind theKind) { myKind = theKind; }

    // Origin-folder name ("XY") or an earlier construction plane's name.
    const std::string& BasePlane() const { return myBasePlane; }
    void SetBasePlane(std::string theName) { myBasePlane = std::move(theName); }

    const std::string& SecondPlane() const { return mySecondPlane; }
    void SetSecondPlane(std::string theName) { mySecondPlane = std::move(theName); }

    double Offset() const { return myOffset; }
    void SetOffset(double theValue) { myOffset = theValue; }

    double AngleDegrees() const { return myAngleDegrees; }
    void SetAngleDegrees(double theValue) { myAngleDegrees = theValue; }

    void SetPoints(const gp_Pnt& theA, const gp_Pnt& theB, const gp_Pnt& theC);

private:
    // Evaluate without a document, for the cases that need no lookup.
    bool Build(const ComputeContext& theContext, gp_Ax3& theResult, std::string& theError) const;

    PlaneKind   myKind = PlaneKind::Offset;
    std::string myBasePlane = "XY";
    std::string mySecondPlane;
    double      myOffset = 10.0;
    double      myAngleDegrees = 45.0;
    gp_Pnt      myPointA{0.0, 0.0, 0.0};
    gp_Pnt      myPointB{10.0, 0.0, 0.0};
    gp_Pnt      myPointC{0.0, 10.0, 0.0};

    // Last successful result, so AsPlane can answer after the rebuild
    // without redoing the lookup it no longer has a context for.
    gp_Ax3 myResult;
    bool   myIsValid = false;
};

// How a construction axis is defined.
enum class AxisKind
{
    TwoPoints = 0,          // through two points
    PerpendicularToPlane = 1  // normal to a plane, through its origin
};

const std::vector<std::string>& AxisKindNames();
std::string AxisKindName(AxisKind theKind);
bool ParseAxisKind(const std::string& theText, AxisKind& theKind);

class ConstructionAxisFeature : public Feature, public ConstructionGeometry
{
public:
    ConstructionAxisFeature() = default;

    std::string TypeName() const override { return "ConstructionAxis"; }
    EntityType ConstructionType() const override { return EntityType::ConstructionAxis; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    bool AsAxis(gp_Ax1& theAxis) const override;
    bool AsPoint(gp_Pnt& thePoint) const override;

    AxisKind Kind() const { return myKind; }
    void SetKind(AxisKind theKind) { myKind = theKind; }

    void SetPoints(const gp_Pnt& theFrom, const gp_Pnt& theTo);

    const std::string& BasePlane() const { return myBasePlane; }
    void SetBasePlane(std::string theName) { myBasePlane = std::move(theName); }

private:
    bool Build(const ComputeContext& theContext, gp_Ax1& theResult, std::string& theError) const;

    AxisKind    myKind = AxisKind::TwoPoints;
    gp_Pnt      myFrom{0.0, 0.0, 0.0};
    gp_Pnt      myTo{0.0, 0.0, 10.0};
    std::string myBasePlane = "XY";

    gp_Ax1 myResult;
    bool   myIsValid = false;
};

// How a construction point is defined.
enum class PointKind
{
    AtCoordinates = 0,       // typed in
    AxisPlaneIntersection = 1  // where a named axis crosses a named plane
};

const std::vector<std::string>& PointKindNames();
std::string PointKindName(PointKind theKind);
bool ParsePointKind(const std::string& theText, PointKind& theKind);

class ConstructionPointFeature : public Feature, public ConstructionGeometry
{
public:
    ConstructionPointFeature() = default;
    explicit ConstructionPointFeature(const gp_Pnt& thePosition);

    std::string TypeName() const override { return "ConstructionPoint"; }
    EntityType ConstructionType() const override { return EntityType::ConstructionPoint; }

    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::unique_ptr<Feature> Clone() const override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    bool AsPoint(gp_Pnt& thePoint) const override;

    PointKind Kind() const { return myKind; }
    void SetKind(PointKind theKind) { myKind = theKind; }

    void SetPosition(const gp_Pnt& thePosition) { myPosition = thePosition; }

    void SetAxisName(std::string theName) { myAxisName = std::move(theName); }
    void SetPlaneName(std::string theName) { myPlaneName = std::move(theName); }

private:
    bool Build(const ComputeContext& theContext, gp_Pnt& theResult, std::string& theError) const;

    PointKind   myKind = PointKind::AtCoordinates;
    gp_Pnt      myPosition{0.0, 0.0, 0.0};
    std::string myAxisName = "Z";
    std::string myPlaneName = "XY";

    gp_Pnt myResult;
    bool   myIsValid = false;
};

} // namespace lcad
