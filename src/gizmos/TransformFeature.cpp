#include "gizmos/TransformFeature.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

} // namespace

TransformFeature::TransformFeature(double theTx, double theTy, double theTz,
                                   double theRxDeg, double theRyDeg, double theRzDeg,
                                   const gp_Pnt& thePivot)
    : myTx(theTx), myTy(theTy), myTz(theTz),
      myRxDeg(theRxDeg), myRyDeg(theRyDeg), myRzDeg(theRzDeg),
      myPivot(thePivot)
{
}

gp_Trsf TransformFeature::Trsf() const
{
    gp_Trsf rotateX;
    rotateX.SetRotation(gp_Ax1(myPivot, gp_Dir(1.0, 0.0, 0.0)), myRxDeg * kDegToRad);

    gp_Trsf rotateY;
    rotateY.SetRotation(gp_Ax1(myPivot, gp_Dir(0.0, 1.0, 0.0)), myRyDeg * kDegToRad);

    gp_Trsf rotateZ;
    rotateZ.SetRotation(gp_Ax1(myPivot, gp_Dir(0.0, 0.0, 1.0)), myRzDeg * kDegToRad);

    gp_Trsf translate;
    translate.SetTranslation(gp_Vec(myTx, myTy, myTz));

    // Rotate about the pivot -- X, then Y, then Z -- and translate last,
    // so a combined move+rotate typed into the dialog reads the way its
    // fields are laid out top to bottom. A gizmo-driven drag only ever
    // has one of the six components nonzero, so this order never actually
    // matters for that path.
    return translate.Multiplied(rotateZ.Multiplied(rotateY.Multiplied(rotateX)));
}

bool TransformFeature::Compute(const ComputeContext& theContext,
                               const TopoDS_Shape&   theInput,
                               TopoDS_Shape&         theOutput,
                               std::string&          theError)
{
    (void)theContext;

    if (theInput.IsNull()) {
        theError = "nothing upstream to move";
        return false;
    }

    try {
        BRepBuilderAPI_Transform mover(theInput, Trsf());
        if (!mover.IsDone()) {
            theError = "transform did not complete";
            return false;
        }
        theOutput = mover.Shape();
        return true;
    } catch (const Standard_Failure& e) {
        theError = e.GetMessageString() ? e.GetMessageString() : "OCCT error";
        return false;
    }
}

std::unique_ptr<Feature> TransformFeature::Clone() const
{
    auto copy = std::make_unique<TransformFeature>(myTx, myTy, myTz, myRxDeg, myRyDeg, myRzDeg, myPivot);
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> TransformFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeDouble("Translation X", myTx, "mm"));
    parameters.push_back(Parameter::MakeDouble("Translation Y", myTy, "mm"));
    parameters.push_back(Parameter::MakeDouble("Translation Z", myTz, "mm"));
    parameters.push_back(Parameter::MakeDouble("Rotation X", myRxDeg, "deg"));
    parameters.push_back(Parameter::MakeDouble("Rotation Y", myRyDeg, "deg"));
    parameters.push_back(Parameter::MakeDouble("Rotation Z", myRzDeg, "deg"));
    return parameters;
}

bool TransformFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Translation X") { myTx = theParameter.doubleValue; return true; }
    if (theParameter.name == "Translation Y") { myTy = theParameter.doubleValue; return true; }
    if (theParameter.name == "Translation Z") { myTz = theParameter.doubleValue; return true; }
    if (theParameter.name == "Rotation X")    { myRxDeg = theParameter.doubleValue; return true; }
    if (theParameter.name == "Rotation Y")    { myRyDeg = theParameter.doubleValue; return true; }
    if (theParameter.name == "Rotation Z")    { myRzDeg = theParameter.doubleValue; return true; }
    return false;
}

} // namespace lcad
