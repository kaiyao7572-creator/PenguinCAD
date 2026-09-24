#include "gizmos/TransformFeature.h"

#include "core/Body.h"
#include "features/FeatureUtils.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

// What "the same body" means when a pick is resolved against the input
// shape. BodyTable's numbers, by way of CombineFeature: a pick has to
// keep resolving for exactly as long as the body's NAME would have
// survived, so the rule belongs to the pick rather than to whichever
// feature is holding it. The copies of this rule -- here, in
// CombineFeature.cpp and in PatternFeatures.cpp -- have to stay in step.
constexpr double kSameSizeRatio   = 0.02;
constexpr double kSameCentre      = 1.0;  // mm
constexpr double kAmbiguityMargin = 4.0;
constexpr double kMinVolume       = 1.0e-9;

// A scale is a similarity ratio, so the sane band is symmetric about one.
// The ends are guards against a mistyped exponent rather than a modelling
// limit: at a millionth the kernel's own tolerance swallows the body, and
// at a million the bounding box leaves anything else in the document
// behind by a factor nobody is going to find on screen.
constexpr double kMinScale = 1.0e-6;
constexpr double kMaxScale = 1.0e6;

std::string OcctMessage(const Standard_Failure& theFailure, const std::string& theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message) : theFallback;
}

bool IsUsableScale(double theFactor)
{
    return std::isfinite(theFactor) && theFactor >= kMinScale && theFactor <= kMaxScale;
}

// Size and position of a solid, measured the way Body measures them so
// the two agree. The magnitude of the volume, not its sign: an imported
// solid can arrive reversed, and a sign flip upstream must not make a
// body unrecognisable when it is plainly the same size in the same place.
bool VolumeAndCentre(const TopoDS_Shape& theShape, double& theVolume, gp_Pnt& theCentre)
{
    if (theShape.IsNull() || !HasSolid(theShape)) {
        return false;
    }
    try {
        GProp_GProps properties;
        BRepGProp::VolumeProperties(theShape, properties);
        theVolume = std::fabs(properties.Mass());
        theCentre = properties.CentreOfMass();
        return theVolume > kMinVolume;
    } catch (const Standard_Failure&) {
        return false;
    }
}

std::string LostBodyMessage(const CombineBodyRef& theRef, BodyMatch theOutcome)
{
    const std::string what = "the body '" + theRef.name + "'";
    if (theOutcome == BodyMatch::Ambiguous) {
        return what + " now matches two bodies equally well -- re-pick it";
    }
    return what + " is no longer in the model -- re-pick it";
}

} // namespace

BodyMatch FindBodyForRef(const std::vector<TopoDS_Shape>& theShapes,
                         const CombineBodyRef&            theRef,
                         std::size_t&                     theIndex)
{
    if (theRef.IsNull()) {
        return BodyMatch::Missing;
    }

    std::size_t winner   = theShapes.size();
    double      best     = std::numeric_limits<double>::max();
    double      runnerUp = std::numeric_limits<double>::max();

    for (std::size_t i = 0; i < theShapes.size(); ++i) {
        double volume = 0.0;
        gp_Pnt centre;
        if (!VolumeAndCentre(theShapes[i], volume, centre)) {
            continue;
        }

        const double wanted = std::fabs(theRef.volume);
        const double scale  = std::max({volume, wanted, kMinVolume});
        if (std::fabs(volume - wanted) / scale > kSameSizeRatio) {
            continue;
        }

        const double distance = centre.Distance(theRef.centre);
        if (distance > kSameCentre) {
            continue;
        }

        if (distance < best) {
            runnerUp = best;
            best = distance;
            winner = i;
        } else if (distance < runnerUp) {
            runnerUp = distance;
        }
    }

    if (winner == theShapes.size()) {
        return BodyMatch::Missing;
    }
    // An exact match still wins outright -- best is then zero and nothing
    // clears the bar -- which is what keeps an untouched body resolving
    // even with a near-twin beside it.
    if (runnerUp < std::numeric_limits<double>::max()
        && runnerUp < best * kAmbiguityMargin + kSameCentre * 1.0e-3) {
        return BodyMatch::Ambiguous;
    }

    theIndex = winner;
    return BodyMatch::Found;
}

TransformFeature::TransformFeature(double theTx, double theTy, double theTz,
                                   double theRxDeg, double theRyDeg, double theRzDeg,
                                   const gp_Pnt& thePivot)
    : myTx(theTx), myTy(theTy), myTz(theTz),
      myRxDeg(theRxDeg), myRyDeg(theRyDeg), myRzDeg(theRzDeg),
      myPivot(thePivot)
{
}

bool TransformFeature::SetScale(double theFactor)
{
    if (!IsUsableScale(theFactor)) {
        return false;
    }
    myScale = theFactor;
    return true;
}

gp_Trsf TransformFeature::Trsf() const
{
    gp_Trsf scale;
    if (IsUsableScale(myScale)) {
        scale.SetScale(myPivot, myScale);
    }
    // An unusable factor never reaches here: Compute() refuses it first,
    // loudly. Leaving the scale out rather than handing gp_Trsf a zero is
    // only so a caller who asks for Trsf() anyway gets a transform it can
    // multiply, not a kernel exception out of a const accessor.

    gp_Trsf rotateX;
    rotateX.SetRotation(gp_Ax1(myPivot, gp_Dir(1.0, 0.0, 0.0)), myRxDeg * kDegToRad);

    gp_Trsf rotateY;
    rotateY.SetRotation(gp_Ax1(myPivot, gp_Dir(0.0, 1.0, 0.0)), myRyDeg * kDegToRad);

    gp_Trsf rotateZ;
    rotateZ.SetRotation(gp_Ax1(myPivot, gp_Dir(0.0, 0.0, 1.0)), myRzDeg * kDegToRad);

    gp_Trsf translate;
    translate.SetTranslation(gp_Vec(myTx, myTy, myTz));

    // ORDER, right to left, which is the order a point actually travels:
    // SCALE about the pivot, then rotate about the pivot -- X, then Y,
    // then Z -- then TRANSLATE. Four reasons it is this way round and not
    // another:
    //
    // 1. gp_Trsf's rotation and scale are both ABOUT THE PIVOT, so the
    //    "translate to the origin, act, translate back" sandwich is
    //    already inside them. Doing it again by hand is the classic way
    //    to rotate a body into next week.
    // 2. Translation goes LAST, so "Translation X = 10" moves the body
    //    10mm whatever the scale is. Scaling a translation that had
    //    already been applied would make the same typed number mean a
    //    different distance -- the silent kind of wrong.
    // 3. A UNIFORM scale about a point commutes with any rotation about
    //    that same point, so scale's position among the three rotations
    //    is free; it is written first because that is the order the
    //    properties panel would read if you worked down its rows. (The
    //    harness asserts the commutation rather than trusting it.)
    // 4. The three rotations do NOT commute with each other. X-then-Y-
    //    then-Z is the order the dialog's fields are laid out top to
    //    bottom, which is the only defensible tie-break. A gizmo drag only
    //    ever has ONE of them nonzero, so for that path it cannot matter.
    return translate.Multiplied(rotateZ.Multiplied(rotateY.Multiplied(rotateX.Multiplied(scale))));
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
    if (!IsUsableScale(myScale)) {
        // Zero collapses the body, negative turns it inside out, and NaN
        // poisons every coordinate downstream. None of them is a size.
        theError = "a scale of " + FormatNumber(myScale)
                 + " is not a size -- it has to be greater than zero";
        return false;
    }

    try {
        const gp_Trsf trsf = Trsf();

        if (myTarget.IsNull()) {
            // No pick: the whole upstream shape moves, which is what the
            // numeric dialog means by Move.
            BRepBuilderAPI_Transform mover(theInput, trsf);
            if (!mover.IsDone() || mover.Shape().IsNull()) {
                theError = "transform did not complete";
                return false;
            }
            theOutput = mover.Shape();
            return true;
        }

        std::vector<TopoDS_Shape> bodies = SplitIntoBodies(theInput);
        std::size_t index = 0;
        const BodyMatch outcome = FindBodyForRef(bodies, myTarget, index);
        if (outcome != BodyMatch::Found) {
            // Falling back to moving everything would be a model that
            // quietly changed shape, which is exactly what the pick
            // exists to prevent.
            theError = LostBodyMessage(myTarget, outcome);
            return false;
        }

        // theCopyGeom is true because the transform may be a similarity
        // rather than a rigid motion: a scaled shape has to have its
        // curves and surfaces rebuilt, and the bodies left alone still
        // share the input's geometry, so a copy here does not cost the
        // whole document.
        BRepBuilderAPI_Transform mover(bodies[index], trsf, Standard_True);
        if (!mover.IsDone() || mover.Shape().IsNull()) {
            theError = "could not transform '" + myTarget.name + "'";
            return false;
        }
        bodies[index] = mover.Shape();

        // One body in, one body out: MakeCompoundOf hands back the shape
        // itself rather than wrapping it, so a single-body document does
        // not gain a compound it never had.
        theOutput = MakeCompoundOf(bodies);
        if (IsEmptyShape(theOutput)) {
            theError = "the transform produced no geometry";
            return false;
        }
        return true;
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "OCCT error");
        return false;
    }
}

std::unique_ptr<Feature> TransformFeature::Clone() const
{
    auto copy = std::make_unique<TransformFeature>(myTx, myTy, myTz, myRxDeg, myRyDeg, myRzDeg, myPivot);
    copy->myScale  = myScale;   // straight through, not SetScale: a clone
    copy->myTarget = myTarget;  // has to reproduce even a refused value
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

    // Unitless: the properties panel reads the empty unit as "a bare
    // number" and stops trying to parse "2" as 2mm.
    Parameter scale = Parameter::MakeDouble("Scale", myScale, "");
    scale.minimum = kMinScale;
    scale.maximum = kMaxScale;
    parameters.push_back(scale);

    // The pivot is not decoration: a rotation is meaningless without the
    // point it turned about, and a gizmo drag captures that point from
    // the body rather than from anything the user typed. Showing it is
    // what makes the drag reproducible by hand afterwards.
    parameters.push_back(Parameter::MakeDouble("Pivot X", myPivot.X(), "mm"));
    parameters.push_back(Parameter::MakeDouble("Pivot Y", myPivot.Y(), "mm"));
    parameters.push_back(Parameter::MakeDouble("Pivot Z", myPivot.Z(), "mm"));

    // The pick rides a plain string row, the same bargain Combine's
    // bodies and a press/pull's face make: which body this was built on
    // is inspectable, and clearing it (back to "the whole model") needs
    // no bespoke widget.
    parameters.push_back(Parameter::MakeString("Body", myTarget.Encode()));
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
    if (theParameter.name == "Scale")         { return SetScale(theParameter.doubleValue); }

    if (theParameter.name == "Pivot X") {
        myPivot = gp_Pnt(theParameter.doubleValue, myPivot.Y(), myPivot.Z());
        return true;
    }
    if (theParameter.name == "Pivot Y") {
        myPivot = gp_Pnt(myPivot.X(), theParameter.doubleValue, myPivot.Z());
        return true;
    }
    if (theParameter.name == "Pivot Z") {
        myPivot = gp_Pnt(myPivot.X(), myPivot.Y(), theParameter.doubleValue);
        return true;
    }

    if (theParameter.name == "Body") {
        if (theParameter.stringValue.empty()) {
            // Emptying the row is a real choice, not a mistake: it is how
            // a drag on one body is widened back to "move the whole
            // model", which is what this feature meant before bodies were
            // pickable. Text that is present but unreadable is refused,
            // the way Combine refuses it, rather than quietly forgetting
            // which body was picked.
            myTarget = CombineBodyRef();
            return true;
        }
        CombineBodyRef target;
        if (!CombineBodyRef::Decode(theParameter.stringValue, target)) {
            return false;
        }
        myTarget = target;
        return true;
    }
    return false;
}

} // namespace lcad
