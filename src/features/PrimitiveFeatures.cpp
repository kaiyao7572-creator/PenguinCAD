#include "features/PrimitiveFeatures.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <Standard_Failure.hxx>
#include <gp_Dir.hxx>

#include <cmath>

namespace lcad {

namespace {

// Dimensions are millimetres; anything below this is a degenerate solid
// the kernel would either reject or turn into invisible slivers.
constexpr double kMinSize = 1.0e-7;

std::string OcctMessage(const Standard_Failure& theFailure, const std::string& theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message) : theFallback;
}

// Z up, or Z down when the height is negative -- so a drilling cylinder
// can point downwards without the user having to move its origin too.
gp_Ax2 AxisForHeight(const gp_Pnt& theOrigin, double theHeight)
{
    return gp_Ax2(theOrigin, gp_Dir(0.0, 0.0, theHeight < 0.0 ? -1.0 : 1.0));
}

bool ReadDouble(const Parameter& theParameter, const char* theName, double& theTarget)
{
    if (theParameter.name != theName) {
        return false;
    }
    theTarget = theParameter.doubleValue;
    return true;
}

} // namespace

// ---- PrimitiveFeature ----

gp_Ax2 PrimitiveFeature::Axis() const
{
    return gp_Ax2(myOrigin, gp_Dir(0.0, 0.0, 1.0));
}

void PrimitiveFeature::CopyPrimitiveTo(PrimitiveFeature& theOther) const
{
    theOther.myOrigin = myOrigin;
    theOther.myOperation = myOperation;
    CopyBaseTo(theOther);
}

bool PrimitiveFeature::Compute(const ComputeContext& theContext,
                               const TopoDS_Shape&   theInput,
                               TopoDS_Shape&         theOutput,
                               std::string&          theError)
{
    (void)theContext;

    TopoDS_Shape shape;
    try {
        if (!MakeShape(shape, theError)) {
            if (theError.empty()) {
                theError = "invalid dimensions";
            }
            return false;
        }
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "could not build " + TypeName());
        return false;
    }

    if (shape.IsNull()) {
        theError = "could not build " + TypeName();
        return false;
    }

    return ApplyBooleanOperation(myOperation, theInput, shape, theOutput, theError);
}

std::vector<Parameter> PrimitiveFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    AppendParameters(parameters);
    parameters.push_back(Parameter::MakeDouble("Origin X", myOrigin.X()));
    parameters.push_back(Parameter::MakeDouble("Origin Y", myOrigin.Y()));
    parameters.push_back(Parameter::MakeDouble("Origin Z", myOrigin.Z()));
    parameters.push_back(Parameter::MakeString("Operation", BooleanOpName(myOperation)));
    return parameters;
}

bool PrimitiveFeature::SetParameter(const Parameter& theParameter)
{
    if (ApplyParameter(theParameter)) {
        return true;
    }
    if (theParameter.name == "Origin X") {
        myOrigin.SetX(theParameter.doubleValue);
        return true;
    }
    if (theParameter.name == "Origin Y") {
        myOrigin.SetY(theParameter.doubleValue);
        return true;
    }
    if (theParameter.name == "Origin Z") {
        myOrigin.SetZ(theParameter.doubleValue);
        return true;
    }
    if (theParameter.name == "Operation") {
        BooleanOp operation = myOperation;
        if (!ParseBooleanOp(theParameter.stringValue, operation)) {
            return false;
        }
        myOperation = operation;
        return true;
    }
    return false;
}

// ---- Box ----

BoxFeature::BoxFeature(double theLength, double theWidth, double theHeight)
    : myLength(theLength), myWidth(theWidth), myHeight(theHeight)
{
}

std::unique_ptr<Feature> BoxFeature::Clone() const
{
    auto copy = std::make_unique<BoxFeature>(myLength, myWidth, myHeight);
    CopyPrimitiveTo(*copy);
    return copy;
}

bool BoxFeature::MakeShape(TopoDS_Shape& theShape, std::string& theError) const
{
    if (std::fabs(myLength) < kMinSize) {
        theError = "length must be greater than zero";
        return false;
    }
    if (std::fabs(myWidth) < kMinSize) {
        theError = "width must be greater than zero";
        return false;
    }
    if (std::fabs(myHeight) < kMinSize) {
        theError = "height must be greater than zero";
        return false;
    }

    // Built from two corners rather than corner-plus-sizes: that form
    // copes with negative dimensions, which a parameter edit can produce
    // even though the creation dialog can't.
    const gp_Pnt& origin = Origin();
    const gp_Pnt opposite(origin.X() + myLength, origin.Y() + myWidth, origin.Z() + myHeight);

    BRepPrimAPI_MakeBox maker(origin, opposite);
    maker.Build();
    if (!maker.IsDone()) {
        theError = "could not build box";
        return false;
    }
    theShape = maker.Shape();
    return true;
}

void BoxFeature::AppendParameters(std::vector<Parameter>& theParameters) const
{
    theParameters.push_back(Parameter::MakeDouble("Length", myLength));
    theParameters.push_back(Parameter::MakeDouble("Width", myWidth));
    theParameters.push_back(Parameter::MakeDouble("Height", myHeight));
}

bool BoxFeature::ApplyParameter(const Parameter& theParameter)
{
    return ReadDouble(theParameter, "Length", myLength)
        || ReadDouble(theParameter, "Width", myWidth)
        || ReadDouble(theParameter, "Height", myHeight);
}

// ---- Cylinder ----

CylinderFeature::CylinderFeature(double theRadius, double theHeight)
    : myRadius(theRadius), myHeight(theHeight)
{
}

std::unique_ptr<Feature> CylinderFeature::Clone() const
{
    auto copy = std::make_unique<CylinderFeature>(myRadius, myHeight);
    CopyPrimitiveTo(*copy);
    return copy;
}

bool CylinderFeature::MakeShape(TopoDS_Shape& theShape, std::string& theError) const
{
    if (myRadius < kMinSize) {
        theError = "radius must be greater than zero";
        return false;
    }
    if (std::fabs(myHeight) < kMinSize) {
        theError = "height must be greater than zero";
        return false;
    }

    BRepPrimAPI_MakeCylinder maker(AxisForHeight(Origin(), myHeight),
                                   myRadius,
                                   std::fabs(myHeight));
    maker.Build();
    if (!maker.IsDone()) {
        theError = "could not build cylinder";
        return false;
    }
    theShape = maker.Shape();
    return true;
}

void CylinderFeature::AppendParameters(std::vector<Parameter>& theParameters) const
{
    theParameters.push_back(Parameter::MakeDouble("Radius", myRadius));
    theParameters.push_back(Parameter::MakeDouble("Height", myHeight));
}

bool CylinderFeature::ApplyParameter(const Parameter& theParameter)
{
    return ReadDouble(theParameter, "Radius", myRadius)
        || ReadDouble(theParameter, "Height", myHeight);
}

// ---- Sphere ----

SphereFeature::SphereFeature(double theRadius)
    : myRadius(theRadius)
{
}

std::unique_ptr<Feature> SphereFeature::Clone() const
{
    auto copy = std::make_unique<SphereFeature>(myRadius);
    CopyPrimitiveTo(*copy);
    return copy;
}

bool SphereFeature::MakeShape(TopoDS_Shape& theShape, std::string& theError) const
{
    if (myRadius < kMinSize) {
        theError = "radius must be greater than zero";
        return false;
    }

    BRepPrimAPI_MakeSphere maker(Axis(), myRadius);
    maker.Build();
    if (!maker.IsDone()) {
        theError = "could not build sphere";
        return false;
    }
    theShape = maker.Shape();
    return true;
}

void SphereFeature::AppendParameters(std::vector<Parameter>& theParameters) const
{
    theParameters.push_back(Parameter::MakeDouble("Radius", myRadius));
}

bool SphereFeature::ApplyParameter(const Parameter& theParameter)
{
    return ReadDouble(theParameter, "Radius", myRadius);
}

// ---- Cone ----

ConeFeature::ConeFeature(double theBottomRadius, double theTopRadius, double theHeight)
    : myBottomRadius(theBottomRadius), myTopRadius(theTopRadius), myHeight(theHeight)
{
}

std::unique_ptr<Feature> ConeFeature::Clone() const
{
    auto copy = std::make_unique<ConeFeature>(myBottomRadius, myTopRadius, myHeight);
    CopyPrimitiveTo(*copy);
    return copy;
}

bool ConeFeature::MakeShape(TopoDS_Shape& theShape, std::string& theError) const
{
    if (myBottomRadius < 0.0 || myTopRadius < 0.0) {
        theError = "radii cannot be negative";
        return false;
    }
    // One radius may be zero -- that's a pointed cone -- but not both.
    if (myBottomRadius < kMinSize && myTopRadius < kMinSize) {
        theError = "bottom or top radius must be greater than zero";
        return false;
    }
    if (std::fabs(myBottomRadius - myTopRadius) < kMinSize) {
        theError = "bottom and top radius are equal -- use a cylinder";
        return false;
    }
    if (std::fabs(myHeight) < kMinSize) {
        theError = "height must be greater than zero";
        return false;
    }

    BRepPrimAPI_MakeCone maker(AxisForHeight(Origin(), myHeight),
                               myBottomRadius,
                               myTopRadius,
                               std::fabs(myHeight));
    maker.Build();
    if (!maker.IsDone()) {
        theError = "could not build cone";
        return false;
    }
    theShape = maker.Shape();
    return true;
}

void ConeFeature::AppendParameters(std::vector<Parameter>& theParameters) const
{
    theParameters.push_back(Parameter::MakeDouble("Bottom Radius", myBottomRadius));
    theParameters.push_back(Parameter::MakeDouble("Top Radius", myTopRadius));
    theParameters.push_back(Parameter::MakeDouble("Height", myHeight));
}

bool ConeFeature::ApplyParameter(const Parameter& theParameter)
{
    return ReadDouble(theParameter, "Bottom Radius", myBottomRadius)
        || ReadDouble(theParameter, "Top Radius", myTopRadius)
        || ReadDouble(theParameter, "Height", myHeight);
}

// ---- Torus ----

TorusFeature::TorusFeature(double theMajorRadius, double theMinorRadius)
    : myMajorRadius(theMajorRadius), myMinorRadius(theMinorRadius)
{
}

std::unique_ptr<Feature> TorusFeature::Clone() const
{
    auto copy = std::make_unique<TorusFeature>(myMajorRadius, myMinorRadius);
    CopyPrimitiveTo(*copy);
    return copy;
}

bool TorusFeature::MakeShape(TopoDS_Shape& theShape, std::string& theError) const
{
    if (myMajorRadius < kMinSize) {
        theError = "major radius must be greater than zero";
        return false;
    }
    if (myMinorRadius < kMinSize) {
        theError = "minor radius must be greater than zero";
        return false;
    }
    // Equal radii give a torus that closes on its own axis, and a larger
    // minor radius makes it self-intersect: both produce invalid solids
    // that only show up as failures much further down the timeline.
    if (myMinorRadius >= myMajorRadius) {
        theError = "minor radius must be smaller than the major radius";
        return false;
    }

    BRepPrimAPI_MakeTorus maker(Axis(), myMajorRadius, myMinorRadius);
    maker.Build();
    if (!maker.IsDone()) {
        theError = "could not build torus";
        return false;
    }
    theShape = maker.Shape();
    return true;
}

void TorusFeature::AppendParameters(std::vector<Parameter>& theParameters) const
{
    theParameters.push_back(Parameter::MakeDouble("Major Radius", myMajorRadius));
    theParameters.push_back(Parameter::MakeDouble("Minor Radius", myMinorRadius));
}

bool TorusFeature::ApplyParameter(const Parameter& theParameter)
{
    return ReadDouble(theParameter, "Major Radius", myMajorRadius)
        || ReadDouble(theParameter, "Minor Radius", myMinorRadius);
}

} // namespace lcad
