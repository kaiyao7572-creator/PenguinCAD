#pragma once

#include "core/Document.h"
#include "core/ProfileProvider.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_InteractiveObject.hxx>
#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Quantity_Color.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_View.hxx>
#include <gp_Pnt.hxx>

#include <string>
#include <vector>

namespace lcad {

struct CommandContext;
class SketchFeature;

// A line of text floating in the viewport, in model coordinates.
struct SketchLabel
{
    gp_Pnt      position;
    std::string text;
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

    void AddSketch(SketchFeature& theSketch, bool theIsActive);
    void AddAnnotations(SketchFeature& theSketch);
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

    std::string myActiveSketchName;
    bool        myAreSketchesVisible = true;
    bool        myAreConstraintsVisible = true;
    double      myPixelRatio = 1.0;
};

} // namespace lcad
