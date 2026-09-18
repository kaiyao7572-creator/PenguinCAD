#include "features/PressPullFeature.h"

#include "features/FeatureUtils.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <cmath>

namespace lcad {

namespace {

// Below this the move is a no-op rather than a feature, and a zero-length
// prism is a degenerate shape the booleans would choke on.
constexpr double kMinDistance = 1.0e-7;

std::string OcctMessage(const Standard_Failure& theFailure, const std::string& theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message) : theFallback;
}

// The direction that points OUT of the solid.
//
// A face's surface normal points out of its own parameterisation, not out
// of the body: OCCT records which of the two the solid actually uses in
// the face's orientation. Ignoring that pushes half the faces of a box the
// wrong way, and the half depends on how the box was built -- so it would
// look like an intermittent bug rather than a missing line.
bool OutwardNormal(const TopoDS_Face& theFace, gp_Dir& theResult, std::string& theError)
{
    try {
        BRepAdaptor_Surface surface(theFace);
        if (surface.GetType() != GeomAbs_Plane) {
            theError = "press/pull needs a flat face";
            return false;
        }
        gp_Dir normal = surface.Plane().Axis().Direction();
        if (theFace.Orientation() == TopAbs_REVERSED) {
            normal.Reverse();
        }
        theResult = normal;
        return true;
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "could not read the face");
        return false;
    }
}

} // namespace

PressPullFeature::PressPullFeature(GeometryRef theFace, double theDistance)
    : myFace(std::move(theFace))
    , myDistance(theDistance)
{
}

bool PressPullFeature::Compute(const ComputeContext& theContext,
                               const TopoDS_Shape&   theInput,
                               TopoDS_Shape&         theOutput,
                               std::string&          theError)
{
    (void)theContext;

    if (std::fabs(myDistance) < kMinDistance) {
        theError = "distance must not be zero";
        return false;
    }
    if (myFace.IsNull() || myFace.type != EntityType::BRepFace) {
        theError = "no face selected";
        return false;
    }
    if (IsEmptyShape(theInput)) {
        theError = "nothing to press or pull -- there is no body yet";
        return false;
    }

    // Resolved against the INPUT shape, which is the model as it stands at
    // this point in the timeline. Refusing beats guessing: a press/pull
    // that silently moved to a neighbouring face after an upstream edit
    // would be a wrong model nobody notices.
    TopoDS_Shape resolved;
    if (!ResolveGeometryRefInShape(theInput, myFace, resolved)) {
        theError = "the face this was built on no longer exists -- re-select it";
        return false;
    }

    try {
        const TopoDS_Face face = TopoDS::Face(resolved);
        gp_Dir normal;
        if (!OutwardNormal(face, normal, theError)) {
            return false;
        }

        BRepPrimAPI_MakePrism prism(face, gp_Vec(normal) * myDistance);
        prism.Build();
        if (!prism.IsDone()) {
            theError = "could not sweep that face";
            return false;
        }

        // Out adds, in removes. One signed distance rather than a
        // direction plus a mode, because that is how the drag feels.
        const BooleanOp operation = myDistance > 0.0 ? BooleanOp::Join : BooleanOp::Cut;
        return ApplyBooleanOperation(operation, theInput, prism.Shape(), theOutput, theError);
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "press/pull failed");
        return false;
    }
}

std::unique_ptr<Feature> PressPullFeature::Clone() const
{
    auto copy = std::make_unique<PressPullFeature>();
    copy->myFace = myFace;
    copy->myDistance = myDistance;
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> PressPullFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeDouble("Distance", myDistance));
    // The face rides a plain string row, so which face this was built on
    // is inspectable without a new widget -- the same bargain a profile
    // reference makes.
    parameters.push_back(Parameter::MakeString("Face", myFace.Encode()));
    return parameters;
}

bool PressPullFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Distance") {
        myDistance = theParameter.doubleValue;
        return true;
    }
    if (theParameter.name == "Face") {
        GeometryRef parsed;
        if (!GeometryRef::Decode(theParameter.stringValue, parsed)) {
            return false;  // refuse rather than quietly clearing the face
        }
        myFace = parsed;
        return true;
    }
    return false;
}

} // namespace lcad
