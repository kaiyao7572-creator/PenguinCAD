#include "core/GeometryRef.h"

#include "core/Body.h"

#include <BRepGProp.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>

#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

namespace lcad {

namespace {

// A match has to agree on size to within this fraction, and sit within
// this fraction of the body's bounding-box diagonal. Relative on purpose:
// a 2mm boss and a 2m weldment cannot share an absolute tolerance.
constexpr double kMeasureTolerance = 0.01;
constexpr double kPositionTolerance = 0.01;

// The runner-up has to be at least this much worse than the winner, or
// the reference is ambiguous and resolving refuses. A cube's six faces
// have identical areas, so without this a reference to one of them would
// silently resolve to whichever happened to be enumerated first.
constexpr double kAmbiguityMargin = 4.0;

bool MeasureAndCentre(const TopoDS_Shape& theShape,
                      EntityType          theType,
                      double&             theMeasure,
                      gp_Pnt&             theCentre)
{
    if (theShape.IsNull()) {
        return false;
    }
    try {
        GProp_GProps props;
        switch (theType) {
            case EntityType::BRepFace:
                BRepGProp::SurfaceProperties(theShape, props);
                break;
            case EntityType::BRepEdge:
                BRepGProp::LinearProperties(theShape, props);
                break;
            case EntityType::BRepVertex: {
                // A vertex has no extent, so mass properties report a zero
                // mass at the origin for every one of them -- which makes
                // all eight corners of a box look identical and resolving
                // refuse. Its position IS its identity, and that has to be
                // read off the vertex itself.
                if (theShape.ShapeType() != TopAbs_VERTEX) {
                    return false;
                }
                theMeasure = 0.0;
                theCentre = BRep_Tool::Pnt(TopoDS::Vertex(theShape));
                return true;
            }
            default:
                return false;
        }
        theMeasure = props.Mass();
        theCentre = props.CentreOfMass();
        return true;
    } catch (const Standard_Failure&) {
        return false;
    }
}

double DiagonalOf(const TopoDS_Shape& theShape)
{
    try {
        Bnd_Box box;
        BRepBndLib::Add(theShape, box);
        if (box.IsVoid()) {
            return 1.0;
        }
        double xMin, yMin, zMin, xMax, yMax, zMax;
        box.Get(xMin, yMin, zMin, xMax, yMax, zMax);
        const double diagonal = gp_Pnt(xMin, yMin, zMin).Distance(gp_Pnt(xMax, yMax, zMax));
        return diagonal > 1.0e-9 ? diagonal : 1.0;
    } catch (const Standard_Failure&) {
        return 1.0;
    }
}

// Deduplicated sub-shapes of a shape, the same way Body enumerates its
// own: once per shape, not once per use.
std::vector<TopoDS_Shape> SubShapesOf(const TopoDS_Shape& theShape, EntityType theType)
{
    std::vector<TopoDS_Shape> shapes;
    if (theShape.IsNull()) {
        return shapes;
    }
    TopAbs_ShapeEnum wanted = TopAbs_SHAPE;
    switch (theType) {
        case EntityType::BRepFace:   wanted = TopAbs_FACE;   break;
        case EntityType::BRepEdge:   wanted = TopAbs_EDGE;   break;
        case EntityType::BRepVertex: wanted = TopAbs_VERTEX; break;
        default:                     return shapes;
    }

    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(theShape, wanted, map);
    for (Standard_Integer i = 1; i <= map.Extent(); ++i) {
        shapes.push_back(map(i));
    }
    return shapes;
}

} // namespace

std::string GeometryRef::Encode() const
{
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "%s|%s|%.10g,%.10g,%.10g|%.10g",
                  EntityTypeName(type).c_str(), body.c_str(),
                  point.X(), point.Y(), point.Z(), measure);
    return std::string(buffer);
}

bool GeometryRef::Decode(const std::string& theText, GeometryRef& theResult)
{
    std::vector<std::string> parts;
    std::istringstream stream(theText);
    std::string part;
    while (std::getline(stream, part, '|')) {
        parts.push_back(part);
    }
    if (parts.size() != 4 || parts[1].empty()) {
        return false;
    }

    GeometryRef parsed;
    if (parts[0] == "BRepFace") {
        parsed.type = EntityType::BRepFace;
    } else if (parts[0] == "BRepEdge") {
        parsed.type = EntityType::BRepEdge;
    } else if (parts[0] == "BRepVertex") {
        parsed.type = EntityType::BRepVertex;
    } else {
        return false;
    }
    parsed.body = parts[1];

    double x = 0.0, y = 0.0, z = 0.0;
    if (std::sscanf(parts[2].c_str(), "%lf,%lf,%lf", &x, &y, &z) != 3) {
        return false;
    }
    parsed.point = gp_Pnt(x, y, z);

    try {
        parsed.measure = std::stod(parts[3]);
    } catch (const std::exception&) {
        return false;
    }

    theResult = parsed;
    return true;
}

std::string EncodeGeometryRefs(const std::vector<GeometryRef>& theRefs)
{
    std::string text;
    for (const GeometryRef& ref : theRefs) {
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

std::vector<GeometryRef> DecodeGeometryRefs(const std::string& theText)
{
    std::vector<GeometryRef> refs;
    std::istringstream stream(theText);
    std::string part;
    while (std::getline(stream, part, ';')) {
        GeometryRef ref;
        if (GeometryRef::Decode(part, ref)) {
            refs.push_back(ref);
        }
    }
    return refs;
}

GeometryRef MakeGeometryRef(const Body& theBody, const TopoDS_Shape& theSubShape)
{
    GeometryRef ref;
    if (theSubShape.IsNull()) {
        return ref;
    }

    switch (theSubShape.ShapeType()) {
        case TopAbs_FACE:   ref.type = EntityType::BRepFace;   break;
        case TopAbs_EDGE:   ref.type = EntityType::BRepEdge;   break;
        case TopAbs_VERTEX: ref.type = EntityType::BRepVertex; break;
        default:            return ref;
    }

    if (!MeasureAndCentre(theSubShape, ref.type, ref.measure, ref.point)) {
        return GeometryRef();
    }
    ref.body = theBody.Name();
    return ref;
}

std::vector<GeometryRef> CollectGeometryRefs(const Body& theBody, EntityType theType)
{
    std::vector<GeometryRef> refs;
    for (const TopoDS_Shape& shape : SubShapesOf(theBody.Shape(), theType)) {
        GeometryRef ref = MakeGeometryRef(theBody, shape);
        if (!ref.IsNull()) {
            refs.push_back(ref);
        }
    }
    return refs;
}

bool ResolveGeometryRef(const Body& theBody, const GeometryRef& theRef, TopoDS_Shape& theResult)
{
    if (theRef.IsNull() || theRef.body != theBody.Name()) {
        return false;
    }
    return ResolveGeometryRefInShape(theBody.Shape(), theRef, theResult);
}

bool ResolveGeometryRefInShape(const TopoDS_Shape& theShape,
                               const GeometryRef&  theRef,
                               TopoDS_Shape&       theResult)
{
    if (theRef.IsNull() || theShape.IsNull()) {
        return false;
    }

    const double diagonal = DiagonalOf(theShape);
    const double maxDistance = diagonal * kPositionTolerance;

    double best = std::numeric_limits<double>::max();
    double runnerUp = std::numeric_limits<double>::max();
    TopoDS_Shape winner;

    for (const TopoDS_Shape& shape : SubShapesOf(theShape, theRef.type)) {
        double measure = 0.0;
        gp_Pnt centre;
        if (!MeasureAndCentre(shape, theRef.type, measure, centre)) {
            continue;
        }

        const double scale = std::max({std::fabs(measure), std::fabs(theRef.measure), 1.0e-9});
        if (std::fabs(measure - theRef.measure) / scale > kMeasureTolerance) {
            continue;
        }

        const double distance = centre.Distance(theRef.point);
        if (distance > maxDistance) {
            continue;
        }

        if (distance < best) {
            runnerUp = best;
            best = distance;
            winner = shape;
        } else if (distance < runnerUp) {
            runnerUp = distance;
        }
    }

    if (winner.IsNull()) {
        return false;
    }

    // Two candidates this close together means the reference does not
    // actually pick one of them out. Refuse: acting on the wrong face of a
    // symmetric part is exactly the failure that is hard to notice.
    if (runnerUp < std::numeric_limits<double>::max()
        && runnerUp < best * kAmbiguityMargin + maxDistance * 1.0e-3) {
        return false;
    }

    theResult = winner;
    return true;
}

} // namespace lcad
