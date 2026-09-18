#include "core/GeometrySelection.h"

#include "core/Body.h"
#include "core/Document.h"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <algorithm>

namespace lcad {

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
            ++myFilterGeneration;
        }
        return;
    }
    // Refuse to turn the last one off: a viewport where nothing at all
    // can be picked looks broken rather than filtered.
    if (found != myFilters.end() && myFilters.size() > 1) {
        myFilters.erase(found);
        ++myFilterGeneration;
    }
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
