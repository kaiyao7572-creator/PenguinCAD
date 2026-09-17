#include "features/ProfileFeatures.h"

#include "core/Document.h"
#include "core/ProfileProvider.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <Standard_Failure.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Millimetres / degrees below this are a no-op sweep rather than a solid.
constexpr double kMinSweep = 1.0e-7;

std::string OcctMessage(const Standard_Failure& theFailure, const std::string& theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message) : theFallback;
}

void DedupeProfileRefs(std::vector<ProfileRef>& theRefs)
{
    std::vector<ProfileRef> kept;
    kept.reserve(theRefs.size());
    for (const ProfileRef& ref : theRefs) {
        if (ref.IsNull()) {
            continue;
        }
        const bool seen = std::any_of(kept.begin(), kept.end(),
                                      [&ref](const ProfileRef& theKept) {
                                          return theKept.SameRegion(ref);
                                      });
        if (!seen) {
            kept.push_back(ref);
        }
    }
    theRefs = std::move(kept);
}

// How many refs an encoded string CLAIMS to hold. DecodeProfileRefs drops
// the segments it cannot parse, so without this a hand-edited row with one
// mangled ref would come back as a smaller selection and the solid would
// quietly shrink -- the same sin as widening it, through the other door.
std::size_t CountProfileRefSegments(const std::string& theText)
{
    std::size_t count = 0;
    std::istringstream parts(theText);
    std::string part;
    while (std::getline(parts, part, ';')) {
        if (!part.empty()) {
            ++count;
        }
    }
    return count;
}

std::string ToLower(const std::string& theText)
{
    std::string lower = theText;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char theChar) {
        return static_cast<char>(std::tolower(theChar));
    });
    return lower;
}

} // namespace

// ---- ProfileFeature ----

void ProfileFeature::SetProfiles(std::vector<ProfileRef> theProfiles)
{
    DedupeProfileRefs(theProfiles);
    myProfiles = std::move(theProfiles);
}

bool ProfileFeature::ResolveProfile(const ComputeContext&     theContext,
                                    std::vector<TopoDS_Face>& theFaces,
                                    gp_Pln&                   thePlane,
                                    std::string&              theError) const
{
    if (mySketchName.empty()) {
        theError = "no sketch selected";
        return false;
    }

    // FindFeature only looks upstream, so a sketch dragged below this
    // feature in the timeline reads as missing -- say so rather than
    // just "not found".
    Feature* feature = theContext.FindFeature(mySketchName);
    if (feature == nullptr) {
        theError = "sketch '" + mySketchName + "' is not earlier in the timeline";
        return false;
    }

    ProfileProvider* provider = AsProfileProvider(feature);
    if (provider == nullptr) {
        theError = "'" + mySketchName + "' is not a sketch";
        return false;
    }

    try {
        thePlane = provider->ProfilePlane();
        if (myProfiles.empty()) {
            theFaces = provider->ProfileFaces();
        } else {
            theFaces.clear();
            theFaces.reserve(myProfiles.size());
            for (const ProfileRef& ref : myProfiles) {
                TopoDS_Face face;
                if (!provider->FindProfile(ref, face)) {
                    // Sweeping the refs that still resolve and quietly
                    // forgetting this one would change the solid behind the
                    // user's back -- the model would keep rebuilding, just
                    // not into the shape they designed. Fail instead and
                    // let them re-pick.
                    //
                    // Deciding WHICH region a ref still names is
                    // FindProfile's job and only its job: it is the one
                    // place that can weigh a changed boundary against the
                    // seed, and a second opinion here would either repeat
                    // that judgement or quietly disagree with it.
                    theError = "a profile in '" + mySketchName
                             + "' no longer exists -- re-select it";
                    return false;
                }
                theFaces.push_back(face);
            }
        }
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "could not read the sketch profile");
        return false;
    }

    theFaces.erase(std::remove_if(theFaces.begin(),
                                  theFaces.end(),
                                  [](const TopoDS_Face& theFace) { return theFace.IsNull(); }),
                   theFaces.end());

    if (theFaces.empty()) {
        theError = "sketch '" + mySketchName + "' has no closed profile";
        return false;
    }
    return true;
}

void ProfileFeature::AppendCommonParameters(std::vector<Parameter>& theParameters) const
{
    theParameters.push_back(Parameter::MakeString("Sketch", mySketchName));
    // Encoded refs ride the plain string row the properties panel already
    // knows how to show and edit, so the selection is inspectable and
    // clearable without a bespoke widget.
    theParameters.push_back(Parameter::MakeString("Profiles", EncodeProfileRefs(myProfiles)));
    theParameters.push_back(Parameter::MakeString("Operation", BooleanOpName(myOperation)));
}

bool ProfileFeature::ApplyCommonParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Sketch") {
        // Same reasoning as SetSketchName: refs from the old sketch would
        // resolve against entity ids that mean something else here.
        if (theParameter.stringValue != mySketchName) {
            myProfiles.clear();
        }
        mySketchName = theParameter.stringValue;
        return true;
    }
    if (theParameter.name == "Profiles") {
        // Empty is the documented way back to the whole sketch.
        if (theParameter.stringValue.empty()) {
            myProfiles.clear();
            return true;
        }
        std::vector<ProfileRef> refs = DecodeProfileRefs(theParameter.stringValue);
        if (refs.empty()) {
            // Text that decodes to nothing is a typo, not a request for
            // the whole sketch -- silently widening the feature to every
            // region would be a surprising way to answer a mistake.
            return false;
        }
        if (refs.size() != CountProfileRefSegments(theParameter.stringValue)) {
            // Some segments parsed and some did not. Taking the survivors
            // would narrow the feature just as silently as the empty case
            // would widen it, so refuse the string whole and leave the
            // stored selection exactly as it was.
            return false;
        }
        DedupeProfileRefs(refs);
        myProfiles = std::move(refs);
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

void ProfileFeature::CopyProfileTo(ProfileFeature& theOther) const
{
    theOther.mySketchName = mySketchName;
    // Undo restores the timeline from clones, so a field missed here is a
    // field the user loses the moment they press Ctrl+Z.
    theOther.myProfiles = myProfiles;
    theOther.myOperation = myOperation;
    CopyBaseTo(theOther);
}

// ---- Extrude ----

ExtrudeFeature::ExtrudeFeature(std::string theSketchName, double theDistance)
    : myDistance(theDistance)
{
    SetSketchName(std::move(theSketchName));
}

bool ExtrudeFeature::Compute(const ComputeContext& theContext,
                             const TopoDS_Shape&   theInput,
                             TopoDS_Shape&         theOutput,
                             std::string&          theError)
{
    if (std::fabs(myDistance) < kMinSweep) {
        theError = "distance must not be zero";
        return false;
    }

    std::vector<TopoDS_Face> faces;
    gp_Pln plane;
    if (!ResolveProfile(theContext, faces, plane, theError)) {
        return false;
    }

    TopoDS_Shape tool;
    try {
        gp_Vec sweep = gp_Vec(plane.Axis().Direction()) * myDistance;
        if (myIsReversed) {
            sweep.Reverse();
        }

        std::vector<TopoDS_Shape> solids;
        solids.reserve(faces.size());

        for (const TopoDS_Face& face : faces) {
            TopoDS_Shape profile = face;

            if (myIsSymmetric) {
                // Half the sweep backwards first, then the full length
                // forwards: that lands the profile in the middle of the
                // result, which is what "symmetric" means to the user.
                gp_Trsf shift;
                shift.SetTranslation(sweep * -0.5);
                BRepBuilderAPI_Transform transform(face, shift, Standard_True);
                if (!transform.IsDone()) {
                    theError = "could not centre the profile for a symmetric extrude";
                    return false;
                }
                profile = transform.Shape();
            }

            BRepPrimAPI_MakePrism prism(profile, sweep);
            prism.Build();
            if (!prism.IsDone()) {
                theError = "extrude failed for this profile";
                return false;
            }
            solids.push_back(prism.Shape());
        }

        tool = MakeCompoundOf(solids);
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "extrude failed");
        return false;
    }

    if (IsEmptyShape(tool)) {
        theError = "extrude produced no solid";
        return false;
    }

    return ApplyBooleanOperation(Operation(), theInput, tool, theOutput, theError);
}

std::unique_ptr<Feature> ExtrudeFeature::Clone() const
{
    auto copy = std::make_unique<ExtrudeFeature>();
    copy->myDistance = myDistance;
    copy->myIsReversed = myIsReversed;
    copy->myIsSymmetric = myIsSymmetric;
    CopyProfileTo(*copy);
    return copy;
}

std::vector<Parameter> ExtrudeFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeDouble("Distance", myDistance));
    parameters.push_back(Parameter::MakeBool("Reversed", myIsReversed));
    parameters.push_back(Parameter::MakeBool("Symmetric", myIsSymmetric));
    AppendCommonParameters(parameters);
    return parameters;
}

bool ExtrudeFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Distance") {
        myDistance = theParameter.doubleValue;
        return true;
    }
    if (theParameter.name == "Reversed") {
        myIsReversed = theParameter.boolValue;
        return true;
    }
    if (theParameter.name == "Symmetric") {
        myIsSymmetric = theParameter.boolValue;
        return true;
    }
    return ApplyCommonParameter(theParameter);
}

// ---- Revolve ----

const std::vector<std::string>& RevolveAxisNames()
{
    static const std::vector<std::string> names = {
        "Sketch X", "Sketch Y", "World X", "World Y", "World Z"};
    return names;
}

std::string RevolveAxisName(RevolveAxis theAxis)
{
    const std::vector<std::string>& names = RevolveAxisNames();
    const std::size_t index = static_cast<std::size_t>(theAxis);
    return index < names.size() ? names[index] : names.front();
}

RevolveAxis RevolveAxisFromInt(int theValue)
{
    if (theValue < 0 || theValue > static_cast<int>(RevolveAxis::WorldZ)) {
        return RevolveAxis::SketchY;
    }
    return static_cast<RevolveAxis>(theValue);
}

bool ParseRevolveAxis(const std::string& theText, RevolveAxis& theAxis)
{
    const std::string wanted = ToLower(theText);
    if (wanted.empty()) {
        return false;
    }

    const std::vector<std::string>& names = RevolveAxisNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::string candidate = ToLower(names[i]);
        if (candidate == wanted) {
            theAxis = RevolveAxisFromInt(static_cast<int>(i));
            return true;
        }
        candidate.erase(std::remove(candidate.begin(), candidate.end(), ' '), candidate.end());
        if (candidate == wanted) {
            theAxis = RevolveAxisFromInt(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

RevolveFeature::RevolveFeature(std::string theSketchName,
                               double      theAngleDegrees,
                               RevolveAxis theAxis)
    : myAngleDegrees(theAngleDegrees), myAxis(theAxis)
{
    SetSketchName(std::move(theSketchName));
}

gp_Ax1 RevolveFeature::AxisOf(const gp_Pln& thePlane) const
{
    switch (myAxis) {
        case RevolveAxis::SketchX:
            return thePlane.XAxis();
        case RevolveAxis::SketchY:
            return thePlane.YAxis();
        case RevolveAxis::WorldX:
            return gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0));
        case RevolveAxis::WorldY:
            return gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 1.0, 0.0));
        case RevolveAxis::WorldZ:
            return gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0));
    }
    return thePlane.YAxis();
}

bool RevolveFeature::Compute(const ComputeContext& theContext,
                             const TopoDS_Shape&   theInput,
                             TopoDS_Shape&         theOutput,
                             std::string&          theError)
{
    if (myAngleDegrees < kMinSweep || myAngleDegrees > 360.0 + kMinSweep) {
        theError = "angle must be between 0 and 360 degrees";
        return false;
    }

    std::vector<TopoDS_Face> faces;
    gp_Pln plane;
    if (!ResolveProfile(theContext, faces, plane, theError)) {
        return false;
    }

    TopoDS_Shape tool;
    try {
        gp_Ax1 axis = AxisOf(plane);
        if (myIsReversed) {
            // Turning the axis round is safer than a negative angle:
            // BRepSweep_Revol treats the angle as a magnitude.
            axis = axis.Reversed();
        }
        const double angle = std::min(myAngleDegrees, 360.0) * kPi / 180.0;

        std::vector<TopoDS_Shape> solids;
        solids.reserve(faces.size());

        for (const TopoDS_Face& face : faces) {
            BRepPrimAPI_MakeRevol revol(face, axis, angle);
            revol.Build();
            if (!revol.IsDone()) {
                theError = "revolve failed -- the axis may cross the profile";
                return false;
            }
            solids.push_back(revol.Shape());
        }

        tool = MakeCompoundOf(solids);
    } catch (const Standard_Failure& failure) {
        // Much the most common cause is an axis running through the
        // middle of the profile, which sweeps the shape through itself.
        theError = OcctMessage(failure, "revolve failed")
                 + " (check that the " + RevolveAxisName(myAxis)
                 + " axis does not cross the profile)";
        return false;
    }

    if (IsEmptyShape(tool)) {
        theError = "revolve produced no solid";
        return false;
    }

    return ApplyBooleanOperation(Operation(), theInput, tool, theOutput, theError);
}

std::unique_ptr<Feature> RevolveFeature::Clone() const
{
    auto copy = std::make_unique<RevolveFeature>();
    copy->myAngleDegrees = myAngleDegrees;
    copy->myAxis = myAxis;
    copy->myIsReversed = myIsReversed;
    CopyProfileTo(*copy);
    return copy;
}

std::vector<Parameter> RevolveFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeDouble("Angle", myAngleDegrees, "deg"));
    parameters.push_back(Parameter::MakeString("Axis", RevolveAxisName(myAxis)));
    parameters.push_back(Parameter::MakeBool("Reversed", myIsReversed));
    AppendCommonParameters(parameters);
    return parameters;
}

bool RevolveFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Angle") {
        myAngleDegrees = theParameter.doubleValue;
        return true;
    }
    if (theParameter.name == "Axis") {
        RevolveAxis axis = myAxis;
        if (!ParseRevolveAxis(theParameter.stringValue, axis)) {
            return false;
        }
        myAxis = axis;
        return true;
    }
    if (theParameter.name == "Reversed") {
        myIsReversed = theParameter.boolValue;
        return true;
    }
    return ApplyCommonParameter(theParameter);
}

} // namespace lcad
