#pragma once

#include "core/Entity.h"
#include "core/GeometryRef.h"

#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS_Shape.hxx>

#include <cstddef>
#include <string>
#include <vector>

namespace lcad {

class Document;

// What the user has picked in the 3D viewport.
//
// Separate from ProfileSelection, which holds regions of a sketch, and
// from SketchSelection, which holds sketch curves. All three exist
// because they answer to different tools: a constraint wants a curve, an
// extrude wants a region, and Press/Pull wants a face. Folding them into
// one list would make every consumer filter, and would make Ctrl+click
// ambiguous about what it was adding to.
//
// Lives in core because the shell fills it in from OCCT's selection and
// the feature subsystem reads it, and neither should have to know about
// the other -- the same reasoning ProfileSelection is here for.
class GeometrySelection
{
public:
    static GeometrySelection& Instance();

    // ---- what is pickable ----
    //
    // Fusion picks a face when the cursor is over one and an edge when it
    // is over an edge, with a filter to narrow that when geometry is
    // crowded. Several types can be on at once; turning them all off
    // would make the viewport inert, so the last one cannot be cleared.
    bool IsFilterOn(EntityType theType) const;
    void SetFilter(EntityType theType, bool theOn);
    const std::vector<EntityType>& Filters() const { return myFilters; }

    // Bumped whenever the filter or the priority changes. The shell watches this to know
    // when to re-activate OCCT's selection modes, rather than every
    // filter command needing a handle on the window and the body list.
    std::size_t FilterGeneration() const { return myFilterGeneration; }

    // Every type a filter can apply to, in the order the UI shows them.
    static const std::vector<EntityType>& FilterableTypes();

    // ---- selection priority ----
    //
    // Fusion's Select Body / Face / Edge Priority: ONE type becomes the
    // only thing a click can pick, whatever the filters say, until it is
    // turned off. The filters are independent checkboxes -- in Fusion too
    // -- so turning Faces off leaves edges pickable, and anyone who wanted
    // "faces only" reads that as a broken filter. Priority is the tool for
    // that. Only one at a time; setting one also ticks its filter, as
    // Fusion's does; changing any filter ends it.
    bool HasPriority() const { return myHasPriority; }
    EntityType Priority() const { return myPriority; }
    void SetPriority(EntityType theType);
    void ClearPriority();

    // What a click can pick right now: the priority alone, or the filters.
    // This, not Filters(), is what the viewport activates.
    std::vector<EntityType> PickableTypes() const;

    // ---- what is picked ----

    void Set(std::vector<GeometryRef> theItems);
    void Clear();

    const std::vector<GeometryRef>& Items() const { return myItems; }
    std::size_t Count() const { return myItems.size(); }
    bool IsEmpty() const { return myItems.empty(); }

    // The single picked item of a given type, or a null ref. Most tools
    // want exactly one face and should refuse two rather than guess.
    GeometryRef SoleItem(EntityType theType) const;

private:
    GeometrySelection();

    std::vector<EntityType>  myFilters;
    std::size_t              myFilterGeneration = 1;
    bool                     myHasPriority = false;
    EntityType               myPriority = EntityType::BRepFace;
    std::vector<GeometryRef> myItems;
};

// One line describing what is picked, for the status bar: the kind of
// thing, and the measurement anyone would want off it -- a face's area, an
// edge's length, a vertex's position. Fusion answers the same question the
// same way, and it is the quickest way to check a model without reaching
// for the measure tool.
//
// Lives here rather than in the shell so the wording can be tested without
// a window.
std::string DescribeSelection(const std::vector<GeometryRef>& theItems);

// The OCCT shape type a filter stands for. TopAbs_SHAPE means the whole
// object, which is how a body is picked.
TopAbs_ShapeEnum TopAbsTypeOf(EntityType theType);

// Describe a sub-shape picked in the viewport, by finding which body owns
// it. Matching is by identity (TopoDS_Shape::IsSame), not by geometry:
// the selected shape IS one of the body's sub-shapes, because that is
// where the viewport got it from.
bool MakeGeometryRefIn(const Document&     theDocument,
                       const TopoDS_Shape& theSubShape,
                       GeometryRef&        theResult);

} // namespace lcad
