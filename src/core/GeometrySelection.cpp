#include "core/GeometrySelection.h"

#include "core/Body.h"
#include "core/Document.h"
#include "core/Units.h"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <algorithm>
#include <cstdio>
#include <string>

namespace lcad {

namespace {

// Trailing zeros trimmed, like every other number the app shows.
std::string Trimmed(double theValue)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%.2f", theValue);
    std::string out(text);
    if (out.find('.') != std::string::npos) {
        out.erase(out.find_last_not_of('0') + 1);
        if (!out.empty() && out.back() == '.') {
            out.pop_back();
        }
    }
    return out;
}

// Areas are held in square millimetres, so converting them means the
// SQUARE of the length conversion -- an area in inches is not its
// millimetre value divided by 25.4.
std::string FormatArea(double theSquareMillimetres)
{
    const LengthUnit unit = DefaultLengthUnit();
    const double     per  = MillimetersPer(unit);
    const double     value = per > 0.0 ? theSquareMillimetres / (per * per) : theSquareMillimetres;
    return Trimmed(value) + " " + SymbolOf(unit) + "\u00B2";
}

std::string FormatLength(double theMillimetres)
{
    return FormatValue(theMillimetres, UnitKind::Length, DefaultLengthUnit(),
                       AngleUnit::Degree, 2);
}

std::string Counted(std::size_t theCount, const char* theSingular, const char* thePlural)
{
    return std::to_string(theCount) + " " + (theCount == 1 ? theSingular : thePlural);
}

} // namespace

std::string DescribeSelection(const std::vector<GeometryRef>& theItems)
{
    if (theItems.empty()) {
        return std::string();
    }

    std::size_t faces = 0, edges = 0, vertices = 0;
    double      area = 0.0, length = 0.0;
    for (const GeometryRef& item : theItems) {
        switch (item.type) {
            case EntityType::BRepFace:   ++faces;    area   += item.measure; break;
            case EntityType::BRepEdge:   ++edges;    length += item.measure; break;
            case EntityType::BRepVertex: ++vertices;                         break;
            default: break;
        }
    }

    // One thing picked: name it and give its own measurement, which is
    // what the user is asking for by clicking it.
    if (theItems.size() == 1) {
        const GeometryRef& only = theItems.front();
        if (only.type == EntityType::BRepFace) {
            return "Face   Area " + FormatArea(only.measure);
        }
        if (only.type == EntityType::BRepEdge) {
            return "Edge   Length " + FormatLength(only.measure);
        }
        if (only.type == EntityType::BRepVertex) {
            return "Vertex   " + FormatLength(only.point.X()) + ", "
                 + FormatLength(only.point.Y()) + ", " + FormatLength(only.point.Z());
        }
        return "1 selected";
    }

    // Several of one kind total up; a mixed bag only counts, because
    // there is no one number that would mean anything across kinds.
    std::vector<std::string> parts;
    if (faces != 0) {
        parts.push_back(Counted(faces, "face", "faces"));
    }
    if (edges != 0) {
        parts.push_back(Counted(edges, "edge", "edges"));
    }
    if (vertices != 0) {
        parts.push_back(Counted(vertices, "vertex", "vertices"));
    }

    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        text += (i == 0 ? "" : ", ") + parts[i];
    }
    if (faces != 0 && edges == 0 && vertices == 0) {
        text += "   Total area " + FormatArea(area);
    } else if (edges != 0 && faces == 0 && vertices == 0) {
        text += "   Total length " + FormatLength(length);
    }
    return text;
}

TopAbs_ShapeEnum TopAbsTypeOf(EntityType theType)
{
    switch (theType) {
        case EntityType::BRepFace:   return TopAbs_FACE;
        case EntityType::BRepEdge:   return TopAbs_EDGE;
        case EntityType::BRepVertex: return TopAbs_VERTEX;
        default:                     return TopAbs_SHAPE;
    }
}

GeometrySelection::GeometrySelection()
{
    // Faces and edges on by default, vertices off and whole bodies off.
    // That is what Fusion feels like out of the box: you point at a part
    // and get the face, and vertices only get in the way until you
    // actually want one.
    myFilters = {EntityType::BRepFace, EntityType::BRepEdge};
}

GeometrySelection& GeometrySelection::Instance()
{
    static GeometrySelection theInstance;
    return theInstance;
}

const std::vector<EntityType>& GeometrySelection::FilterableTypes()
{
    static const std::vector<EntityType> theTypes = {
        EntityType::BRepBody, EntityType::BRepFace,
        EntityType::BRepEdge, EntityType::BRepVertex};
    return theTypes;
}

bool GeometrySelection::IsFilterOn(EntityType theType) const
{
    return std::find(myFilters.begin(), myFilters.end(), theType) != myFilters.end();
}

void GeometrySelection::SetFilter(EntityType theType, bool theOn)
{
    const auto found = std::find(myFilters.begin(), myFilters.end(), theType);
    if (theOn) {
        if (found == myFilters.end()) {
            myFilters.push_back(theType);
            myHasPriority = false;
            ++myFilterGeneration;
        }
        return;
    }
    // Refuse to turn the last one off: a viewport where nothing at all
    // can be picked looks broken rather than filtered.
    if (found != myFilters.end() && myFilters.size() > 1) {
        myFilters.erase(found);
        myHasPriority = false;
        ++myFilterGeneration;
    }
}

void GeometrySelection::SetPriority(EntityType theType)
{
    if (myHasPriority && myPriority == theType) {
        return;
    }
    if (!IsFilterOn(theType)) {
        myFilters.push_back(theType);
    }
    myHasPriority = true;
    myPriority = theType;
    ++myFilterGeneration;
}

void GeometrySelection::ClearPriority()
{
    if (!myHasPriority) {
        return;
    }
    myHasPriority = false;
    ++myFilterGeneration;
}

std::vector<EntityType> GeometrySelection::PickableTypes() const
{
    if (myHasPriority) {
        return {myPriority};
    }
    return myFilters;
}

void GeometrySelection::Set(std::vector<GeometryRef> theItems)
{
    myItems = std::move(theItems);
}

void GeometrySelection::Clear()
{
    myItems.clear();
}

GeometryRef GeometrySelection::SoleItem(EntityType theType) const
{
    GeometryRef found;
    for (const GeometryRef& item : myItems) {
        if (item.type != theType) {
            continue;
        }
        if (!found.IsNull()) {
            return GeometryRef();  // more than one: let the caller refuse
        }
        found = item;
    }
    return found;
}

bool MakeGeometryRefIn(const Document&     theDocument,
                       const TopoDS_Shape& theSubShape,
                       GeometryRef&        theResult)
{
    if (theSubShape.IsNull()) {
        return false;
    }
    const TopAbs_ShapeEnum type = theSubShape.ShapeType();
    if (type != TopAbs_FACE && type != TopAbs_EDGE && type != TopAbs_VERTEX) {
        return false;
    }

    for (const BodyPtr& body : theDocument.Bodies()) {
        if (!body || body->Shape().IsNull()) {
            continue;
        }
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(body->Shape(), type, map);
        if (!map.Contains(theSubShape)) {
            continue;
        }
        theResult = MakeGeometryRef(*body, theSubShape);
        return !theResult.IsNull();
    }
    return false;
}

} // namespace lcad
