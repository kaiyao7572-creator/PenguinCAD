#include "features/ModifyFeatures.h"

#include "features/FeatureUtils.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace lcad {

namespace {

constexpr double kMinSize = 1.0e-7;

// Tolerance BRepOffsetAPI wants for the offset construction; 1 micron is
// the value OCCT's own samples use for millimetre models.
constexpr double kOffsetTolerance = 1.0e-3;

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

// Unique edges: TopExp_Explorer would hand back every shared edge once
// per adjacent face, and adding an edge twice confuses the fillet
// builder's contour bookkeeping.
void CollectEdges(const TopoDS_Shape& theShape, std::vector<TopoDS_Edge>& theEdges)
{
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(theShape, TopAbs_EDGE, map);
    for (Standard_Integer i = 1; i <= map.Extent(); ++i) {
        const TopoDS_Edge edge = TopoDS::Edge(map(i));
        // Seam and pole edges (a sphere's, for instance) have no real
        // geometry to round off and make the builder fail outright.
        if (edge.IsNull() || BRep_Tool::Degenerated(edge)) {
            continue;
        }
        theEdges.push_back(edge);
    }
}

double ShapeVolume(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return 0.0;
    }
    GProp_GProps properties;
    BRepGProp::VolumeProperties(theShape, properties);
    return properties.Mass();
}

bool IsPlanarFace(const TopoDS_Face& theFace)
{
    try {
        BRepAdaptor_Surface surface(theFace, Standard_False);
        return surface.GetType() == GeomAbs_Plane;
    } catch (const Standard_Failure&) {
        return false;
    }
}

// The face the shell opens through, chosen by rule so the choice
// survives an upstream parameter change.
bool PickOpenFace(const TopoDS_Shape& theSolid, ShellOpening theOpening, TopoDS_Face& theFace)
{
    if (theOpening == ShellOpening::None) {
        return false;
    }

    bool   found      = false;
    bool   bestPlanar = false;
    double bestScore  = 0.0;

    for (TopExp_Explorer explorer(theSolid, TopAbs_FACE); explorer.More(); explorer.Next()) {
        const TopoDS_Face face = TopoDS::Face(explorer.Current());
        if (face.IsNull()) {
            continue;
        }

        GProp_GProps properties;
        BRepGProp::SurfaceProperties(face, properties);
        const double area = properties.Mass();
        if (area <= kMinSize) {
            continue;
        }

        double score = 0.0;
        switch (theOpening) {
            case ShellOpening::Top:
                score = properties.CentreOfMass().Z();
                break;
            case ShellOpening::Bottom:
                score = -properties.CentreOfMass().Z();
                break;
            case ShellOpening::Largest:
            case ShellOpening::None:
                score = area;
                break;
        }

        // A flat lid is nearly always what "the top face" means, so any
        // planar candidate beats a curved one outright.
        const bool planar = theOpening == ShellOpening::Largest ? bestPlanar : IsPlanarFace(face);
        const bool better = !found
                         || (planar && !bestPlanar)
                         || (planar == bestPlanar && score > bestScore);
        if (better) {
            found = true;
            bestPlanar = planar;
            bestScore = score;
            theFace = face;
        }
    }

    return found;
}

} // namespace

// ---- ModifyFeature ----

bool ModifyFeature::Compute(const ComputeContext& theContext,
                            const TopoDS_Shape&   theInput,
                            TopoDS_Shape&         theOutput,
                            std::string&          theError)
{
    (void)theContext;

    if (!HasSolid(theInput)) {
        theError = EmptyInputError();
        return false;
    }

    const std::vector<TopoDS_Shape> solids = CollectSolids(theInput);
    std::vector<TopoDS_Shape> results;
    results.reserve(solids.size());

    for (const TopoDS_Shape& solid : solids) {
        TopoDS_Shape modified;
        try {
            if (!ModifySolid(solid, modified, theError)) {
                if (theError.empty()) {
                    theError = TypeName() + " failed";
                }
                return false;
            }
        } catch (const Standard_Failure& failure) {
            theError = OcctMessage(failure, TypeName() + " failed");
            return false;
        }

        if (IsEmptyShape(modified)) {
            theError = TypeName() + " produced no geometry";
            return false;
        }
        results.push_back(modified);
    }

    theOutput = MakeCompoundOf(results);
    return true;
}

// ---- Fillet ----

FilletFeature::FilletFeature(double theRadius)
    : myRadius(theRadius)
{
}

std::unique_ptr<Feature> FilletFeature::Clone() const
{
    auto copy = std::make_unique<FilletFeature>(myRadius);
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> FilletFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeDouble("Radius", myRadius));
    return parameters;
}

bool FilletFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Radius") {
        myRadius = theParameter.doubleValue;
        return true;
    }
    return false;
}

std::string FilletFeature::EmptyInputError() const
{
    return "nothing to fillet -- create a body first";
}

bool FilletFeature::ModifySolid(const TopoDS_Shape& theSolid,
                                TopoDS_Shape&       theResult,
                                std::string&        theError) const
{
    if (myRadius < kMinSize) {
        theError = "fillet radius must be greater than zero";
        return false;
    }

    const std::string tooLarge =
        "fillet radius too large for this geometry (" + FormatNumber(myRadius) + " mm)";

    try {
        std::vector<TopoDS_Edge> edges;
        CollectEdges(theSolid, edges);
        if (edges.empty()) {
            theError = "this body has no edges to fillet";
            return false;
        }

        BRepFilletAPI_MakeFillet fillet(theSolid);
        for (const TopoDS_Edge& edge : edges) {
            // Adding an edge already swallowed by a tangent contour would
            // register it twice and make the builder fail.
            if (fillet.Contour(edge) != 0) {
                continue;
            }
            fillet.Add(myRadius, edge);
        }

        if (fillet.NbContours() == 0) {
            theError = "this body has no edges to fillet";
            return false;
        }

        fillet.Build();
        if (!fillet.IsDone()) {
            theError = tooLarge;
            return false;
        }

        theResult = fillet.Shape();
        return true;
    } catch (const Standard_Failure&) {
        // Every OCCT failure in here reduces in practice to the same
        // thing: the requested round doesn't fit the geometry.
        theError = tooLarge;
        return false;
    }
}

// ---- Chamfer ----

ChamferFeature::ChamferFeature(double theDistance)
    : myDistance(theDistance)
{
}

std::unique_ptr<Feature> ChamferFeature::Clone() const
{
    auto copy = std::make_unique<ChamferFeature>(myDistance);
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> ChamferFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeDouble("Distance", myDistance));
    return parameters;
}

bool ChamferFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Distance") {
        myDistance = theParameter.doubleValue;
        return true;
    }
    return false;
}

std::string ChamferFeature::EmptyInputError() const
{
    return "nothing to chamfer -- create a body first";
}

bool ChamferFeature::ModifySolid(const TopoDS_Shape& theSolid,
                                 TopoDS_Shape&       theResult,
                                 std::string&        theError) const
{
    if (myDistance < kMinSize) {
        theError = "chamfer distance must be greater than zero";
        return false;
    }

    const std::string tooLarge =
        "chamfer distance too large for this geometry (" + FormatNumber(myDistance) + " mm)";

    try {
        std::vector<TopoDS_Edge> edges;
        CollectEdges(theSolid, edges);
        if (edges.empty()) {
            theError = "this body has no edges to chamfer";
            return false;
        }

        BRepFilletAPI_MakeChamfer chamfer(theSolid);
        for (const TopoDS_Edge& edge : edges) {
            if (chamfer.Contour(edge) != 0) {
                continue;
            }
            chamfer.Add(myDistance, edge);
        }

        if (chamfer.NbContours() == 0) {
            theError = "this body has no edges to chamfer";
            return false;
        }

        chamfer.Build();
        if (!chamfer.IsDone()) {
            theError = tooLarge;
            return false;
        }

        theResult = chamfer.Shape();
        return true;
    } catch (const Standard_Failure&) {
        theError = tooLarge;
        return false;
    }
}

// ---- Shell ----

const std::vector<std::string>& ShellOpeningNames()
{
    static const std::vector<std::string> names = {"None", "Top", "Bottom", "Largest"};
    return names;
}

std::string ShellOpeningName(ShellOpening theOpening)
{
    const std::vector<std::string>& names = ShellOpeningNames();
    const std::size_t index = static_cast<std::size_t>(theOpening);
    return index < names.size() ? names[index] : names.front();
}

ShellOpening ShellOpeningFromInt(int theValue)
{
    if (theValue < 0 || theValue > static_cast<int>(ShellOpening::Largest)) {
        return ShellOpening::Top;
    }
    return static_cast<ShellOpening>(theValue);
}

bool ParseShellOpening(const std::string& theText, ShellOpening& theOpening)
{
    const std::string wanted = ToLower(theText);
    if (wanted.empty()) {
        return false;
    }

    const std::vector<std::string>& names = ShellOpeningNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (ToLower(names[i]) == wanted) {
            theOpening = ShellOpeningFromInt(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

ShellFeature::ShellFeature(double theThickness, ShellOpening theOpening)
    : myThickness(theThickness), myOpening(theOpening)
{
}

std::unique_ptr<Feature> ShellFeature::Clone() const
{
    auto copy = std::make_unique<ShellFeature>(myThickness, myOpening);
    copy->myIsOutward = myIsOutward;
    CopyBaseTo(*copy);
    return copy;
}

std::vector<Parameter> ShellFeature::Parameters() const
{
    std::vector<Parameter> parameters;
    parameters.push_back(Parameter::MakeDouble("Thickness", myThickness));
    parameters.push_back(Parameter::MakeString("Open Face", ShellOpeningName(myOpening)));
    parameters.push_back(Parameter::MakeBool("Outward", myIsOutward));
    return parameters;
}

bool ShellFeature::SetParameter(const Parameter& theParameter)
{
    if (theParameter.name == "Thickness") {
        myThickness = theParameter.doubleValue;
        return true;
    }
    if (theParameter.name == "Open Face") {
        ShellOpening opening = myOpening;
        if (!ParseShellOpening(theParameter.stringValue, opening)) {
            return false;
        }
        myOpening = opening;
        return true;
    }
    if (theParameter.name == "Outward") {
        myIsOutward = theParameter.boolValue;
        return true;
    }
    return false;
}

std::string ShellFeature::EmptyInputError() const
{
    return "nothing to shell -- create a body first";
}

bool ShellFeature::ModifySolid(const TopoDS_Shape& theSolid,
                               TopoDS_Shape&       theResult,
                               std::string&        theError) const
{
    if (myThickness < kMinSize) {
        theError = "shell thickness must be greater than zero";
        return false;
    }

    const std::string tooThick =
        "shell thickness too large for this geometry (" + FormatNumber(myThickness) + " mm)";

    try {
        TopTools_ListOfShape closingFaces;
        TopoDS_Face openFace;
        if (PickOpenFace(theSolid, myOpening, openFace)) {
            closingFaces.Append(openFace);
        }

        // Negative offsets eat into the body, which is what a wall
        // thickness means; positive ones grow a skin around it.
        const double offset = myIsOutward ? myThickness : -myThickness;

        BRepOffsetAPI_MakeThickSolid maker;
        maker.MakeThickSolidByJoin(theSolid, closingFaces, offset, kOffsetTolerance);
        maker.Build();
        if (!maker.IsDone()) {
            theError = tooThick;
            return false;
        }

        // A thickness the body can't take has three different outcomes
        // in OCCT, and only one of them is "not done": it also hands back
        // walls that have collapsed through each other, or the untouched
        // input. So the result is measured rather than trusted.
        const TopoDS_Shape result = maker.Shape();
        if (IsEmptyShape(result)) {
            theError = tooThick;
            return false;
        }

        BRepCheck_Analyzer analyzer(result);
        if (!analyzer.IsValid()) {
            theError = tooThick;
            return false;
        }

        const double volume = ShapeVolume(result);
        if (volume <= kMinSize) {
            theError = tooThick;
            return false;
        }
        if (!myIsOutward) {
            // Hollowing leaves the wall material only, so the result has
            // to be measurably lighter than what went in. Equal volume
            // means nothing was removed.
            const double before = ShapeVolume(theSolid);
            if (volume >= before - std::max(1.0e-9, before * 1.0e-6)) {
                theError = tooThick;
                return false;
            }
        }

        theResult = result;
        return true;
    } catch (const Standard_Failure&) {
        theError = tooThick;
        return false;
    }
}

} // namespace lcad
