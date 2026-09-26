#include "features/SweepFeature.h"

#include "core/Document.h"
#include "core/ProfileProvider.h"

#include <BRepAdaptor_CompCurve.hxx>
#include <BRepAlgoAPI_Check.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <BRepBuilderAPI_TransitionMode.hxx>
#include <BRepFill_TypeOfContact.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_UniformAbscissa.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Law_Linear.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Millimetres / degrees below this are a no-op sweep rather than a solid.
constexpr double kMinSweep = 1.0e-7;

// Past this the taper's tangent runs away faster than any path is long.
constexpr double kMaxTaperDegrees = 89.0;

// Ten turns. More than that is a typo, not a design.
constexpr double kMaxTwistDegrees = 3600.0;

// What counts as "the path starts at the profile". OCCT calls two points
// the same at 1e-7, so a path drawn from a point of the profile plane
// lands well inside this; anything the user can see is well outside.
constexpr double kOnPlane = 1.0e-6;

// A section that shrinks below this much of its drawn size has been
// closed off by the taper rather than tapered.
constexpr double kMinTaperScale = 1.0e-3;

// How square the path has to stay to the profile for a parallel sweep to
// have any thickness at all -- about three hundredths of a degree.
constexpr double kMinTangentCosine = 1.0e-3;

// Enough samples that a bend reads as a bend rather than a polygon.
constexpr int kPathSamples = 64;

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

std::string PipeErrorText(BRepBuilderAPI_PipeError theStatus)
{
    switch (theStatus) {
        case BRepBuilderAPI_PlaneNotIntersectGuide:
            return "the twist could not be carried along this path -- try a smaller angle";
        case BRepBuilderAPI_ImpossibleContact:
            return "the profile could not be brought onto the path";
        case BRepBuilderAPI_PipeDone:
        case BRepBuilderAPI_PipeNotDone:
            break;
    }
    return "the sweep failed -- the path may turn tighter than the profile is wide";
}

double ShapeLength(const TopoDS_Shape& theShape)
{
    GProp_GProps properties;
    BRepGProp::LinearProperties(theShape, properties);
    return properties.Mass();
}

double ShapeVolume(const TopoDS_Shape& theShape)
{
    GProp_GProps properties;
    BRepGProp::VolumeProperties(theShape, properties);
    return properties.Mass();
}

// One place along the path, with the direction of travel there.
struct PathSample
{
    gp_Pnt point;
    gp_Dir tangent;
    double fraction = 0.0;   // 0 at the start, 1 at the end, measured by arc length
};

// Walks the path at even arc-length steps. Every guard below reads these
// samples rather than the curve, so the path is only ever traversed once
// and every question is asked of the same points.
bool SamplePath(const TopoDS_Wire& thePath, std::vector<PathSample>& theSamples)
{
    theSamples.clear();

    Handle(BRepAdaptor_CompCurve) curve = new BRepAdaptor_CompCurve(thePath);
    const double first = curve->FirstParameter();
    const double last  = curve->LastParameter();
    if (last - first < kMinSweep) {
        return false;
    }

    GCPnts_UniformAbscissa sampler(*curve, kPathSamples, first, last);
    if (!sampler.IsDone() || sampler.NbPoints() < 2) {
        return false;
    }

    const int count = sampler.NbPoints();
    theSamples.reserve(static_cast<std::size_t>(count));
    for (int i = 1; i <= count; ++i) {
        gp_Pnt point;
        gp_Vec tangent;
        curve->D1(sampler.Parameter(i), point, tangent);
        if (tangent.Magnitude() < kMinSweep) {
            return false;   // a cusp: no direction to sweep along
        }
        PathSample sample;
        sample.point    = point;
        sample.tangent  = gp_Dir(tangent);
        sample.fraction = static_cast<double>(i - 1) / static_cast<double>(count - 1);
        theSamples.push_back(sample);
    }
    return true;
}

// Turn the path round so it runs away from the profile.
//
// Rebuilt edge by edge rather than merely flagged reversed: the pipe
// anchors a taper to the wire's own first vertex, and a re-ordered wire
// says which end that is without relying on an orientation flag being
// honoured all the way down.
bool ReversePath(const TopoDS_Wire& thePath, TopoDS_Wire& theResult)
{
    std::vector<TopoDS_Edge> edges;
    for (BRepTools_WireExplorer explorer(thePath); explorer.More(); explorer.Next()) {
        edges.push_back(explorer.Current());
    }
    if (edges.empty()) {
        return false;
    }

    BRepBuilderAPI_MakeWire maker;
    for (auto edge = edges.rbegin(); edge != edges.rend(); ++edge) {
        maker.Add(TopoDS::Edge(edge->Reversed()));
    }
    if (!maker.IsDone()) {
        return false;
    }
    theResult = maker.Wire();
    return true;
}

// Distance from a point to the nearest point of a shape. False when OCCT
// cannot measure it, which is a reason to refuse rather than to invent a
// number -- everything this is used for divides by it.
bool NearestDistance(const gp_Pnt& thePoint, const TopoDS_Shape& theShape, double& theDistance)
{
    BRepBuilderAPI_MakeVertex vertex(thePoint);
    if (!vertex.IsDone()) {
        return false;
    }
    BRepExtrema_DistShapeShape measure(vertex.Vertex(), theShape);
    if (!measure.IsDone() || measure.NbSolution() < 1) {
        return false;
    }
    theDistance = measure.Value();
    return true;
}

// A guide curve spiralling around the path.
//
// MakePipeShell has no twist of its own. What it has is a guided mode,
// where the section's normal points from each path point at the matching
// point of a second curve; spiral that curve around the path and the
// section turns with it, which is what Fusion's twist angle does.
//
// The frame the spiral is laid out in is carried along by parallel
// transport -- each step turns the previous normal by the smallest
// rotation that takes the previous tangent onto the new one. That frame
// is twist-free by construction, so the only turning left in the guide is
// the turning that was asked for.
bool BuildTwistGuide(const std::vector<PathSample>& theSamples,
                     double                         theTwistRadians,
                     double                         theRadius,
                     TopoDS_Wire&                   theGuide)
{
    if (theSamples.size() < 3 || theRadius < kMinSweep) {
        return false;
    }

    // Any direction square to the first tangent will do. The section is
    // swept in the pose it was drawn in, so only how far the guide turns
    // matters, never where it starts turning from.
    const gp_Dir& start = theSamples.front().tangent;
    const gp_Dir helper = (std::fabs(start.X()) < 0.9) ? gp_Dir(1.0, 0.0, 0.0)
                                                       : gp_Dir(0.0, 1.0, 0.0);
    gp_Dir normal   = start.Crossed(helper);
    gp_Dir previous = start;

    Handle(TColgp_HArray1OfPnt) points =
        new TColgp_HArray1OfPnt(1, static_cast<int>(theSamples.size()));

    for (std::size_t i = 0; i < theSamples.size(); ++i) {
        const PathSample& sample = theSamples[i];

        const gp_Vec turn = gp_Vec(previous).Crossed(gp_Vec(sample.tangent));
        if (turn.Magnitude() > kMinSweep) {
            gp_Trsf rotation;
            rotation.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(turn)),
                                 previous.Angle(sample.tangent));
            normal = gp_Dir(gp_Vec(normal).Transformed(rotation));
        }

        // Float drift accumulates over a few hundred steps, so the frame
        // is squared up again every time rather than once at the end.
        const gp_Vec across = gp_Vec(sample.tangent).Crossed(gp_Vec(normal));
        if (across.Magnitude() < kMinSweep) {
            return false;
        }
        const gp_Dir binormal(across);
        normal = gp_Dir(gp_Vec(binormal).Crossed(gp_Vec(sample.tangent)));

        const double angle = theTwistRadians * sample.fraction;
        const gp_Vec offset = gp_Vec(normal) * (theRadius * std::cos(angle))
                            + gp_Vec(binormal) * (theRadius * std::sin(angle));
        points->SetValue(static_cast<int>(i) + 1, sample.point.Translated(offset));

        previous = sample.tangent;
    }

    GeomAPI_Interpolate interpolate(points, Standard_False, Precision::Confusion());
    interpolate.Perform();
    if (!interpolate.IsDone()) {
        return false;
    }

    BRepBuilderAPI_MakeEdge edge(interpolate.Curve());
    if (!edge.IsDone()) {
        return false;
    }
    BRepBuilderAPI_MakeWire wire(edge.Edge());
    if (!wire.IsDone()) {
        return false;
    }
    theGuide = wire.Wire();
    return true;
}

// Everything about the sweep that is the same for every section.
struct SweepPlan
{
    TopoDS_Wire path;
    TopoDS_Wire guide;                 // null unless there is a twist
    gp_Pnt      start;                 // first point of the path: the taper scales about it
    gp_Ax2      frame;                 // read only when isParallel
    bool        isParallel  = false;
    // How far a taper pushes the walls sideways over the whole path, in
    // millimetres. Zero means no taper. The scale each section needs is
    // worked out from this and its own distance from the path, so every
    // wall leaves at the same angle rather than every wall being scaled
    // by the same factor.
    double      taperOffset = 0.0;
};

// One closed section carried along the path. A face's holes come through
// here too, one wire at a time, and are cut out afterwards.
bool SweepSection(const SweepPlan&    thePlan,
                  const TopoDS_Wire&  theSection,
                  TopoDS_Shape&       theResult,
                  std::string&        theError)
{
    BRepOffsetAPI_MakePipeShell pipe(thePlan.path);

    // Corners are mitred rather than swept through. The default mode
    // folds the shell back on itself at a sharp turn and still reports
    // success -- an L-shaped path came back with half the volume it
    // should have had.
    pipe.SetTransitionMode(BRepBuilderAPI_RightCorner);

    if (thePlan.isParallel) {
        pipe.SetMode(thePlan.frame);
    } else {
        // Corrected Frenet, not plain Frenet: the plain one flips the
        // section over wherever the path's curvature dies, putting a fold
        // in an otherwise innocent sweep.
        pipe.SetMode(Standard_False);
    }

    if (!thePlan.guide.IsNull()) {
        // Standard_False asks for the section to be matched to the guide
        // by the plane square to the path, which is how the guide was
        // built. Matching by length ratio instead quietly skews the
        // sections on a curved path -- a round profile, which a twist
        // cannot possibly change, came out 1% short of its own volume.
        pipe.SetMode(thePlan.guide, Standard_False, BRepFill_NoContact);
    }

    if (std::fabs(thePlan.taperOffset) > kMinSweep) {
        double radius = 0.0;
        if (!NearestDistance(thePlan.start, theSection, radius) || radius < kMinSweep) {
            theError = "the path starts on the profile's own outline -- a taper needs a "
                       "distance to work from";
            return false;
        }

        const double scale = 1.0 + thePlan.taperOffset / radius;
        if (scale < kMinTaperScale) {
            theError = "the taper angle closes the profile off before the end of the path";
            return false;
        }

        // The law's own parameter range is mapped onto the path, so [0,1]
        // reads as "start of the path" to "end of it" whatever the path
        // is made of. OCCT scales each section about the path point it
        // sits at, which is why the scale is worked out from the
        // section's distance to that point.
        Handle(Law_Linear) law = new Law_Linear();
        law->Set(0.0, 1.0, 1.0, scale);
        pipe.SetLaw(theSection, law, Standard_False, Standard_False);
    } else {
        // No location vertex: OCCT works out where along the path the
        // section sits, which is what lets a path pass through the middle
        // of the profile and sweep both ways from it.
        pipe.Add(theSection, Standard_False, Standard_False);
    }

    if (!pipe.IsReady()) {
        theError = "the sweep has no profile to carry along the path";
        return false;
    }

    pipe.Build();
    if (!pipe.IsDone()) {
        theError = PipeErrorText(pipe.GetStatus());
        return false;
    }
    if (!pipe.MakeSolid()) {
        theError = "the swept surface would not close into a solid";
        return false;
    }

    theResult = pipe.Shape();
    if (IsEmptyShape(theResult)) {
        theError = "the sweep produced no solid";
        return false;
    }
    return true;
}

// One region of the sketch swept along the path, holes and all.
bool SweepFace(const SweepPlan&   thePlan,
               const TopoDS_Face& theFace,
               TopoDS_Shape&      theResult,
               std::string&       theError)
{
    const TopoDS_Wire outer = BRepTools::OuterWire(theFace);
    if (outer.IsNull()) {
        theError = "a profile region has no outline to sweep";
        return false;
    }

    TopoDS_Shape solid;
    if (!SweepSection(thePlan, outer, solid, theError)) {
        return false;
    }

    // A region with a hole in it sweeps into a tube, so the hole is
    // carried along the same path and taken back out. Reusing the shared
    // boolean keeps every cut in the app going through one door.
    std::vector<TopoDS_Shape> holes;
    for (TopExp_Explorer explorer(theFace, TopAbs_WIRE); explorer.More(); explorer.Next()) {
        const TopoDS_Wire wire = TopoDS::Wire(explorer.Current());
        if (wire.IsSame(outer)) {
            continue;
        }
        TopoDS_Shape hole;
        if (!SweepSection(thePlan, wire, hole, theError)) {
            return false;
        }
        holes.push_back(hole);
    }

    if (holes.empty()) {
        theResult = solid;
        return true;
    }
    return ApplyBooleanOperation(BooleanOp::Cut, solid, MakeCompoundOf(holes), theResult, theError);
}

// A pipe that folds through itself still comes back looking like a solid;
// the fold only shows up later, as a boolean that fails or a body whose
// volume is nonsense. Ask now, while there is still a name to put on it.
bool IsSoundSolid(const TopoDS_Shape& theShape)
{
    if (ShapeVolume(theShape) <= kMinSweep) {
        return false;
    }
    BRepAlgoAPI_Check check(theShape, Standard_False, Standard_True);
    check.Perform();
    return check.IsValid();
}

} // namespace

// ---- orientation ----

const std::vector<std::string>& SweepOrientationNames()
{
    static const std::vector<std::string> names = {"Perpendicular", "Parallel"};
    return names;
}

std::string SweepOrientationName(SweepOrientation theOrientation)
{
    const std::vector<std::string>& names = SweepOrientationNames();
    const std::size_t index = static_cast<std::size_t>(theOrientation);
    return index < names.size() ? names[index] : names.front();
}

SweepOrientation SweepOrientationFromInt(int theValue)
{
    if (theValue < 0 || theValue > static_cast<int>(SweepOrientation::Parallel)) {
        return SweepOrientation::Perpendicular;
    }
    return static_cast<SweepOrientation>(theValue);
}

bool ParseSweepOrientation(const std::string& theText, SweepOrientation& theOrientation)
{
    const std::string wanted = ToLower(theText);
    if (wanted.empty()) {
        return false;
    }

    const std::vector<std::string>& names = SweepOrientationNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (ToLower(names[i]) == wanted) {
            theOrientation = SweepOrientationFromInt(static_cast<int>(i));
            return true;
        }
    }
    if (wanted.size() == 1 && wanted[0] >= '0' && wanted[0] <= '1') {
        theOrientation = SweepOrientationFromInt(wanted[0] - '0');
        return true;
    }
    return false;
}

// ---- Sweep ----

SweepFeature::SweepFeature(std::string theSketchName, std::string thePathSketchName)
    : myPathSketchName(std::move(thePathSketchName))
{
    SetSketchName(std::move(theSketchName));
}

bool SweepFeature::Compute(const ComputeContext& theContext,
                           const TopoDS_Shape&   theInput,
                           TopoDS_Shape&         theOutput,
                           std::string&          theError)
{
    if (myPathSketchName.empty()) {
        theError = "no path selected";
        return false;
    }
    if (myPathSketchName == SketchName()) {
        // Both come out of the same seam, so the path would be one of the
        // profile's own loops: coplanar with the section and sweeping it
        // through itself.
        theError = "the path must be a different sketch from the profile";
        return false;
    }
    if (std::fabs(myTaperDegrees) > kMaxTaperDegrees) {
        theError = "taper angle must be between -" + FormatNumber(kMaxTaperDegrees)
                 + " and " + FormatNumber(kMaxTaperDegrees) + " degrees";
        return false;
    }
    if (std::fabs(myTwistDegrees) > kMaxTwistDegrees) {
        theError = "twist angle must be between -" + FormatNumber(kMaxTwistDegrees)
                 + " and " + FormatNumber(kMaxTwistDegrees) + " degrees";
        return false;
    }

    std::vector<TopoDS_Face> faces;
    gp_Pln plane;
    if (!ResolveProfile(theContext, faces, plane, theError)) {
        return false;
    }

    TopoDS_Wire path;
    if (!ResolvePath(theContext, path, theError)) {
        return false;
    }

    TopoDS_Shape tool;
    try {
        const double length = ShapeLength(path);
        if (length < kMinSweep) {
            theError = "the path in '" + myPathSketchName + "' is too short to sweep along";
            return false;
        }

        TopoDS_Vertex head, tail;
        TopExp::Vertices(path, head, tail);
        if (head.IsNull() || tail.IsNull()) {
            theError = "the path in '" + myPathSketchName + "' has no ends to follow";
            return false;
        }
        const bool isClosed = head.IsSame(tail) == Standard_True;

        // The sweep always runs AWAY from the profile: it makes the twist
        // turn the same way whichever way round the path happens to have
        // been drawn, and it is the only direction a taper grows in.
        if (!isClosed
            && plane.Distance(BRep_Tool::Pnt(tail)) < plane.Distance(BRep_Tool::Pnt(head))) {
            TopoDS_Wire reversed;
            if (!ReversePath(path, reversed)) {
                theError = "could not follow the path in '" + myPathSketchName + "'";
                return false;
            }
            path = reversed;
        }

        std::vector<PathSample> samples;
        if (!SamplePath(path, samples)) {
            theError = "the path in '" + myPathSketchName + "' could not be followed";
            return false;
        }

        // A path lying in the profile's own plane sweeps the section
        // through itself and leaves a flat nothing behind. Signed
        // distances, because which SIDE each sample is on is the next
        // question: a path with samples on both sides runs through the
        // profile rather than away from it.
        const gp_Vec normal(plane.Axis().Direction());
        const double planeTolerance = std::max(kOnPlane, length * 1.0e-9);
        bool leavesPlane = false;
        bool above = false;
        bool below = false;
        for (const PathSample& sample : samples) {
            const double offset = gp_Vec(plane.Location(), sample.point).Dot(normal);
            if (offset > planeTolerance) {
                above = true;
                leavesPlane = true;
            } else if (offset < -planeTolerance) {
                below = true;
                leavesPlane = true;
            }
        }
        if (!leavesPlane) {
            theError = "the path lies in the profile's own plane -- a sweep needs a path "
                       "that leaves it";
            return false;
        }
        const bool crossesProfile = above && below;

        SweepPlan plan;
        plan.path       = path;
        plan.start      = samples.front().point;
        plan.isParallel = (myOrientation == SweepOrientation::Parallel);
        plan.frame      = gp_Ax2(plane.Location(), plane.Axis().Direction(),
                                 plane.XAxis().Direction());

        if (plan.isParallel) {
            // A parallel sweep carries the section along without turning
            // it, so the solid is only as thick as the path's travel
            // ACROSS the section. Where the path turns to run along the
            // profile's plane it contributes no thickness at all, and the
            // sweep comes back as a valid-looking solid with a leg
            // missing -- which is exactly the kind of quiet wrongness
            // worth an error message.
            const double leading = gp_Vec(samples.front().tangent).Dot(normal);
            for (const PathSample& sample : samples) {
                const double along = gp_Vec(sample.tangent).Dot(normal);
                if (std::fabs(along) < kMinTangentCosine || along * leading < 0.0) {
                    theError = "with the parallel orientation the path must not turn to run "
                               "along the profile's plane -- use perpendicular instead";
                    return false;
                }
            }
        }

        if (std::fabs(myTaperDegrees) > kMinSweep) {
            if (isClosed) {
                theError = "a closed path cannot be tapered -- the profile would have to be "
                           "two sizes at once where the path meets itself";
                return false;
            }
            if (crossesProfile) {
                theError = "a taper needs the path to run away from the profile, not through it";
                return false;
            }
            const double startsAt = plane.Distance(samples.front().point);
            if (startsAt > planeTolerance) {
                theError = "a taper needs the path to start at the profile -- this one begins "
                         + FormatNumber(startsAt) + " mm away";
                return false;
            }
            // Degrees in, radians out: the one place this feature converts.
            plan.taperOffset = std::tan(myTaperDegrees * kPi / 180.0) * length;
        }

        if (std::fabs(myTwistDegrees) > kMinSweep) {
            if (isClosed) {
                theError = "a closed path cannot be twisted -- the profile would not meet "
                           "itself again where the path closes";
                return false;
            }
            // Degrees in, radians out.
            const double twist = myTwistDegrees * kPi / 180.0;
            if (!BuildTwistGuide(samples, twist, length * 0.1, plan.guide)) {
                theError = "could not twist the profile along this path";
                return false;
            }
        }

        std::vector<TopoDS_Shape> solids;
        solids.reserve(faces.size());
        for (const TopoDS_Face& face : faces) {
            TopoDS_Shape solid;
            if (!SweepFace(plan, face, solid, theError)) {
                return false;
            }
            // Checked one region at a time on purpose: two regions that
            // touch would read as interfering if they were checked as one
            // compound, and that is not what is being asked.
            if (!IsSoundSolid(solid)) {
                theError = "the sweep folds through itself -- the path may turn tighter than "
                           "the profile is wide";
                return false;
            }
            solids.push_back(solid);
        }

        if (!FuseProfileSolids(solids, tool, theError)) {
            return false;
        }
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "sweep failed");
        return false;
    }

    if (IsEmptyShape(tool)) {
        theError = "sweep produced no solid";
        return false;
    }

    return ApplyBooleanOperation(Operation(), theInput, tool, theOutput, theError);
}

bool SweepFeature::ResolvePath(const ComputeContext& theContext,
                               TopoDS_Wire&          thePath,
                               std::string&          theError) const
{
    // Same shape and the same tone as ResolveProfile: a missing path
    // sketch and a path drawn away are ordinary mistakes, not kernel
    // errors, and are worded to be shown to the user as they are.
    Feature* feature = theContext.FindFeature(myPathSketchName);
    if (feature == nullptr) {
        theError = "path sketch '" + myPathSketchName + "' is not earlier in the timeline";
        return false;
    }

    ProfileProvider* provider = AsProfileProvider(feature);
    if (provider == nullptr) {
        theError = "'" + myPathSketchName + "' is not a sketch";
        return false;
    }

    std::vector<TopoDS_Wire> wires;
    try {
        wires = provider->ProfileWires();
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "could not read the path sketch");
        return false;
    }

    wires.erase(std::remove_if(wires.begin(),
                               wires.end(),
                               [](const TopoDS_Wire& theWire) { return theWire.IsNull(); }),
                wires.end());

    if (wires.empty()) {
        // The seam hands over closed wires and nothing else, so an open
        // chain -- the usual Fusion sweep path -- never arrives here. Say
        // what will work rather than leaving the user to guess.
        theError = "path sketch '" + myPathSketchName + "' has no closed path -- a sweep can "
                   "only follow a closed loop for now";
        return false;
    }
    if (wires.size() > 1) {
        // Picking one would be picking the user's design for them, and
        // the largest is no more likely to be meant than any other.
        theError = "path sketch '" + myPathSketchName + "' holds "
                 + std::to_string(wires.size())
                 + " separate loops -- a sweep follows exactly one";
        return false;
    }

    thePath = wires.front();
    return true;
}

std::unique_ptr<Feature> SweepFeature::Clone() const
{
    auto copy = std::make_unique<SweepFeature>();
    copy->myPathSketchName = myPathSketchName;
    copy->myOrientation    = myOrientation;
    copy->myTaperDegrees   = myTaperDegrees;
    copy->myTwistDegrees   = myTwistDegrees;
    // Carries the sketch, the picked regions, the operation and the base
    // fields -- undo restores the timeline from clones, so anything
    // missed here is lost the moment the user presses Ctrl+Z.
    CopyProfileTo(*copy);
    return copy;
}

std::vector<Parameter> SweepFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeString("Path", myPathSketchName));
    parameters.push_back(Parameter::MakeString("Orientation", SweepOrientationName(myOrientation)));
    parameters.push_back(Parameter::MakeDouble("Taper Angle", myTaperDegrees, "deg"));
    parameters.push_back(Parameter::MakeDouble("Twist Angle", myTwistDegrees, "deg"));
    AppendCommonParameters(parameters);
    return parameters;
}

bool SweepFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Path") {
        myPathSketchName = theParameter.stringValue;
        return true;
    }
    if (theParameter.name == "Orientation") {
        SweepOrientation orientation = myOrientation;
        if (!ParseSweepOrientation(theParameter.stringValue, orientation)) {
            return false;
        }
        myOrientation = orientation;
        return true;
    }
    if (theParameter.name == "Taper Angle") {
        myTaperDegrees = theParameter.doubleValue;
        return true;
    }
    if (theParameter.name == "Twist Angle") {
        myTwistDegrees = theParameter.doubleValue;
        return true;
    }
    return ApplyCommonParameter(theParameter);
}

} // namespace lcad
