#pragma once

#include "sketch/SketchConstraints.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchTools.h"

#include <gp_Pnt2d.hxx>

#include <vector>

namespace lcad {

// What the user has picked inside the active sketch.
//
// Sketch geometry is deliberately kept out of OCCT's own selection: it is
// displayed with selection switched off so sketch lines never sit between
// the cursor and the solid faces behind them. Picking therefore happens
// here, in sketch coordinates, which also gives the constraint commands
// the thing they actually need -- an entity plus WHICH of its points was
// clicked.
class SketchSelection
{
public:
    static SketchSelection& Instance();

    // theAdditive keeps what was already picked (Ctrl+click).
    void Add(const SketchPointRef& theRef, bool theAdditive);
    void Remove(const SketchPointRef& theRef);
    void Clear();

    bool Contains(const SketchPointRef& theRef) const;

    const std::vector<SketchPointRef>& Items() const { return myItems; }
    std::size_t Count() const { return myItems.size(); }
    bool IsEmpty() const { return myItems.empty(); }

    // Distinct entity ids, in the order they were picked. Several picked
    // points on one entity collapse to a single id.
    std::vector<int> EntityIds() const;

    // Drop picks whose entity no longer exists -- an undo or a trim can
    // take the geometry out from under the selection.
    void Prune(const SketchFeature& theSketch);

private:
    SketchSelection() = default;

    std::vector<SketchPointRef> myItems;
};

// What lies under a point in sketch coordinates. Characteristic points
// win over the curve itself, so clicking a corner picks the corner rather
// than one of the two lines meeting there.
bool PickSketchEntity(const SketchFeature& theSketch,
                      const gp_Pnt2d&      thePoint,
                      double               theTolerance,
                      SketchPointRef&      theResult);

// Position of a picked point, for glyphs and dimension anchors.
bool ResolvePoint(const SketchFeature&  theSketch,
                  const SketchPointRef& theRef,
                  gp_Pnt2d&             theResult);

// The tool that owns the viewport whenever no drawing tool does: click to
// pick, Ctrl+click to add, Delete to erase, Escape to clear.
SketchTool& SketchSelectTool();

} // namespace lcad
