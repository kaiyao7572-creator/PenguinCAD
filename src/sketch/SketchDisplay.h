#pragma once

#include "core/Document.h"
#include "core/ProfileProvider.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <AIS_TextLabel.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Quantity_Color.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_View.hxx>
#include <gp_Pnt.hxx>

#include <map>
#include <string>
#include <vector>

namespace lcad {

struct CommandContext;
class SketchFeature;

// A line of text floating in the viewport, in model coordinates.
// One of Fusion's on-canvas value boxes: a number floating beside the
// cursor while a tool collects, drawn on a filled plate so it reads
// against whatever is behind it.
struct SketchLabel
{
    gp_Pnt      position;
    std::string text;

    // The box the next keystroke lands in. Fusion highlights it rather
    // than marking it with a character, so the eye finds it without
    // reading anything.
    bool isActive = false;
};

// Owns every AIS object the sketch subsystem puts in the viewport.
//
// Sketch geometry isn't part of Document::Shape(), so MainWindow's
// redisplay never sees it -- this observer keeps it in sync instead. It
// wipes and rebuilds its objects on every document change, which is what
// keeps deleted or undone sketches from leaking into the viewport.
class SketchDisplay : public DocumentObserver
{
public:
    static SketchDisplay& Instance();

    // Bind to the document/viewer a command is running against. Cheap and
    // idempotent; every sketch command calls it before doing anything.
    void Attach(const CommandContext& theContext);

    void OnDocumentChanged(Document& theDocument) override;

    // The sketch being edited is drawn brighter than the finished ones,
    // and is the only one whose constraints and dimensions are shown.
    void SetActiveSketchName(const std::string& theName);
    const std::string& ActiveSketchName() const { return myActiveSketchName; }

    // Rubber-band geometry for the tool in progress. A null shape clears
    // it, as does ClearPreview().
    void ShowPreview(const TopoDS_Shape& theShape);
    void ClearPreview();

    // The live length/angle readout a create tool floats beside the
    // cursor. It belongs to the rubber band, so ClearPreview takes it
    // down too -- one call still ends an in-progress curve completely.
    // The objects are reused between calls: a text label rebuilt on every
    // mouse move flickers.
    void ShowPreviewLabels(const std::vector<SketchLabel>& theLabels);

    // Paint one value box: plate, text colour and size, highlighted when
    // it is the one taking keystrokes.
    void StyleValueBox(const Handle(AIS_TextLabel)& theLabel, bool theIsActive) const;
    void ClearPreviewLabels();

    bool AreSketchesVisible() const { return myAreSketchesVisible; }
    void SetSketchesVisible(bool theValue);

    bool AreConstraintsVisible() const { return myAreConstraintsVisible; }
    void SetConstraintsVisible(bool theValue);

    // The active sketch's regions as they were last drawn. Hit-testing a
    // hover against these costs nothing, where recomputing the planar
    // arrangement on every mouse move would not keep up with the cursor.
    const std::vector<ProfileRegion>& ActiveProfileRegions() const { return myActiveRegions; }

    // Light up one region under the cursor, or -1 for none. Recolours the
    // objects already on screen rather than rebuilding them: a Refresh per
    // mouse move would tear down and re-display every sketch object in the
    // document, which reads as a stutter rather than as a highlight.
    void SetHoveredProfile(int theIndex);
    int HoveredProfile() const { return myHoveredProfile; }

    // ---- the model view: no sketch open ----
    //
    // A finished sketch's regions stay pickable, as in Fusion, but only the
    // region under the cursor and the ones already picked are tinted.
    // Filling every region of every sketch would bury the model in blue,
    // which is why Fusion does not do it either.

    // A finished sketch's regions, built the first time they are asked for
    // after a rebuild and kept until the next one. The hover asks on every
    // mouse move, which is far too often to rebuild the planar arrangement.
    const std::vector<ProfileRegion>& RegionsOf(const SketchFeature& theSketch);

    // Light one region of a finished sketch; an empty name or -1 lights
    // none. Moves one object rather than refreshing, for the same reason
    // SetHoveredProfile does. True when what is lit changed, so the caller
    // knows a redraw is due.
    bool SetModelHover(const std::string& theSketchName, int theIndex);

    // The picks changed -- a click, or Extrude spending the ones it built
    // on after the rebuild had already drawn them. Redraws only what shows
    // them.
    void OnProfileSelectionChanged();

    // True for the AIS objects this display put in the viewport. Code that
    // walks everything on screen -- the plane picker looking for pickable
    // faces, the sketch-mode fade looking for the model -- has to be able
    // to tell sketch curves apart from real geometry, and their AIS type
    // alone doesn't say.
    bool OwnsObject(const Handle(AIS_InteractiveObject)& theObject) const;

    // Rebuild every sketch object from the document.
    void Refresh();
    void Redraw();

private:
    SketchDisplay() = default;

    void ClearSketchObjects();

    // Refresh's body; Refresh itself only marks that one is under way.
    void RefreshObjects();

    // Hand an object to the viewer and remember it so the next Refresh can
    // take it back out again.
    void Show(const Handle(AIS_InteractiveObject)& theObject,
              Standard_Integer                     theDisplayMode,
              Graphic3d_ZLayerId                   theLayer);

    // Display one shape with a colour and line width, remembering it so
    // the next Refresh can take it back out again. Widths are in logical
    // pixels; DeviceWidth scales them to the framebuffer.
    void AddShape(const TopoDS_Shape& theShape,
                  const Quantity_Color& theColor,
                  double                theWidth,
                  bool                  theIsDashed = false);

    // The dots Fusion puts on every endpoint and curve centre. They are a
    // separate object from the curves because AIS only draws the vertices
    // of a shape that has no edges at all.
    void AddMarkers(SketchFeature& theSketch, const Quantity_Color& theColor);

    // One translucent tint per closed region -- the signal that tells the
    // user an area is extrudable before they reach for Extrude, and the
    // thing they click to say which area they meant. Drawn from the same
    // regions Extrude will consume, so what is lit is what is built.
    void AddProfileFill(SketchFeature& theSketch);

    // Colour and opacity for one region, given whether it is hovered or
    // picked. Kept in one place so the resting, hovered and selected
    // states can't drift apart between the initial draw and a recolour.
    void ApplyProfileTint(const Handle(AIS_Shape)& theObject, std::size_t theIndex) const;

    // A flat, unlit tint over one region, on the fill layer, not
    // pickable. Displayed but not remembered -- the caller keeps it.
    Handle(AIS_Shape) ShowRegionFill(const TopoDS_Face&    theFace,
                                     const Quantity_Color& theColor,
                                     Standard_Real         theTransparency);

    // The finished sketch the picks belong to, if it is still in the
    // document and on screen.
    const SketchFeature* PickedSketch() const;

    // Model view only: one tint per picked region, and the hover tint.
    void AddModelPicks();
    void RemoveModelPicks();
    void UpdateModelHover();
    void RemoveModelHover();

    void AddSketch(SketchFeature& theSketch, bool theIsActive);
    void AddAnnotations(SketchFeature& theSketch);

    // Every curve's own length/radius/diameter, drawn beside it.
    void AddMeasures(SketchFeature& theSketch);
    void AddSelection(SketchFeature& theSketch);

    // Model size of one screen pixel, used to keep glyphs and arrowheads
    // the same size however far the view is zoomed.
    double PixelSize() const;

    // Logical pixels -> framebuffer pixels. OCCT line widths and marker
    // scales are in framebuffer pixels, so on a scaled display a width of
    // 2 draws the one-pixel hairline the user complained about.
    double DeviceWidth(double theLogicalPixels) const;

    Document*                      myDocument = nullptr;
    Handle(AIS_InteractiveContext) myContext;
    Handle(V3d_View)               myView;

    std::vector<Handle(AIS_InteractiveObject)> mySketchObjects;
    Handle(AIS_Shape)                          myPreview;
    std::vector<Handle(AIS_TextLabel)>         myPreviewLabels;

    // Parallel to each other: the regions of the active sketch and the
    // object drawn for each, so a hover can recolour exactly one.
    std::vector<ProfileRegion>     myActiveRegions;
    std::vector<Handle(AIS_Shape)> myProfileObjects;
    int                            myHoveredProfile = -1;

    // The model view's regions by sketch name, and the few tints it draws.
    std::map<std::string, std::vector<ProfileRegion>> myModelRegions;
    std::vector<Handle(AIS_Shape)>                    myModelPickObjects;
    Handle(AIS_Shape)                                 myModelHoverObject;
    std::string                                       myModelHoverSketch;
    int                                               myModelHoverIndex = -1;

    // Refresh prunes the picks, which notifies; the refresh is already
    // drawing them, so the notification must not start a second draw.
    bool myIsRefreshing = false;

    std::string myActiveSketchName;
    bool        myAreSketchesVisible = true;
    bool        myAreConstraintsVisible = true;
    double      myPixelRatio = 1.0;
};

} // namespace lcad
