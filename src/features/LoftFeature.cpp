#include "features/LoftFeature.h"

#include "core/Document.h"
#include "core/ProfileProvider.h"

#include <BRepFill_ThruSectionErrorStatus.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace lcad {

namespace {

// Separates a section's sketch from the region chosen inside it. The ref
// that follows carries its own '@', so the FIRST mark is the split point.
constexpr char kProfileMark = ':';

// Two section planes closer than this, and parallel, are the same plane.
// Both are loose compared with the kernel's own tolerances because this
// test only decides which MESSAGE a degenerate loft gets: the volume
// check after the build is what actually catches the bad solid.
constexpr double kSamePlaneDistance = 1.0e-6;
constexpr double kSamePlaneAngle    = 1.0e-9;

std::string OcctMessage(const Standard_Failure& theFailure, const std::string& theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message) : theFallback;
}

std::string Trim(const std::string& theText)
{
    const std::size_t first = theText.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return std::string();
    }
    const std::size_t last = theText.find_last_not_of(" \t");
    return theText.substr(first, last - first + 1);
}

// "section 2 'Rim'" -- every failure names the section it happened in,
// because a loft through five sketches fails just as readably as one
// through two only if the user is told which one was the problem.
std::string SectionLabel(std::size_t theIndex, const LoftSection& theSection)
{
    std::string label = "section " + std::to_string(theIndex + 1);
    if (!theSection.sketch.empty()) {
        label += " ('" + theSection.sketch + "')";
    }
    return label;
}

int EdgeCount(const TopoDS_Wire& theWire)
{
    int count = 0;
    for (TopExp_Explorer explorer(theWire, TopAbs_EDGE); explorer.More(); explorer.Next()) {
        ++count;
    }
    return count;
}

// The classic loft failure is sections that don't correspond. OCCT splits
// edges to match a circle against a square and usually succeeds, so the
// counts are only worth mentioning when they differ AND the build failed
// -- at which point "4, 1" turns "loft failed" into something the user
// can go and fix.
std::string EdgeCountHint(const std::vector<TopoDS_Wire>& theWires)
{
    if (theWires.size() < 2) {
        return std::string();
    }

    std::ostringstream counts;
    const int first = EdgeCount(theWires.front());
    bool      same  = true;
    for (std::size_t i = 0; i < theWires.size(); ++i) {
        const int count = EdgeCount(theWires[i]);
        same = same && count == first;
        if (i != 0) {
            counts << ", ";
        }
        counts << count;
    }
    if (same) {
        return std::string();
    }
    return " -- the sections have different numbers of curves (" + counts.str()
         + "); matching their corner counts usually fixes it";
}

// OCCT's own status, turned into something a user can act on. Anything it
// cannot explain falls back to a plain statement plus whatever the
// sections themselves give away.
std::string LoftFailureMessage(BRepFill_ThruSectionErrorStatus theStatus,
                               const std::vector<TopoDS_Wire>& theWires)
{
    switch (theStatus) {
        case BRepFill_ThruSectionErrorStatus_NotSameTopology:
            return "loft failed -- every section must be a closed profile";
        case BRepFill_ThruSectionErrorStatus_ProfilesInconsistent:
            return "loft failed -- the sections could not be matched up"
                 + EdgeCountHint(theWires);
        case BRepFill_ThruSectionErrorStatus_Null3DCurve:
            return "loft failed -- a section curve has no 3D geometry";
        case BRepFill_ThruSectionErrorStatus_WrongUsage:
            return "loft failed -- a section cannot be used that way";
        default:
            break;
    }
    return "loft failed" + EdgeCountHint(theWires);
}

// The same plane, whichever way round its normal points: a loft between
// two sections drawn on it has nowhere to go.
bool SamePlane(const gp_Pln& theLeft, const gp_Pln& theRight)
{
    return theLeft.Axis().Direction().IsParallel(theRight.Axis().Direction(), kSamePlaneAngle)
        && theLeft.Distance(theRight.Location()) <= kSamePlaneDistance;
}

} // namespace

// ---- LoftSection ----

std::string LoftSection::Encode() const
{
    if (profile.IsNull()) {
        return sketch;
    }
    return sketch + kProfileMark + profile.Encode();
}

bool LoftSection::Decode(const std::string& theText, LoftSection& theResult)
{
    const std::string text = Trim(theText);
    if (text.empty()) {
        return false;
    }

    LoftSection parsed;
    const std::size_t mark = text.find(kProfileMark);
    if (mark == std::string::npos) {
        parsed.sketch = text;
        theResult = parsed;
        return true;
    }

    parsed.sketch = Trim(text.substr(0, mark));
    if (parsed.sketch.empty()) {
        return false;
    }
    if (!ProfileRef::Decode(Trim(text.substr(mark + 1)), parsed.profile)) {
        return false;
    }
    theResult = parsed;
    return true;
}

std::string EncodeLoftSections(const std::vector<LoftSection>& theSections)
{
    std::string text;
    for (const LoftSection& section : theSections) {
        if (section.sketch.empty()) {
            continue;
        }
        if (!text.empty()) {
            text += "; ";
        }
        text += section.Encode();
    }
    return text;
}

bool DecodeLoftSections(const std::string& theText, std::vector<LoftSection>& theSections)
{
    std::vector<LoftSection> parsed;
    std::istringstream parts(theText);
    std::string part;
    while (std::getline(parts, part, ';')) {
        LoftSection section;
        if (!LoftSection::Decode(part, section)) {
            return false;
        }
        parsed.push_back(std::move(section));
    }
    if (parsed.empty()) {
        return false;
    }
    theSections = std::move(parsed);
    return true;
}

// ---- Loft ----

LoftFeature::LoftFeature(std::vector<LoftSection> theSections)
{
    SetSections(std::move(theSections));
}

bool LoftFeature::ResolveSections(const ComputeContext&     theContext,
                                  std::vector<TopoDS_Wire>& theWires,
                                  std::vector<gp_Pln>&      thePlanes,
                                  std::string&              theError) const
{
    theWires.clear();
    thePlanes.clear();
    theWires.reserve(mySections.size());
    thePlanes.reserve(mySections.size());

    for (std::size_t i = 0; i < mySections.size(); ++i) {
        const LoftSection& section = mySections[i];
        const std::string  label   = SectionLabel(i, section);

        if (section.sketch.empty()) {
            theError = label + " names no sketch";
            return false;
        }

        // FindFeature only looks upstream, so a sketch dragged below this
        // feature in the timeline reads as missing -- say so rather than
        // just "not found".
        Feature* feature = theContext.FindFeature(section.sketch);
        if (feature == nullptr) {
            theError = label + " is not earlier in the timeline";
            return false;
        }

        ProfileProvider* provider = AsProfileProvider(feature);
        if (provider == nullptr) {
            theError = label + " is not a sketch";
            return false;
        }
        // As in ResolveProfile: a section sketch that failed this rebuild
        // holds geometry its own parameters no longer give.
        if (!feature->LastError().empty()) {
            theError = label + " failed (" + feature->LastError() + ")";
            return false;
        }

        try {
            TopoDS_Face face;
            if (!section.profile.IsNull()) {
                if (!provider->FindProfile(section.profile, face)) {
                    // Same bargain as every other reference here: the
                    // region this named is gone, and blending through
                    // whatever now sits nearby would be a wrong solid
                    // nobody notices.
                    theError = label + ": that profile no longer exists -- pick it again "
                                       "in the Loft dialog";
                    return false;
                }
            } else {
                const std::vector<TopoDS_Face> faces = provider->ProfileFaces();
                if (faces.empty()) {
                    theError = label + " has no closed profile";
                    return false;
                }
                if (faces.size() > 1) {
                    // Which region to blend through is a question only the
                    // user can answer; taking the largest would be a guess
                    // that shows up as a wrong body rather than an error.
                    theError = label + " has " + std::to_string(faces.size())
                             + " closed profiles -- pick the one to blend through in the "
                               "Loft dialog, which lists them separately";
                    return false;
                }
                face = faces.front();
            }

            if (face.IsNull()) {
                theError = label + " has no closed profile";
                return false;
            }

            // A loft section is ONE loop. The ring of a rectangle drawn
            // around a circle is a perfectly good profile that a blend
            // cannot use, and quietly lofting its outer loop alone would
            // fill the hole back in.
            std::vector<TopoDS_Wire> loops;
            for (TopExp_Explorer explorer(face, TopAbs_WIRE); explorer.More(); explorer.Next()) {
                loops.push_back(TopoDS::Wire(explorer.Current()));
            }
            if (loops.empty()) {
                theError = label + " has no closed profile";
                return false;
            }
            if (loops.size() > 1) {
                theError = label + " has a hole in it -- a loft section must be a "
                                   "single closed loop";
                return false;
            }

            theWires.push_back(loops.front());
            thePlanes.push_back(provider->ProfilePlane());
        } catch (const Standard_Failure& failure) {
            theError = OcctMessage(failure, label + ": could not read the sketch profile");
            return false;
        }
    }
    return true;
}

bool LoftFeature::Compute(const ComputeContext& theContext,
                          const TopoDS_Shape&   theInput,
                          TopoDS_Shape&         theOutput,
                          std::string&          theError)
{
    // Not a stylistic minimum: BRepOffsetAPI_ThruSections walks its
    // sections in pairs and crashes outright on a list of one, so this
    // check is the difference between a message and a dead application.
    if (mySections.size() < 2) {
        theError = "a loft needs at least two sections";
        return false;
    }
    if (myIsClosed && mySections.size() < 3) {
        // Closing two sections blends the same pair twice, once in each
        // direction, and the second pass lands exactly on the first.
        theError = "a closed loft needs at least three sections";
        return false;
    }

    std::vector<TopoDS_Wire> wires;
    std::vector<gp_Pln>      planes;
    if (!ResolveSections(theContext, wires, planes, theError)) {
        return false;
    }

    // Neighbouring sections on one plane give a segment with no height.
    // OCCT builds that without complaint and hands back a solid it calls
    // VALID with a volume of exactly zero -- the quiet kind of wrong this
    // codebase would rather fail on. Closed makes the last section a
    // neighbour of the first, so it is checked too.
    const std::size_t steps = myIsClosed ? planes.size() : planes.size() - 1;
    for (std::size_t i = 0; i < steps; ++i) {
        const std::size_t next = (i + 1) % planes.size();
        if (SamePlane(planes[i], planes[next])) {
            theError = "sections " + std::to_string(i + 1) + " and " + std::to_string(next + 1)
                     + " lie on the same plane -- a loft needs them on different planes";
            return false;
        }
    }

    TopoDS_Shape tool;
    try {
        // isSolid, because a shell would put a body with no volume in the
        // timeline. With Closed the first section is fed in a second time
        // at the end: OCCT recognises the loop and caps nothing, which is
        // exactly right for a tube that joins back to itself.
        BRepOffsetAPI_ThruSections generator(Standard_True, myIsRuled, Precision::Confusion());

        // The section wires belong to the sketches, and everything else
        // downstream still reads them. Left mutable, ThruSections splits
        // their edges in place to make the sections correspond -- editing
        // geometry this feature does not own.
        generator.SetMutableInput(Standard_False);

        for (const TopoDS_Wire& wire : wires) {
            generator.AddWire(wire);
        }
        if (myIsClosed) {
            generator.AddWire(wires.front());
        }

        generator.Build();
        if (!generator.IsDone()) {
            theError = LoftFailureMessage(generator.GetStatus(), wires);
            return false;
        }
        tool = generator.Shape();

        if (!HasSolid(tool)) {
            theError = "loft produced no solid";
            return false;
        }

        // One last look before the boolean. A twisted section list comes
        // back as a "valid" solid whose faces cancel out, and a body of no
        // volume is both wrong in the browser and the last thing a
        // boolean should be handed.
        GProp_GProps volume;
        BRepGProp::VolumeProperties(tool, volume);
        if (volume.Mass() <= 0.0) {
            theError = "the loft collapsed to no volume -- check that the sections are on "
                       "different planes and run the same way round";
            return false;
        }
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "loft failed") + EdgeCountHint(wires);
        return false;
    }

    return ApplyBooleanOperation(Operation(), theInput, tool, theOutput, theError);
}

std::unique_ptr<Feature> LoftFeature::Clone() const
{
    auto copy = std::make_unique<LoftFeature>();
    copy->mySections = mySections;
    copy->myIsClosed = myIsClosed;
    copy->myIsRuled = myIsRuled;
    // Carries the operation and the base name/suppressed flags. Undo
    // restores the timeline from clones, so a field missed here is a field
    // the user loses the moment they press Ctrl+Z.
    CopyProfileTo(*copy);
    return copy;
}

std::vector<Parameter> LoftFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeBool("Closed", myIsClosed));
    parameters.push_back(Parameter::MakeBool("Ruled", myIsRuled));
    // The order in this row IS the shape, so it is spelled out in full and
    // edited as text: retyping "Rim; Waist; Base" as "Base; Waist; Rim"
    // turns the loft round without a bespoke widget to reorder a list.
    parameters.push_back(Parameter::MakeString("Sections", EncodeLoftSections(mySections)));
    parameters.push_back(Parameter::MakeString("Operation", BooleanOpName(Operation())));
    return parameters;
}

bool LoftFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Closed") {
        myIsClosed = theParameter.boolValue;
        return true;
    }
    if (theParameter.name == "Ruled") {
        myIsRuled = theParameter.boolValue;
        return true;
    }
    if (theParameter.name == "Sections") {
        std::vector<LoftSection> sections;
        if (!DecodeLoftSections(theParameter.stringValue, sections)) {
            // Empty text, or one mangled segment among good ones. Either
            // way, keeping what survived would blend through a different
            // set of sections than the row says -- refuse the edit whole
            // and leave the stored list exactly as it was.
            return false;
        }
        mySections = std::move(sections);
        return true;
    }
    if (theParameter.name == "Operation") {
        BooleanOp operation = Operation();
        if (!ParseBooleanOp(theParameter.stringValue, operation)) {
            return false;
        }
        SetOperation(operation);
        return true;
    }
    // Deliberately not ProfileFeature::ApplyCommonParameter: its Sketch
    // and Profiles rows are the single-sketch field the sections replace,
    // and accepting an edit to a row this feature never shows would be a
    // setting that silently does nothing.
    return false;
}

} // namespace lcad
