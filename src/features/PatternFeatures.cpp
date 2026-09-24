#include "features/PatternFeatures.h"

#include "core/Body.h"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Two places closer together than this are one place -- OCCT's own
// confusion tolerance, which is what every boolean downstream will
// conclude about them anyway.
constexpr double kCoincident = 1.0e-7;

// Millimetres. A spacing below this is not a pattern, it is a stack.
constexpr double kMinSpacing = 1.0e-7;

// Degrees. A step this small turns nothing: at any radius this app works
// in it moves a point less than the kernel can tell apart.
constexpr double kMinAngleStep = 1.0e-6;

// Below this there is no volume to speak of. Which body a pick names is
// FindBodyForRef's question (CombineFeature.h), answered in one place.
constexpr double kMinVolume = 1.0e-9;

std::string OcctMessage(const Standard_Failure& theFailure, const std::string& theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message) : theFallback;
}

std::string ToLower(const std::string& theText)
{
    std::string lower = theText;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char theChar) {
        return static_cast<char>(std::tolower(theChar));
    });
    return lower;
}

// Match theText against a list of labels, case- and space-insensitively,
// the way ParseRevolveAxis does -- a value shown in a dropdown has to be
// understood when it is typed back into the properties panel by hand.
bool IndexOfName(const std::vector<std::string>& theNames,
                 const std::string&              theText,
                 int&                            theIndex)
{
    const std::string wanted = ToLower(theText);
    if (wanted.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < theNames.size(); ++i) {
        std::string candidate = ToLower(theNames[i]);
        if (candidate == wanted) {
            theIndex = static_cast<int>(i);
            return true;
        }
        candidate.erase(std::remove(candidate.begin(), candidate.end(), ' '), candidate.end());
        if (candidate == wanted) {
            theIndex = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

// Size and position of a solid, measured the way Body measures them so
// the two agree.
//
// The volume is SIGNED here, unlike CombineFeature's helper of a similar
// name. Body identity does not care which way round a solid is -- an
// imported one can arrive reversed -- but a COPY that came back reversed
// when its original was not is an inside-out tool, and that difference is
// exactly what PlacedAsExpected is looking for.
bool MeasureSolid(const TopoDS_Shape& theShape, double& theVolume, gp_Pnt& theCentre)
{
    if (theShape.IsNull() || !HasSolid(theShape)) {
        return false;
    }
    try {
        GProp_GProps properties;
        BRepGProp::VolumeProperties(theShape, properties);
        theVolume = properties.Mass();
        theCentre = properties.CentreOfMass();
        return std::fabs(theVolume) > kMinVolume;
    } catch (const Standard_Failure&) {
        return false;
    }
}

using Resolution = BodyMatch;

std::string LostBodyMessage(const CombineBodyRef& theRef, Resolution theOutcome)
{
    const std::string what = "the body '" + theRef.name + "'";
    if (theOutcome == Resolution::Ambiguous) {
        return what + " now matches two bodies equally well -- re-pick it";
    }
    return what + " is no longer in the model -- re-pick it";
}

// Where one instance ended up, for the single check that catches every
// way a pattern can stack copies on top of each other.
//
// The centre is COMPUTED, not measured: a rigid motion carries a solid's
// centre of mass exactly, so the transform applied to the seed's centre
// is the copy's centre, and measuring every instance would pay for a
// volume integration per copy to learn what arithmetic already knows.
// Compute() measures the first copy of each body and checks it against
// that arithmetic, so a kernel that stopped behaving this way would be
// caught rather than quietly believed.
//
// The box is measured, and only once two centres have already collided.
// Two distinct translations always move a bounded solid, so for a
// rectangular pattern the centres settle it outright. A rotation does
// not -- a body sitting on the axis keeps its centre wherever it is
// turned -- and neither does a mirror of something straddling the plane.
// There the box is the second signal, the same two-signal bargain every
// reference in this codebase makes.
struct Placement
{
    TopoDS_Shape shape;
    gp_Pnt       centre;
    std::string  body;            // the name the user picked, for the message
    int          number = 1;      // 1-based, the way Fusion counts instances
    Bnd_Box      box;
    bool         hasBox = false;

    const Bnd_Box& Box()
    {
        if (!hasBox) {
            BRepBndLib::Add(shape, box, Standard_False);
            hasBox = true;
        }
        return box;
    }
};

Placement MakePlacement(const TopoDS_Shape& theShape,
                        const gp_Pnt&       theCentre,
                        const std::string&  theBody,
                        int                 theNumber)
{
    Placement placement;
    placement.shape  = theShape;
    placement.centre = theCentre;
    placement.body   = theBody;
    placement.number = theNumber;
    return placement;
}

bool SameBox(const Bnd_Box& theLeft, const Bnd_Box& theRight)
{
    if (theLeft.IsVoid() || theRight.IsVoid()) {
        return false;
    }
    Standard_Real lx1 = 0.0, ly1 = 0.0, lz1 = 0.0, lx2 = 0.0, ly2 = 0.0, lz2 = 0.0;
    Standard_Real rx1 = 0.0, ry1 = 0.0, rz1 = 0.0, rx2 = 0.0, ry2 = 0.0, rz2 = 0.0;
    theLeft.Get(lx1, ly1, lz1, lx2, ly2, lz2);
    theRight.Get(rx1, ry1, rz1, rx2, ry2, rz2);

    const double corners[6] = {lx1 - rx1, ly1 - ry1, lz1 - rz1,
                               lx2 - rx2, ly2 - ry2, lz2 - rz2};
    for (const double delta : corners) {
        if (std::fabs(delta) > kCoincident) {
            return false;
        }
    }
    return true;
}

// Refuse a tool that overlaps itself.
//
// Two copies in the same place double the volume the model reports and
// hand every boolean downstream a self-intersecting argument, and neither
// of those announces itself. That is the expensive kind of wrong, so it
// is an error rather than something to draw and let the user notice.
//
// Only the pattern's own instances are compared, seeds included. A copy
// landing on some OTHER body the user modelled there is an overlap they
// can see and may well have meant; a copy landing on a copy is this
// feature stacking geometry on itself.
bool FindCoincidence(std::vector<Placement>& thePlacements,
                     const std::string&      theNoun,
                     std::string&            theError)
{
    for (std::size_t i = 0; i < thePlacements.size(); ++i) {
        for (std::size_t j = i + 1; j < thePlacements.size(); ++j) {
            Placement& left  = thePlacements[i];
            Placement& right = thePlacements[j];

            if (left.centre.Distance(right.centre) > kCoincident) {
                continue;
            }
            if (!SameBox(left.Box(), right.Box())) {
                continue;
            }

            theError = left.body == right.body
                           ? "instances " + std::to_string(left.number) + " and "
                                 + std::to_string(right.number) + " of '" + left.body
                                 + "' land in the same place"
                           : "instance " + std::to_string(left.number) + " of '" + left.body
                                 + "' lands on instance " + std::to_string(right.number)
                                 + " of '" + right.body + "'";
            theError += " -- a " + theNoun + " must not stack copies on top of each other";
            return true;
        }
    }
    return false;
}

// The kernel's copy, checked against the arithmetic everything else here
// relies on.
//
// BRepBuilderAPI_Transform is trusted to carry a solid rigidly: same
// volume, same way round, centre moved exactly as the transform says.
// That holds today, a mirror included -- even though a reflection has a
// negative determinant and could plausibly hand back an inside-out solid
// whose volume reads negative. Measuring the first copy of each body
// costs one integration and turns "plausibly" into "measured", which is
// the difference between a guarded assumption and an unguarded one.
bool PlacedAsExpected(const TopoDS_Shape& theCopy,
                      double              theSeedVolume,
                      const gp_Pnt&       theExpectedCentre,
                      const std::string&  theName,
                      std::string&        theError)
{
    double volume = 0.0;
    gp_Pnt centre;
    if (!MeasureSolid(theCopy, volume, centre)) {
        theError = "the copy of '" + theName + "' came back with no volume";
        return false;
    }

    if (std::fabs(volume - theSeedVolume) > std::fabs(theSeedVolume) * 1.0e-6) {
        theError = "the copy of '" + theName + "' measures " + FormatNumber(volume)
                 + " against the original's " + FormatNumber(theSeedVolume)
                 + " -- it was not copied rigidly";
        return false;
    }

    const double reach = std::max(1.0, theExpectedCentre.XYZ().Modulus());
    if (centre.Distance(theExpectedCentre) > reach * 1.0e-6) {
        theError = "the copy of '" + theName + "' did not land where it was placed";
        return false;
    }
    return true;
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

// A quantity row the properties panel can bound: one instance at the
// bottom, because a pattern of none is not a thing, and the same ceiling
// Compute enforces at the top.
Parameter QuantityParameter(std::string theName, int theCount)
{
    Parameter parameter = Parameter::MakeInt(std::move(theName), theCount);
    parameter.minimum = 1.0;
    parameter.maximum = static_cast<double>(PatternFeature::kMaxInstances);
    return parameter;
}

bool ReadAxisParameter(const Parameter& theParameter, const char* theName, PatternAxis& theAxis)
{
    if (theParameter.name != theName) {
        return false;
    }
    PatternAxis parsed = theAxis;
    if (!ParsePatternAxis(theParameter.stringValue, parsed)) {
        return false;
    }
    theAxis = parsed;
    return true;
}

} // namespace

// ---- axes and planes ----

const std::vector<std::string>& PatternAxisNames()
{
    static const std::vector<std::string> names = {"World X", "World Y", "World Z"};
    return names;
}

std::string PatternAxisName(PatternAxis theAxis)
{
    const std::vector<std::string>& names = PatternAxisNames();
    const std::size_t index = static_cast<std::size_t>(theAxis);
    return index < names.size() ? names[index] : names.front();
}

PatternAxis PatternAxisFromInt(int theValue)
{
    if (theValue < 0 || theValue > static_cast<int>(PatternAxis::WorldZ)) {
        return PatternAxis::WorldX;
    }
    return static_cast<PatternAxis>(theValue);
}

bool ParsePatternAxis(const std::string& theText, PatternAxis& theAxis)
{
    int index = 0;
    if (!IndexOfName(PatternAxisNames(), theText, index)) {
        return false;
    }
    theAxis = PatternAxisFromInt(index);
    return true;
}

gp_Dir PatternAxisDirection(PatternAxis theAxis)
{
    switch (theAxis) {
        case PatternAxis::WorldX:
            return gp_Dir(1.0, 0.0, 0.0);
        case PatternAxis::WorldY:
            return gp_Dir(0.0, 1.0, 0.0);
        case PatternAxis::WorldZ:
            return gp_Dir(0.0, 0.0, 1.0);
    }
    return gp_Dir(1.0, 0.0, 0.0);
}

const std::vector<std::string>& MirrorPlaneNames()
{
    static const std::vector<std::string> names = {"XY", "XZ", "YZ"};
    return names;
}

std::string MirrorPlaneName(MirrorPlane thePlane)
{
    const std::vector<std::string>& names = MirrorPlaneNames();
    const std::size_t index = static_cast<std::size_t>(thePlane);
    return index < names.size() ? names[index] : names.front();
}

MirrorPlane MirrorPlaneFromInt(int theValue)
{
    if (theValue < 0 || theValue > static_cast<int>(MirrorPlane::YZ)) {
        return MirrorPlane::YZ;
    }
    return static_cast<MirrorPlane>(theValue);
}

bool ParseMirrorPlane(const std::string& theText, MirrorPlane& thePlane)
{
    int index = 0;
    if (!IndexOfName(MirrorPlaneNames(), theText, index)) {
        return false;
    }
    thePlane = MirrorPlaneFromInt(index);
    return true;
}

gp_Ax2 MirrorPlaneFrame(MirrorPlane thePlane)
{
    // gp_Trsf::SetMirror(gp_Ax2) reflects across the frame's XY plane, so
    // the frame's Z is the plane's normal.
    const gp_Pnt origin(0.0, 0.0, 0.0);
    switch (thePlane) {
        case MirrorPlane::XY:
            return gp_Ax2(origin, gp_Dir(0.0, 0.0, 1.0));
        case MirrorPlane::XZ:
            return gp_Ax2(origin, gp_Dir(0.0, 1.0, 0.0));
        case MirrorPlane::YZ:
            return gp_Ax2(origin, gp_Dir(1.0, 0.0, 0.0));
    }
    return gp_Ax2(origin, gp_Dir(1.0, 0.0, 0.0));
}

// ---- PatternFeature ----

void PatternFeature::SetBodies(std::vector<CombineBodyRef> theBodies)
{
    myBodies = WithoutNullsOrDuplicates(std::move(theBodies));
}

bool PatternFeature::Compute(const ComputeContext& theContext,
                             const TopoDS_Shape&   theInput,
                             TopoDS_Shape&         theOutput,
                             std::string&          theError)
{
    (void)theContext;

    if (myBodies.empty()) {
        theError = "no body selected";
        return false;
    }
    if (IsEmptyShape(theInput)) {
        theError = "nothing to " + Noun() + " -- there are no bodies yet";
        return false;
    }

    std::vector<gp_Trsf> transforms;
    if (!InstanceTransforms(transforms, theError)) {
        return false;
    }

    try {
        const std::vector<TopoDS_Shape> bodies = SplitIntoBodies(theInput);

        // Everything is resolved before anything is copied: half a
        // pattern is worse than none, and one lost pick fails the whole
        // feature rather than quietly patterning the bodies that
        // survived.
        std::vector<std::size_t> seeds;
        seeds.reserve(myBodies.size());
        for (const CombineBodyRef& ref : myBodies) {
            std::size_t index = 0;
            const Resolution outcome = FindBodyForRef(bodies, ref, index);
            if (outcome != Resolution::Found) {
                theError = LostBodyMessage(ref, outcome);
                return false;
            }
            if (std::find(seeds.begin(), seeds.end(), index) != seeds.end()) {
                // Two names, one body: every copy would be made twice.
                theError = "'" + ref.name + "' is selected twice";
                return false;
            }
            seeds.push_back(index);
        }

        // A pattern of one is the body that is already there -- a no-op,
        // not a mistake. Fusion accepts a quantity of one and draws one
        // instance; so does this.
        if (transforms.empty()) {
            theOutput = theInput;
            return true;
        }

        const std::size_t total = (transforms.size() + 1) * seeds.size();
        if (total > kMaxInstances) {
            theError = std::to_string(seeds.size()) + " bodies at "
                     + std::to_string(transforms.size() + 1) + " instances is "
                     + std::to_string(total) + " copies -- a " + Noun()
                     + " is limited to " + std::to_string(kMaxInstances);
            return false;
        }

        std::vector<Placement> placements;
        placements.reserve(total);

        std::vector<TopoDS_Shape> copies;
        copies.reserve(total - seeds.size());

        for (std::size_t s = 0; s < seeds.size(); ++s) {
            const TopoDS_Shape& seed = bodies[seeds[s]];
            const std::string&  name = myBodies[s].name;

            double seedVolume = 0.0;
            gp_Pnt seedCentre;
            if (!MeasureSolid(seed, seedVolume, seedCentre)) {
                // FindBodyForRef measured this a moment ago, so it cannot
                // normally fail here -- but a guessed centre would poison
                // the coincidence check below, which is the one thing
                // standing between the user and a doubled volume.
                theError = "could not measure '" + name + "'";
                return false;
            }

            placements.push_back(MakePlacement(seed, seedCentre, name, 1));

            for (std::size_t i = 0; i < transforms.size(); ++i) {
                const gp_Trsf& placement = transforms[i];
                const int      number    = static_cast<int>(i) + 2;

                BRepBuilderAPI_Transform copy(seed, placement, Standard_True);
                if (!copy.IsDone() || copy.Shape().IsNull()) {
                    theError = "could not place instance " + std::to_string(number)
                             + " of '" + name + "'";
                    return false;
                }

                const gp_Pnt centre = seedCentre.Transformed(placement);
                if (i == 0
                    && !PlacedAsExpected(copy.Shape(), seedVolume, centre, name, theError)) {
                    return false;
                }

                placements.push_back(MakePlacement(copy.Shape(), centre, name, number));
                copies.push_back(copy.Shape());
            }
        }

        if (FindCoincidence(placements, Noun(), theError)) {
            return false;
        }

        const TopoDS_Shape tool = MakeCompoundOf(copies);
        if (IsEmptyShape(tool)) {
            theError = Noun() + " produced no geometry";
            return false;
        }

        // The seeds stay where they are -- they are part of theInput --
        // so the tool is instance two upwards and a four-instance pattern
        // comes out at four times the volume rather than five.
        return ApplyBooleanOperation(myOperation, theInput, tool, theOutput, theError);
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, Noun() + " failed");
        return false;
    }
}

void PatternFeature::AppendCommonParameters(std::vector<Parameter>& theParameters) const
{
    // The picks ride a plain string row, the same bargain Combine's do:
    // which bodies this was built on is inspectable, and a pick can be
    // cleared or retyped without a bespoke widget.
    theParameters.push_back(Parameter::MakeString("Bodies", EncodeCombineBodyRefs(myBodies)));
    theParameters.push_back(Parameter::MakeString("Operation", BooleanOpName(myOperation)));
}

bool PatternFeature::ApplyCommonParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Bodies") {
        std::vector<CombineBodyRef> bodies = DecodeCombineBodyRefs(theParameter.stringValue);
        if (bodies.empty()) {
            // Text that decodes to nothing is a typo, not a request to
            // pattern nothing.
            return false;
        }
        if (bodies.size() != CountCombineBodyRefSegments(theParameter.stringValue)) {
            // Some segments parsed and some did not. Keeping the
            // survivors would narrow the pattern as silently as an empty
            // string would empty it, so the whole string is refused.
            return false;
        }
        SetBodies(std::move(bodies));
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

void PatternFeature::CopyPatternTo(PatternFeature& theOther) const
{
    // Undo restores the timeline from clones, so a field missed here is a
    // field the user loses the moment they press Ctrl+Z.
    theOther.myBodies = myBodies;
    theOther.myOperation = myOperation;
    CopyBaseTo(theOther);
}

// ---- RectangularPatternFeature ----

RectangularPatternFeature::RectangularPatternFeature(std::vector<CombineBodyRef> theBodies,
                                                     int                         theCount1,
                                                     double                      theSpacing1,
                                                     int                         theCount2,
                                                     double                      theSpacing2)
    : myCount1(theCount1), mySpacing1(theSpacing1), myCount2(theCount2), mySpacing2(theSpacing2)
{
    SetBodies(std::move(theBodies));
}

bool RectangularPatternFeature::InstanceTransforms(std::vector<gp_Trsf>& theTransforms,
                                                   std::string&          theError) const
{
    if (myCount1 <= 0 || myCount2 <= 0) {
        theError = "quantity must be at least 1";
        return false;
    }

    // Zero spacing puts every instance of that direction on top of the
    // first: the volume doubles, nothing goes red, and the boolean is
    // handed a tool overlapping itself. It is the same failure
    // ProfileFeature::SetProfiles refuses for a region picked twice,
    // arriving through a different door, so it is refused here too rather
    // than left for the coincidence check to describe in vaguer words.
    if (myCount1 > 1 && std::fabs(mySpacing1) < kMinSpacing) {
        theError = "spacing 1 is zero, so all " + std::to_string(myCount1)
                 + " instances along " + PatternAxisName(myDirection1)
                 + " would land on top of each other";
        return false;
    }
    if (myCount2 > 1 && std::fabs(mySpacing2) < kMinSpacing) {
        theError = "spacing 2 is zero, so all " + std::to_string(myCount2)
                 + " instances along " + PatternAxisName(myDirection2)
                 + " would land on top of each other";
        return false;
    }

    const std::size_t total =
        static_cast<std::size_t>(myCount1) * static_cast<std::size_t>(myCount2);
    if (total > kMaxInstances) {
        theError = std::to_string(myCount1) + " by " + std::to_string(myCount2) + " is "
                 + std::to_string(total) + " instances -- a pattern is limited to "
                 + std::to_string(kMaxInstances);
        return false;
    }

    // Fusion's "Spacing" distribution: the step is the gap between one
    // instance and the next, so raising the quantity makes the grid
    // longer instead of packing more copies into the same span.
    const gp_Vec step1 = gp_Vec(PatternAxisDirection(myDirection1)) * mySpacing1;
    const gp_Vec step2 = gp_Vec(PatternAxisDirection(myDirection2)) * mySpacing2;

    theTransforms.clear();
    theTransforms.reserve(total - 1);
    for (int i = 0; i < myCount1; ++i) {
        for (int j = 0; j < myCount2; ++j) {
            if (i == 0 && j == 0) {
                continue;  // the seed, already in the model
            }
            gp_Trsf move;
            move.SetTranslation(step1 * static_cast<double>(i) + step2 * static_cast<double>(j));
            theTransforms.push_back(move);
        }
    }
    return true;
}

std::unique_ptr<Feature> RectangularPatternFeature::Clone() const
{
    auto copy = std::make_unique<RectangularPatternFeature>();
    copy->myDirection1 = myDirection1;
    copy->myCount1 = myCount1;
    copy->mySpacing1 = mySpacing1;
    copy->myDirection2 = myDirection2;
    copy->myCount2 = myCount2;
    copy->mySpacing2 = mySpacing2;
    CopyPatternTo(*copy);
    return copy;
}

std::vector<Parameter> RectangularPatternFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeString("Direction 1", PatternAxisName(myDirection1)));
    parameters.push_back(QuantityParameter("Quantity 1", myCount1));
    parameters.push_back(Parameter::MakeDouble("Spacing 1", mySpacing1));
    parameters.push_back(Parameter::MakeString("Direction 2", PatternAxisName(myDirection2)));
    parameters.push_back(QuantityParameter("Quantity 2", myCount2));
    parameters.push_back(Parameter::MakeDouble("Spacing 2", mySpacing2));
    AppendCommonParameters(parameters);
    return parameters;
}

bool RectangularPatternFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Direction 1" || theParameter.name == "Direction 2") {
        return ReadAxisParameter(theParameter, "Direction 1", myDirection1)
            || ReadAxisParameter(theParameter, "Direction 2", myDirection2);
    }
    if (theParameter.name == "Quantity 1") {
        // An impossible quantity is stored and reported at rebuild, the
        // way Extrude stores a zero distance: the properties panel is
        // where it gets fixed, and it cannot be fixed if it was never
        // accepted.
        myCount1 = theParameter.intValue;
        return true;
    }
    if (theParameter.name == "Spacing 1") {
        mySpacing1 = theParameter.doubleValue;
        return true;
    }
    if (theParameter.name == "Quantity 2") {
        myCount2 = theParameter.intValue;
        return true;
    }
    if (theParameter.name == "Spacing 2") {
        mySpacing2 = theParameter.doubleValue;
        return true;
    }
    return ApplyCommonParameter(theParameter);
}

// ---- CircularPatternFeature ----

CircularPatternFeature::CircularPatternFeature(std::vector<CombineBodyRef> theBodies,
                                               PatternAxis                 theAxis,
                                               int                         theCount,
                                               double                      theTotalAngleDegrees)
    : myAxis(theAxis), myCount(theCount), myTotalAngleDegrees(theTotalAngleDegrees)
{
    SetBodies(std::move(theBodies));
}

bool CircularPatternFeature::InstanceTransforms(std::vector<gp_Trsf>& theTransforms,
                                                std::string&          theError) const
{
    if (myCount <= 0) {
        theError = "quantity must be at least 1";
        return false;
    }
    if (static_cast<std::size_t>(myCount) > kMaxInstances) {
        theError = std::to_string(myCount) + " instances -- a pattern is limited to "
                 + std::to_string(kMaxInstances);
        return false;
    }

    theTransforms.clear();
    if (myCount == 1) {
        // One instance is the body already in the model, whatever the
        // angle says; Fusion greys the angle out at a quantity of one.
        return true;
    }

    const double sweep = std::fabs(myTotalAngleDegrees);
    if (sweep < kMinAngleStep || sweep > 360.0 + kCoincident) {
        theError = "total angle must be between 0 and 360 degrees";
        return false;
    }

    // THE step, and the one this feature exists to get right.
    //
    // Over a FULL turn the ring closes on itself: instance one and the
    // place 360 degrees later are the same place, so N instances are N
    // steps of 360/N apart. Using 360/(N-1) there walks all the way round
    // and drops the last instance exactly on the first -- the classic
    // circular-pattern bug, and a silent one, because the volume still
    // adds up and nothing goes red.
    //
    // Over a PARTIAL angle both ends are occupied instead: N instances
    // span N-1 gaps, so the step is the angle over N-1 and the last
    // instance sits exactly on the angle the user typed.
    const bool   fullTurn    = sweep >= 360.0 - kCoincident;
    const double stepDegrees = fullTurn
                                   ? myTotalAngleDegrees / static_cast<double>(myCount)
                                   : myTotalAngleDegrees / static_cast<double>(myCount - 1);

    if (std::fabs(stepDegrees) < kMinAngleStep) {
        theError = "an angle of " + FormatNumber(myTotalAngleDegrees) + " degrees over "
                 + std::to_string(myCount)
                 + " instances leaves no room between them";
        return false;
    }

    const gp_Ax1 axis(gp_Pnt(0.0, 0.0, 0.0), PatternAxisDirection(myAxis));

    // Degrees in, radians out: the one place this feature converts.
    const double step = stepDegrees * kPi / 180.0;

    theTransforms.reserve(static_cast<std::size_t>(myCount - 1));
    for (int i = 1; i < myCount; ++i) {
        gp_Trsf turn;
        turn.SetRotation(axis, step * static_cast<double>(i));
        theTransforms.push_back(turn);
    }
    return true;
}

std::unique_ptr<Feature> CircularPatternFeature::Clone() const
{
    auto copy = std::make_unique<CircularPatternFeature>();
    copy->myAxis = myAxis;
    copy->myCount = myCount;
    copy->myTotalAngleDegrees = myTotalAngleDegrees;
    CopyPatternTo(*copy);
    return copy;
}

std::vector<Parameter> CircularPatternFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeString("Axis", PatternAxisName(myAxis)));
    parameters.push_back(QuantityParameter("Quantity", myCount));
    parameters.push_back(Parameter::MakeDouble("Total Angle", myTotalAngleDegrees, "deg"));
    AppendCommonParameters(parameters);
    return parameters;
}

bool CircularPatternFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Axis") {
        return ReadAxisParameter(theParameter, "Axis", myAxis);
    }
    if (theParameter.name == "Quantity") {
        myCount = theParameter.intValue;
        return true;
    }
    if (theParameter.name == "Total Angle") {
        myTotalAngleDegrees = theParameter.doubleValue;
        return true;
    }
    return ApplyCommonParameter(theParameter);
}

// ---- MirrorFeature ----

MirrorFeature::MirrorFeature(std::vector<CombineBodyRef> theBodies, MirrorPlane thePlane)
    : myPlane(thePlane)
{
    SetBodies(std::move(theBodies));
}

bool MirrorFeature::InstanceTransforms(std::vector<gp_Trsf>& theTransforms,
                                       std::string&          theError) const
{
    (void)theError;

    // A mirror has exactly one instance beyond the original, and nothing
    // to validate: every world plane is a usable one. A body sitting
    // symmetrically across it would reflect onto itself, and that is
    // caught where it can actually be seen -- by measuring where the copy
    // lands, not by second-guessing the plane.
    gp_Trsf reflect;
    reflect.SetMirror(MirrorPlaneFrame(myPlane));

    theTransforms.assign(1, reflect);
    return true;
}

std::unique_ptr<Feature> MirrorFeature::Clone() const
{
    auto copy = std::make_unique<MirrorFeature>();
    copy->myPlane = myPlane;
    CopyPatternTo(*copy);
    return copy;
}

std::vector<Parameter> MirrorFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeString("Mirror Plane", MirrorPlaneName(myPlane)));
    AppendCommonParameters(parameters);
    return parameters;
}

bool MirrorFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Mirror Plane") {
        MirrorPlane plane = myPlane;
        if (!ParseMirrorPlane(theParameter.stringValue, plane)) {
            return false;
        }
        myPlane = plane;
        return true;
    }
    return ApplyCommonParameter(theParameter);
}

} // namespace lcad
