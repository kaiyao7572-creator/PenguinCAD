#pragma once

#include "core/Feature.h"
#include "features/FeatureUtils.h"

#include <gp_Ax2.hxx>
#include <gp_Pnt.hxx>

namespace lcad {

// Shared plumbing for the analytic primitives. They differ only in the
// shape they build and the dimensions they expose, so placement, the
// boolean operation and the never-throw Compute() wrapper live here once.
class PrimitiveFeature : public Feature
{
public:
    bool Compute(const ComputeContext& theContext,
                 const TopoDS_Shape&   theInput,
                 TopoDS_Shape&         theOutput,
                 std::string&          theError) override;

    std::vector<Parameter> Parameters() const override;
    bool SetParameter(const Parameter& theParameter) override;

    const gp_Pnt& Origin() const { return myOrigin; }
    void SetOrigin(const gp_Pnt& theOrigin) { myOrigin = theOrigin; }

    BooleanOp Operation() const { return myOperation; }
    void SetOperation(BooleanOp theOperation) { myOperation = theOperation; }

protected:
    // Build the primitive at the origin. Dimension validation belongs
    // here rather than in Compute so each primitive words its own
    // message ("radius must be greater than zero", not "bad input").
    virtual bool MakeShape(TopoDS_Shape& theShape, std::string& theError) const = 0;

    // Dimensions beyond the shared origin and operation.
    virtual void AppendParameters(std::vector<Parameter>& theParameters) const = 0;
    virtual bool ApplyParameter(const Parameter& theParameter) = 0;

    // Local frame with Z up through the origin: the axis every primitive
    // but the box is built around.
    gp_Ax2 Axis() const;

    // Carries origin and operation into a Clone(); the caller still has
    // to copy its own dimensions and call CopyBaseTo.
    void CopyPrimitiveTo(PrimitiveFeature& theOther) const;

private:
    gp_Pnt    myOrigin    = gp_Pnt(0.0, 0.0, 0.0);
    BooleanOp myOperation = BooleanOp::NewBody;
};

class BoxFeature : public PrimitiveFeature
{
public:
    BoxFeature() = default;
    BoxFeature(double theLength, double theWidth, double theHeight);

    std::string TypeName() const override { return "Box"; }
    std::unique_ptr<Feature> Clone() const override;

    double Length() const { return myLength; }
    double Width() const { return myWidth; }
    double Height() const { return myHeight; }

protected:
    bool MakeShape(TopoDS_Shape& theShape, std::string& theError) const override;
    void AppendParameters(std::vector<Parameter>& theParameters) const override;
    bool ApplyParameter(const Parameter& theParameter) override;

private:
    double myLength = 10.0;
    double myWidth  = 10.0;
    double myHeight = 10.0;
};

class CylinderFeature : public PrimitiveFeature
{
public:
    CylinderFeature() = default;
    CylinderFeature(double theRadius, double theHeight);

    std::string TypeName() const override { return "Cylinder"; }
    std::unique_ptr<Feature> Clone() const override;

    double Radius() const { return myRadius; }
    double Height() const { return myHeight; }

protected:
    bool MakeShape(TopoDS_Shape& theShape, std::string& theError) const override;
    void AppendParameters(std::vector<Parameter>& theParameters) const override;
    bool ApplyParameter(const Parameter& theParameter) override;

private:
    double myRadius = 5.0;
    double myHeight = 10.0;
};

class SphereFeature : public PrimitiveFeature
{
public:
    SphereFeature() = default;
    explicit SphereFeature(double theRadius);

    std::string TypeName() const override { return "Sphere"; }
    std::unique_ptr<Feature> Clone() const override;

    double Radius() const { return myRadius; }

protected:
    bool MakeShape(TopoDS_Shape& theShape, std::string& theError) const override;
    void AppendParameters(std::vector<Parameter>& theParameters) const override;
    bool ApplyParameter(const Parameter& theParameter) override;

private:
    double myRadius = 5.0;
};

class ConeFeature : public PrimitiveFeature
{
public:
    ConeFeature() = default;
    ConeFeature(double theBottomRadius, double theTopRadius, double theHeight);

    std::string TypeName() const override { return "Cone"; }
    std::unique_ptr<Feature> Clone() const override;

    double BottomRadius() const { return myBottomRadius; }
    double TopRadius() const { return myTopRadius; }
    double Height() const { return myHeight; }

protected:
    bool MakeShape(TopoDS_Shape& theShape, std::string& theError) const override;
    void AppendParameters(std::vector<Parameter>& theParameters) const override;
    bool ApplyParameter(const Parameter& theParameter) override;

private:
    double myBottomRadius = 5.0;
    double myTopRadius    = 0.0;
    double myHeight       = 10.0;
};

class TorusFeature : public PrimitiveFeature
{
public:
    TorusFeature() = default;
    TorusFeature(double theMajorRadius, double theMinorRadius);

    std::string TypeName() const override { return "Torus"; }
    std::unique_ptr<Feature> Clone() const override;

    double MajorRadius() const { return myMajorRadius; }
    double MinorRadius() const { return myMinorRadius; }

protected:
    bool MakeShape(TopoDS_Shape& theShape, std::string& theError) const override;
    void AppendParameters(std::vector<Parameter>& theParameters) const override;
    bool ApplyParameter(const Parameter& theParameter) override;

private:
    double myMajorRadius = 10.0;
    double myMinorRadius = 3.0;
};

} // namespace lcad
