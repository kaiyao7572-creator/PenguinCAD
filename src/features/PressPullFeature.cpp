#include "features/PressPullFeature.h"

#include "features/FeatureUtils.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GProp_GProps.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <cstddef>

namespace lcad {

namespace {

// Below this the move is a no-op rather than a feature, and a zero-length
// prism is a degenerate shape the booleans would choke on.
constexpr double kMinDistance = 1.0e-7;

// Linear tolerance handed to the offset builder. Tighter than the
// millimetre-scale geometry this app works in by several orders, so it
// never rounds a real feature away.
constexpr double kOffsetTolerance = 1.0e-6;

// A solid smaller than this is the kernel's way of saying the faces met
// and cancelled the body out, not a body. Pushing a 30mm box's top face
// in by exactly 30 returns a "valid" solid of zero volume rather than
// failing, so the check has to be here.
constexpr double kMinVolume = 1.0e-7;

std::string OcctMessage(const Standard_Failure& theFailure, const std::string& theFallback)
{
    const Standard_CString message = theFailure.GetMessageString();
    return (message != nullptr && message[0] != '\0') ? std::string(message) : theFallback;
}

std::vector<GeometryRef> WithoutNulls(std::vector<GeometryRef> theRefs)
{
    std::vector<GeometryRef> kept;
    kept.reserve(theRefs.size());
    for (GeometryRef& ref : theRefs) {
        if (!ref.IsNull()) {
            kept.push_back(std::move(ref));
        }
    }
    return kept;
}

double VolumeOf(const TopoDS_Shape& theShape)
{
    GProp_GProps properties;
    BRepGProp::VolumeProperties(theShape, properties);
    return properties.Mass();
}

// The direction that points OUT of the solid.
//
// A face's surface normal points out of its own parameterisation, not out
// of the body: OCCT records which of the two the solid actually uses in
// the face's orientation. Ignoring that pushes half the faces of a box the
// wrong way, and the half depends on how the box was built -- so it would
// look like an intermittent bug rather than a missing line.
bool OutwardPlaneNormal(const TopoDS_Face& theFace, gp_Dir& theResult, std::string& theError)
{
    BRepAdaptor_Surface surface(theFace);
    if (surface.GetType() != GeomAbs_Plane) {
        theError = "that face is not flat, so it cannot be swept";
        return false;
    }
    gp_Dir normal = surface.Plane().Axis().Direction();
    if (theFace.Orientation() == TopAbs_REVERSED) {
        normal.Reverse();
    }
    theResult = normal;
    return true;
}

bool IsPlanar(const TopoDS_Face& theFace)
{
    BRepAdaptor_Surface surface(theFace);
    return surface.GetType() == GeomAbs_Plane;
}

// Refuse a move that would turn a cylindrical face inside out.
//
// Measured, not assumed: a 10mm-radius cylinder wall pushed in 12mm comes
// back from the offset builder as a perfectly valid, checkable solid of
// radius 2 -- the radius MIRRORED through the axis rather than an error.
// Nothing downstream can tell that from a correct answer, which is
// exactly the silent-wrong-model failure this codebase refuses to have.
//
// Which way the radius travels follows the same rule as everything else
// here: the face moves the way it looks. A boss's wall looks away from
// its axis so it fattens; a hole's wall is REVERSED and looks at the
// axis, so the same positive distance closes the hole.
bool SurvivesTheMove(const TopoDS_Face& theFace, double theDistance, std::string& theError)
{
    BRepAdaptor_Surface surface(theFace);
    if (surface.GetType() != GeomAbs_Cylinder) {
        return true;
    }

    const double radius = surface.Cylinder().Radius();
    const double signedMove = theFace.Orientation() == TopAbs_REVERSED ? -theDistance : theDistance;
    if (radius + signedMove <= Precision::Confusion()) {
        theError = "moving " + FormatNumber(std::fabs(theDistance)) + "mm would collapse a "
                   + FormatNumber(radius) + "mm radius to nothing";
        return false;
    }
    return true;
}

// Move the faces and let the kernel rebuild the solid around them.
//
// This is the whole feature in one call: BRepOffset_MakeOffset with a
// global offset of zero and a per-face offset on the picks slides each
// picked face along its own normal and re-intersects its neighbours.
// Three things about it were established by probing, not by reading:
//
//  1. GeomAbs_Intersection is mandatory. With GeomAbs_Arc the builder
//     throws "BRep_API: command not done"; with GeomAbs_Tangent it
//     reports not-done with error code 0. Only Intersection builds
//     anything, and it is also the join that EXTENDS neighbouring walls
//     rather than rolling a fillet between them -- which is the Fusion
//     behaviour wanted here.
//  2. It must be handed a bare TopoDS_Solid. Given a compound -- which is
//     what every boolean in this codebase returns -- it produces a closed
//     shell with no solid in it, and the document then holds a body with
//     no volume.
//  3. It cannot cope with two coplanar faces that share a seam edge, as a
//     fuse of two boxes leaves behind. That case returns a null shape.
bool OffsetFacesOfSolid(const TopoDS_Shape&             theSolid,
                        const std::vector<TopoDS_Face>& theFaces,
                        double                          theDistance,
                        TopoDS_Shape&                   theResult,
                        std::string&                    theError)
{
    BRepOffset_MakeOffset builder;
    builder.Initialize(theSolid, 0.0, kOffsetTolerance, BRepOffset_Skin,
                       Standard_False, Standard_False, GeomAbs_Intersection,
                       Standard_False, Standard_False);
    for (const TopoDS_Face& face : theFaces) {
        builder.SetOffsetOnFace(face, theDistance);
    }

    builder.MakeOffsetShape();
    if (!builder.IsDone()) {
        theError = "the body cannot be rebuilt around a face moved that far";
        return false;
    }

    const TopoDS_Shape offset = builder.Shape();
    if (offset.IsNull() || !HasSolid(offset) || VolumeOf(offset) <= kMinVolume) {
        theError = "moving the face that far leaves no body behind";
        return false;
    }

    theResult = offset;
    return true;
}

// The pre-offset behaviour, kept as a fallback: sweep each face along its
// normal and fuse or cut the prism.
//
// It leaves a step where the offset builder would have extended the
// neighbouring walls, so it is strictly the worse answer -- but it copes
// with the seamed shapes the builder refuses outright, and a step is
// better than a face that cannot be moved at all. Flat faces only,
// because a swept cylinder is a tube rather than a wider cylinder and
// fusing one would be a wrong model rather than a coarse one.
bool SweepFacesOfSolid(const TopoDS_Shape&             theSolid,
                       const std::vector<TopoDS_Face>& theFaces,
                       double                          theDistance,
                       TopoDS_Shape&                   theResult,
                       std::string&                    theError)
{
    TopoDS_Shape running = theSolid;
    for (const TopoDS_Face& face : theFaces) {
        gp_Dir normal;
        if (!OutwardPlaneNormal(face, normal, theError)) {
            return false;
        }

        BRepPrimAPI_MakePrism prism(face, gp_Vec(normal) * theDistance);
        prism.Build();
        if (!prism.IsDone()) {
            theError = "could not sweep that face";
            return false;
        }

        // Out adds, in removes. One signed distance rather than a
        // direction plus a mode, because that is how the drag feels.
        const BooleanOp operation = theDistance > 0.0 ? BooleanOp::Join : BooleanOp::Cut;
        TopoDS_Shape combined;
        if (!ApplyBooleanOperation(operation, running, prism.Shape(), combined, theError)) {
            return false;
        }
        running = combined;
    }

    if (IsEmptyShape(running) || VolumeOf(running) <= kMinVolume) {
        theError = "moving the face that far leaves no body behind";
        return false;
    }
    theResult = running;
    return true;
}

bool MoveFacesOfSolid(const TopoDS_Shape&             theSolid,
                      const std::vector<TopoDS_Face>& theFaces,
                      double                          theDistance,
                      TopoDS_Shape&                   theResult,
                      std::string&                    theError)
{
    for (const TopoDS_Face& face : theFaces) {
        if (!SurvivesTheMove(face, theDistance, theError)) {
            return false;
        }
    }

    std::string offsetError;
    if (OffsetFacesOfSolid(theSolid, theFaces, theDistance, theResult, offsetError)) {
        return true;
    }

    bool allPlanar = true;
    for (const TopoDS_Face& face : theFaces) {
        allPlanar = allPlanar && IsPlanar(face);
    }
    std::string sweepError;
    if (allPlanar && SweepFacesOfSolid(theSolid, theFaces, theDistance, theResult, sweepError)) {
        return true;
    }

    // The offset builder's complaint is the one worth showing: the sweep
    // is a consolation prize and its failure says less about what the
    // user did wrong.
    theError = offsetError;
    return false;
}

std::string LostFaceMessage(std::size_t theIndex, std::size_t theCount)
{
    if (theCount <= 1) {
        return "the face this was built on no longer exists -- re-select it";
    }
    return "face " + std::to_string(theIndex + 1) + " of " + std::to_string(theCount)
           + " no longer exists -- re-select the faces";
}

} // namespace

PressPullFeature::PressPullFeature(GeometryRef theFace, double theDistance)
    : myDistance(theDistance)
{
    SetFace(std::move(theFace));
}

PressPullFeature::PressPullFeature(std::vector<GeometryRef> theFaces, double theDistance)
    : myFaces(WithoutNulls(std::move(theFaces)))
    , myDistance(theDistance)
{
}

void PressPullFeature::SetFaces(std::vector<GeometryRef> theFaces)
{
    myFaces = WithoutNulls(std::move(theFaces));
}

const GeometryRef& PressPullFeature::Face() const
{
    static const GeometryRef nothing;
    return myFaces.empty() ? nothing : myFaces.front();
}

void PressPullFeature::SetFace(GeometryRef theFace)
{
    myFaces.clear();
    if (!theFace.IsNull()) {
        myFaces.push_back(std::move(theFace));
    }
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
    if (myFaces.empty()) {
        theError = "no face selected";
        return false;
    }
    if (IsEmptyShape(theInput)) {
        theError = "nothing to press or pull -- there is no body yet";
        return false;
    }

    try {
        // Resolved against the INPUT shape, which is the model as it
        // stands at this point in the timeline. Refusing beats guessing: a
        // press/pull that silently moved to a neighbouring face after an
        // upstream edit would be a wrong model nobody notices. One lost
        // face fails the whole feature rather than moving the survivors,
        // for the same reason a profile feature does.
        std::vector<TopoDS_Face> faces;
        faces.reserve(myFaces.size());
        for (std::size_t i = 0; i < myFaces.size(); ++i) {
            const GeometryRef& ref = myFaces[i];
            if (ref.type != EntityType::BRepFace) {
                theError = "press/pull moves faces -- that is not one";
                return false;
            }

            TopoDS_Shape resolved;
            if (!ResolveGeometryRefInShape(theInput, ref, resolved)
                || resolved.ShapeType() != TopAbs_FACE) {
                theError = LostFaceMessage(i, myFaces.size());
                return false;
            }

            const TopoDS_Face face = TopoDS::Face(resolved);
            for (const TopoDS_Face& seen : faces) {
                if (seen.IsSame(face)) {
                    theError = "the same face is selected twice";
                    return false;
                }
            }
            faces.push_back(face);
        }

        // The offset builder works on one solid at a time (a compound
        // makes it produce a shell with no volume), so the picks are
        // sorted into the bodies that own them and every other body is
        // carried through untouched.
        const std::vector<TopoDS_Shape> solids = CollectSolids(theInput);
        if (solids.empty()) {
            theError = "press/pull needs a solid body";
            return false;
        }

        std::vector<std::vector<TopoDS_Face>> picksPerSolid(solids.size());
        for (const TopoDS_Face& face : faces) {
            std::size_t owner = solids.size();
            for (std::size_t s = 0; s < solids.size() && owner == solids.size(); ++s) {
                for (TopExp_Explorer explorer(solids[s], TopAbs_FACE); explorer.More();
                     explorer.Next()) {
                    if (explorer.Current().IsSame(face)) {
                        owner = s;
                        break;
                    }
                }
            }
            if (owner == solids.size()) {
                theError = "that face belongs to no solid body";
                return false;
            }
            picksPerSolid[owner].push_back(face);
        }

        std::vector<TopoDS_Shape> bodies;
        bodies.reserve(solids.size());
        for (std::size_t s = 0; s < solids.size(); ++s) {
            if (picksPerSolid[s].empty()) {
                bodies.push_back(solids[s]);
                continue;
            }
            TopoDS_Shape moved;
            if (!MoveFacesOfSolid(solids[s], picksPerSolid[s], myDistance, moved, theError)) {
                return false;
            }
            bodies.push_back(moved);
        }

        theOutput = MakeCompoundOf(bodies);
        if (IsEmptyShape(theOutput)) {
            theError = "moving the face that far leaves no body behind";
            return false;
        }
        return true;
    } catch (const Standard_Failure& failure) {
        theError = OcctMessage(failure, "press/pull failed");
        return false;
    }
}

std::unique_ptr<Feature> PressPullFeature::Clone() const
{
    auto copy = std::make_unique<PressPullFeature>();
    copy->myFaces = myFaces;
    copy->myDistance = myDistance;
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> PressPullFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeDouble("Distance", myDistance));
    // The picks ride a plain string row, so which faces this was built on
    // is inspectable without a new widget -- the same bargain a profile
    // reference makes.
    parameters.push_back(Parameter::MakeString("Faces", EncodeGeometryRefs(myFaces)));
    return parameters;
}

bool PressPullFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Distance") {
        myDistance = theParameter.doubleValue;
        return true;
    }
    if (theParameter.name == "Faces") {
        std::vector<GeometryRef> parsed = DecodeGeometryRefs(theParameter.stringValue);
        if (parsed.empty()) {
            return false;  // refuse rather than quietly clearing the picks
        }
        myFaces = std::move(parsed);
        return true;
    }
    return false;
}

} // namespace lcad
