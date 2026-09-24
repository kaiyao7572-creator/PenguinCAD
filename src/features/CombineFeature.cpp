#include "features/CombineFeature.h"

#include "core/Body.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <sstream>

namespace lcad {

namespace {

// What "the same body across a rebuild" means, taken from BodyTable: the
// volume has to agree to within this fraction and the centre to within
// this distance. Deliberately the body table's own numbers rather than new
// ones, so a pick keeps resolving for exactly as long as its NAME would
// have survived -- one rule, in one place, whichever end asks.
constexpr double kSameSizeRatio = 0.02;
constexpr double kSameCentre    = 1.0;  // mm

// The runner-up has to be this much further off than the winner, or the
// reference does not actually pick one of the two out. Two 20mm cubes half
// a millimetre apart are not told apart by a signature, and combining the
// wrong one is exactly the kind of wrong model nobody notices.
constexpr double kAmbiguityMargin = 4.0;

// Below this there is no volume to speak of, so nothing to combine.
constexpr double kMinVolume = 1.0e-9;

std::string OcctMessage(const Standard_Failure& theFailure, const std::string& theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message) : theFallback;
}

// Size and position of a solid, measured the same way Body does it so the
// two agree. The magnitude of the volume, not its sign: an imported solid
// can come in reversed, and a sign flip somewhere upstream must not make a
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

double SolidVolumeOf(const TopoDS_Shape& theShape)
{
    double volume = 0.0;
    gp_Pnt centre;
    return VolumeAndCentre(theShape, volume, centre) ? volume : 0.0;
}

enum class Resolution
{
    Found,
    Missing,
    Ambiguous
};

// Find the one body of theBodies that theRef names.
//
// Against the bodies of the INPUT shape, which is the model as it stands
// at this point in the timeline. Refusing beats guessing, the same bargain
// GeometryRef and ProfileRef make: one re-pick is cheap, a combine that
// quietly moved to the body next door is not.
Resolution ResolveBodyRef(const std::vector<TopoDS_Shape>& theBodies,
                          const CombineBodyRef&            theRef,
                          std::size_t&                     theIndex)
{
    if (theRef.IsNull()) {
        return Resolution::Missing;
    }

    std::size_t winner   = theBodies.size();
    double      best     = std::numeric_limits<double>::max();
    double      runnerUp = std::numeric_limits<double>::max();

    for (std::size_t i = 0; i < theBodies.size(); ++i) {
        double volume = 0.0;
        gp_Pnt centre;
        if (!VolumeAndCentre(theBodies[i], volume, centre)) {
            continue;
        }

        const double wanted = std::fabs(theRef.volume);
        const double scale = std::max({volume, wanted, 1.0e-9});
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

    if (winner == theBodies.size()) {
        return Resolution::Missing;
    }
    // An exact match still wins outright -- best is then zero and nothing
    // clears the bar -- which is what keeps an untouched body resolving
    // even with a near-twin beside it.
    if (runnerUp < std::numeric_limits<double>::max()
        && runnerUp < best * kAmbiguityMargin + kSameCentre * 1.0e-3) {
        return Resolution::Ambiguous;
    }

    theIndex = winner;
    return Resolution::Found;
}

std::string LostBodyMessage(const std::string&    theRole,
                            const CombineBodyRef& theRef,
                            Resolution            theOutcome)
{
    const std::string what = "the " + theRole + " body '" + theRef.name + "'";
    if (theOutcome == Resolution::Ambiguous) {
        return what + " now matches two bodies equally well -- re-pick it";
    }
    return what + " is no longer in the model -- re-pick it";
}

// Which tool a kernel message is about. With a single tool the name adds
// nothing the dialog did not already say.
std::string ToolPrefix(const std::vector<CombineBodyRef>& theTools, std::size_t theIndex)
{
    if (theTools.size() <= 1 || theIndex >= theTools.size()) {
        return std::string();
    }
    return "'" + theTools[theIndex].name + "': ";
}

std::vector<CombineBodyRef> WithoutNullsOrDuplicates(std::vector<CombineBodyRef> theRefs)
{
    std::vector<CombineBodyRef> kept;
    kept.reserve(theRefs.size());
    for (CombineBodyRef& ref : theRefs) {
        if (ref.IsNull()) {
            continue;
        }
        bool seen = false;
        for (const CombineBodyRef& already : kept) {
            seen = seen || already.name == ref.name;
        }
        if (!seen) {
            kept.push_back(std::move(ref));
        }
    }
    return kept;
}

} // namespace

// ---- CombineBodyRef ----

std::string CombineBodyRef::Encode() const
{
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "%s|%.10g,%.10g,%.10g|%.10g",
                  name.c_str(), centre.X(), centre.Y(), centre.Z(), volume);
    return std::string(buffer);
}

bool CombineBodyRef::Decode(const std::string& theText, CombineBodyRef& theResult)
{
    std::vector<std::string> parts;
    std::istringstream stream(theText);
    std::string part;
    while (std::getline(stream, part, '|')) {
        parts.push_back(part);
    }
    if (parts.size() != 3 || parts[0].empty()) {
        return false;
    }

    CombineBodyRef parsed;
    parsed.name = parts[0];

    double x = 0.0, y = 0.0, z = 0.0;
    if (std::sscanf(parts[1].c_str(), "%lf,%lf,%lf", &x, &y, &z) != 3) {
        return false;
    }
    parsed.centre = gp_Pnt(x, y, z);

    try {
        parsed.volume = std::stod(parts[2]);
    } catch (const std::exception&) {
        return false;
    }

    if (parsed.IsNull()) {
        return false;
    }
    theResult = parsed;
    return true;
}

std::string EncodeCombineBodyRefs(const std::vector<CombineBodyRef>& theRefs)
{
    std::string text;
    for (const CombineBodyRef& ref : theRefs) {
        if (ref.IsNull()) {
            continue;
        }
        if (!text.empty()) {
            text += ';';
        }
        text += ref.Encode();
    }
    return text;
}

std::vector<CombineBodyRef> DecodeCombineBodyRefs(const std::string& theText)
{
    std::vector<CombineBodyRef> refs;
    std::istringstream stream(theText);
    std::string part;
    while (std::getline(stream, part, ';')) {
        CombineBodyRef ref;
        if (CombineBodyRef::Decode(part, ref)) {
            refs.push_back(ref);
        }
    }
    return refs;
}

std::size_t CountCombineBodyRefSegments(const std::string& theText)
{
    if (theText.empty()) {
        return 0;
    }
    std::size_t count = 1;
    for (const char character : theText) {
        if (character == ';') {
            ++count;
        }
    }
    return count;
}

CombineBodyRef MakeCombineBodyRef(const Body& theBody)
{
    CombineBodyRef ref;
    double volume = 0.0;
    gp_Pnt centre;
    if (!theBody.IsSolid() || !VolumeAndCentre(theBody.Shape(), volume, centre)) {
        return ref;
    }
    ref.name = theBody.Name();
    ref.centre = centre;
    ref.volume = volume;
    return ref;
}

// ---- operations ----

const std::vector<std::string>& CombineOperationNames()
{
    static const std::vector<std::string> names = {BooleanOpName(BooleanOp::Join),
                                                  BooleanOpName(BooleanOp::Cut),
                                                  BooleanOpName(BooleanOp::Intersect)};
    return names;
}

bool IsCombineOperation(BooleanOp theOperation)
{
    return theOperation == BooleanOp::Join || theOperation == BooleanOp::Cut
           || theOperation == BooleanOp::Intersect;
}

BooleanOp CombineOperationFromChoice(int theChoice)
{
    switch (theChoice) {
        case 1:  return BooleanOp::Cut;
        case 2:  return BooleanOp::Intersect;
        default: return BooleanOp::Join;
    }
}

int CombineOperationChoice(BooleanOp theOperation)
{
    switch (theOperation) {
        case BooleanOp::Cut:       return 1;
        case BooleanOp::Intersect: return 2;
        default:                   return 0;
    }
}

// ---- CombineFeature ----

CombineFeature::CombineFeature(CombineBodyRef              theTarget,
                               std::vector<CombineBodyRef> theTools,
                               BooleanOp                   theOperation)
{
    SetTarget(std::move(theTarget));
    SetTools(std::move(theTools));
    SetOperation(theOperation);
}

void CombineFeature::SetTarget(CombineBodyRef theTarget)
{
    myTarget = theTarget.IsNull() ? CombineBodyRef() : std::move(theTarget);
}

void CombineFeature::SetTools(std::vector<CombineBodyRef> theTools)
{
    myTools = WithoutNullsOrDuplicates(std::move(theTools));
}

bool CombineFeature::SetOperation(BooleanOp theOperation)
{
    if (!IsCombineOperation(theOperation)) {
        return false;
    }
    myOperation = theOperation;
    return true;
}

bool CombineFeature::Compute(const ComputeContext& theContext,
                             const TopoDS_Shape&   theInput,
                             TopoDS_Shape&         theOutput,
                             std::string&          theError)
{
    (void)theContext;

    if (myTarget.IsNull()) {
        theError = "no target body selected";
        return false;
    }
    if (myTools.empty()) {
        theError = "no tool body selected";
        return false;
    }
    if (!IsCombineOperation(myOperation)) {
        theError = "combine needs Join, Cut or Intersect";
        return false;
    }
    if (IsEmptyShape(theInput)) {
        theError = "nothing to combine -- there are no bodies yet";
        return false;
    }

    try {
        // The bodies of the input shape, in the order the document lists
        // them. Non-solid bodies stay in this list so they are carried
        // through untouched; they simply never resolve as a pick.
        const std::vector<TopoDS_Shape> bodies = SplitIntoBodies(theInput);

        std::size_t targetIndex = 0;
        const Resolution found = ResolveBodyRef(bodies, myTarget, targetIndex);
        if (found != Resolution::Found) {
            theError = LostBodyMessage("target", myTarget, found);
            return false;
        }

        // Everything is resolved before anything is combined: half a
        // combine is worse than none, and one lost body fails the whole
        // feature rather than quietly combining the survivors.
        std::vector<std::size_t> toolIndices;
        toolIndices.reserve(myTools.size());
        for (const CombineBodyRef& tool : myTools) {
            std::size_t index = 0;
            const Resolution outcome = ResolveBodyRef(bodies, tool, index);
            if (outcome != Resolution::Found) {
                theError = LostBodyMessage("tool", tool, outcome);
                return false;
            }
            if (index == targetIndex) {
                theError = "'" + tool.name + "' is the target body -- a body cannot be "
                           "combined with itself";
                return false;
            }
            if (std::find(toolIndices.begin(), toolIndices.end(), index) != toolIndices.end()) {
                // Two refs on one body: cutting it away twice or fusing it
                // with itself hands the kernel a self-overlapping tool.
                theError = "'" + tool.name + "' is selected twice";
                return false;
            }
            toolIndices.push_back(index);
        }

        TopoDS_Shape running = bodies[targetIndex];
        const double before = SolidVolumeOf(running);

        for (std::size_t i = 0; i < toolIndices.size(); ++i) {
            const TopoDS_Shape& tool = bodies[toolIndices[i]];

            TopoDS_Shape next;
            if (!ApplyBooleanOperation(myOperation, running, tool, next, theError)) {
                theError = ToolPrefix(myTools, i) + theError;
                return false;
            }

            // Two bodies that only touch intersect in a face, and OCCT
            // hands that back as a perfectly good shape with no solid in
            // it. Left alone it would replace the target with a body that
            // has no volume, which is a wrong model rather than an error.
            if (!HasSolid(next)) {
                theError = BooleanOpName(myOperation) + " with '" + myTools[i].name
                           + "' leaves no solid behind -- the bodies only touch";
                return false;
            }

            running = next;
        }

        // A cut that removes nothing is not a cut. OCCT reports it as a
        // success that hands the target straight back, so the only way to
        // notice is to measure. Measured once over the whole operation
        // rather than per tool: with several tools a later one is allowed
        // to be redundant -- the first may already have taken everything
        // it would have reached -- and only a cut that changed nothing at
        // all is the user's mistake.
        if (myOperation == BooleanOp::Cut
            && SolidVolumeOf(running) >= before - std::max(1.0e-9, before * 1.0e-6)) {
            theError = myTools.size() == 1
                           ? "'" + myTools.front().name
                                 + "' does not overlap the target, so Cut removes nothing"
                           : "none of the tool bodies overlap the target, so Cut removes nothing";
            return false;
        }

        std::vector<TopoDS_Shape> kept;
        kept.reserve(bodies.size() + 1);
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            if (i == targetIndex) {
                // A cut can sever the target into several bodies, so the
                // result is flattened in rather than nested -- the body
                // table reads the direct children of the compound.
                for (const TopoDS_Shape& piece : SplitIntoBodies(running)) {
                    kept.push_back(piece);
                }
                continue;
            }
            const bool isTool =
                std::find(toolIndices.begin(), toolIndices.end(), i) != toolIndices.end();
            if (isTool && !myKeepTools) {
                continue;  // consumed, which is what a combine normally does
            }
            kept.push_back(bodies[i]);
        }

        theOutput = MakeCompoundOf(kept);
        if (IsEmptyShape(theOutput)) {
            theError = BooleanOpName(myOperation) + " left nothing behind";
            return false;
        }
        return true;
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "combine failed");
        return false;
    }
}

std::unique_ptr<Feature> CombineFeature::Clone() const
{
    auto copy = std::make_unique<CombineFeature>();
    // Undo restores the timeline from clones, so a field missed here is a
    // field the user loses the moment they press Ctrl+Z.
    copy->myTarget = myTarget;
    copy->myTools = myTools;
    copy->myOperation = myOperation;
    copy->myKeepTools = myKeepTools;
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> CombineFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    // The picks ride plain string rows, the same bargain a press/pull's
    // faces make: which bodies this was built on is inspectable, and a
    // pick can be cleared or retyped without a bespoke widget.
    parameters.push_back(Parameter::MakeString("Target", myTarget.Encode()));
    parameters.push_back(Parameter::MakeString("Tools", EncodeCombineBodyRefs(myTools)));
    parameters.push_back(Parameter::MakeString("Operation", BooleanOpName(myOperation)));
    parameters.push_back(Parameter::MakeBool("Keep Tools", myKeepTools));
    return parameters;
}

bool CombineFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Target") {
        CombineBodyRef target;
        if (!CombineBodyRef::Decode(theParameter.stringValue, target)) {
            // Refuse rather than quietly forgetting which body was picked.
            return false;
        }
        myTarget = target;
        return true;
    }
    if (theParameter.name == "Tools") {
        std::vector<CombineBodyRef> tools = DecodeCombineBodyRefs(theParameter.stringValue);
        if (tools.empty()) {
            return false;
        }
        if (tools.size() != CountCombineBodyRefSegments(theParameter.stringValue)) {
            // Some segments parsed and some did not. Keeping the survivors
            // would narrow the combine as silently as an empty string
            // would empty it, so the whole string is refused.
            return false;
        }
        SetTools(std::move(tools));
        return true;
    }
    if (theParameter.name == "Operation") {
        BooleanOp operation = myOperation;
        if (!ParseBooleanOp(theParameter.stringValue, operation)) {
            return false;
        }
        return SetOperation(operation);
    }
    if (theParameter.name == "Keep Tools") {
        myKeepTools = theParameter.boolValue;
        return true;
    }
    return false;
}

} // namespace lcad
