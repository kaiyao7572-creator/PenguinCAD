#include "sketch/SketchDisplay.h"

#include "OcctViewport.h"
#include "core/Command.h"
#include "sketch/SketchAnnotations.h"
#include "sketch/SketchFeature.h"
#include "core/ProfileSelection.h"
#include "sketch/SketchProfiles.h"
#include "sketch/SketchSelection.h"
#include "sketch/SketchView.h"

#include <AIS_DisplayMode.hxx>
#include <AIS_TextLabel.hxx>
#include <Aspect_TypeOfLine.hxx>
#include <Aspect_TypeOfMarker.hxx>
#include <BRep_Builder.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_TypeOfShadingModel.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>

namespace lcad {

namespace {

// The palette, written in sRGB -- i.e. in the numbers a colour picker
// would report off a screenshot. Quantity_TOC_RGB means *linear* RGB in
// OCCT 7.9, so specifying colours that way makes every value here a third
// darker than it looks and turns picking a palette into guesswork.
//
// The whole family is one hue on purpose. Fusion draws sketch curves in a
// light blue and says everything else -- committed vs in-progress,
// profile vs construction, selected vs not -- with lightness, weight and
// dashes. A second hue reads as a different KIND of thing, which is why
// the old amber preview looked like debug output next to a blue curve.
const Quantity_Color kActiveSketchColor(0.62, 0.82, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kActiveMarkerColor(0.80, 0.91, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kSketchColor(0.44, 0.55, 0.66, Quantity_TOC_sRGB);
const Quantity_Color kPreviewColor(0.84, 0.93, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kConstructionColor(0.68, 0.56, 0.38, Quantity_TOC_sRGB);
const Quantity_Color kSelectionColor(0.20, 0.72, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kProfileFillColor(0.45, 0.66, 0.86, Quantity_TOC_sRGB);
// Hover and picked are the same hue as the resting fill, brighter and a
// good deal more opaque. Fusion says "you are about to get this one" with
// weight rather than with a new colour, and a second hue here would read
// as a different kind of thing rather than as the same thing, lit.
const Quantity_Color kProfileHoverColor(0.66, 0.85, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kProfileChosenColor(0.24, 0.68, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kConstraintColor(0.60, 0.64, 0.72, Quantity_TOC_sRGB);
const Quantity_Color kDimensionColor(0.95, 0.85, 0.55, Quantity_TOC_sRGB);

// Faint enough that the grid still reads through a filled profile, strong
// enough to be unmistakable at a glance -- which is the whole job of the
// fill, since it is the only thing on screen that says "this region is
// extrudable".
constexpr Standard_Real kProfileFillTransparency = 0.82;

// Opaque enough to be unmistakable, translucent enough that the curves
// bounding the region and the grid under it both still read through.
constexpr Standard_Real kProfileHoverTransparency  = 0.62;
constexpr Standard_Real kProfileChosenTransparency = 0.45;

// Line widths and marker sizes in LOGICAL pixels; DeviceWidth() scales
// them. Fusion's sketch curves sit clearly above the grid, and the
// in-progress curve matches the committed one in weight so that clicking
// changes nothing but the colour.
constexpr double kActiveWidth       = 2.0;
constexpr double kInactiveWidth     = 1.3;
constexpr double kConstructionWidth = 1.3;
constexpr double kPreviewWidth      = 2.0;
constexpr double kSelectionWidth    = 3.0;
constexpr double kAnnotationWidth   = 1.2;
constexpr double kMarkerScale       = 4.5;
constexpr double kPointScale        = 1.6;

// Far finer than OCCT's defaults (0.001 and 20 degrees). A sketch is
// looked at straight on and close up: at the stock coefficient the
// boundary of a filled circular region breaks into chords you can count.
constexpr Standard_Real kDeviation      = 1.0e-4;
constexpr Standard_Real kDeviationAngle = 0.07;  // radians, ~4 degrees

// No selection: sketch lines would otherwise sit between the cursor and
// the faces the user is trying to pick, and picking inside a sketch is
// handled in sketch coordinates by SketchSelection instead.
constexpr Standard_Integer kNoSelectionMode = -1;

// Glyphs and arrowheads are sized in pixels so they stay readable at any
// zoom; this is how many.
constexpr Standard_Integer kAnnotationPixels = 11;

constexpr double kTextHeight = 14.0;

// Curves and markers go one layer above the profile fill rather than
// sharing a layer with it: within a layer OCCT draws translucent surfaces
// last, so a coplanar fill would otherwise wash over the very curves it
// is meant to be filling between.
constexpr Graphic3d_ZLayerId kFillLayer  = Graphic3d_ZLayerId_Top;
constexpr Graphic3d_ZLayerId kCurveLayer = Graphic3d_ZLayerId_Topmost;

void RefineTessellation(const Handle(AIS_Shape)& theObject)
{
    theObject->SetOwnDeviationCoefficient(kDeviation);
    theObject->SetOwnDeviationAngle(kDeviationAngle);
}

} // namespace

SketchDisplay& SketchDisplay::Instance()
{
    // Deliberately leaked. The AIS handles below must not be released
    // during static destruction, which runs after the Qt/OCCT viewer that
    // owns the GL context is already gone.
    static SketchDisplay* theInstance = new SketchDisplay();
    return *theInstance;
}

void SketchDisplay::Attach(const CommandContext& theContext)
{
    const Handle(AIS_InteractiveContext) aisContext = theContext.AisContext();
    const bool rebound = myDocument != theContext.document || myContext != aisContext;

    if (rebound) {
        ClearSketchObjects();
        ClearPreview();
    }

    myDocument = theContext.document;
    myContext = aisContext;
    myView = theContext.View();
    if (theContext.viewport != nullptr) {
        // OCCT's Xw_Window doesn't know about Qt's display scaling, so the
        // ratio has to come from the widget that owns the surface.
        myPixelRatio = theContext.viewport->devicePixelRatioF();
    }

    if (myDocument != nullptr) {
        myDocument->AddObserver(this);  // AddObserver de-duplicates
    }

    if (rebound) {
        // The first Attach usually happens once the viewer is finally up,
        // which may be long after the document was last rebuilt.
        Refresh();
        Redraw();
    }
}

void SketchDisplay::OnDocumentChanged(Document& theDocument)
{
    (void)theDocument;
    Refresh();
    // After Refresh, never before: the fade skips sketch objects, so it
    // has to run once this rebuild's sketch objects actually exist.
    SketchView::Instance().RefreshDimming();
    Redraw();
}

bool SketchDisplay::OwnsObject(const Handle(AIS_InteractiveObject)& theObject) const
{
    if (theObject.IsNull()) {
        return false;
    }
    if (!myPreview.IsNull() && myPreview == theObject) {
        return true;
    }
    for (const Handle(AIS_InteractiveObject)& object : mySketchObjects) {
        if (object == theObject) {
            return true;
        }
    }
    return false;
}

void SketchDisplay::SetActiveSketchName(const std::string& theName)
{
    if (myActiveSketchName == theName) {
        return;
    }
    myActiveSketchName = theName;
    // A reference names entity ids in ONE sketch, so carrying picks from
    // the last sketch into this one would point them at whatever happens
    // to share those ids. ProfileSelection drops them for us.
    ProfileSelection::Instance().SetSketchName(theName);
    Refresh();
    Redraw();
}

void SketchDisplay::SetSketchesVisible(bool theValue)
{
    if (myAreSketchesVisible == theValue) {
        return;
    }
    myAreSketchesVisible = theValue;
    Refresh();
    Redraw();
}

void SketchDisplay::SetConstraintsVisible(bool theValue)
{
    if (myAreConstraintsVisible == theValue) {
        return;
    }
    myAreConstraintsVisible = theValue;
    Refresh();
    Redraw();
}

void SketchDisplay::ClearSketchObjects()
{
    if (!myContext.IsNull()) {
        for (const Handle(AIS_InteractiveObject)& object : mySketchObjects) {
            if (!object.IsNull()) {
                myContext->Remove(object, Standard_False);
            }
        }
    }
    mySketchObjects.clear();

    // Drawn by AddProfileFill and owned through mySketchObjects, so they
    // are already off the screen by here -- these are the handles the
    // hover uses to find them again.
    myProfileObjects.clear();
    myActiveRegions.clear();
    myHoveredProfile = -1;
}

double SketchDisplay::PixelSize() const
{
    if (myView.IsNull()) {
        return 0.0;
    }
    return myView->Convert(1);
}

double SketchDisplay::DeviceWidth(double theLogicalPixels) const
{
    return theLogicalPixels * (myPixelRatio > 0.0 ? myPixelRatio : 1.0);
}

void SketchDisplay::Show(const Handle(AIS_InteractiveObject)& theObject,
                         Standard_Integer                     theDisplayMode,
                         Graphic3d_ZLayerId                   theLayer)
{
    myContext->Display(theObject, theDisplayMode, kNoSelectionMode, Standard_False);
    // Sketches are usually drawn right on a face of the body they belong
    // to; the top layers keep them out of the z-fight.
    myContext->SetZLayer(theObject, theLayer);
    mySketchObjects.push_back(theObject);
}

void SketchDisplay::AddShape(const TopoDS_Shape&   theShape,
                             const Quantity_Color& theColor,
                             double                theWidth,
                             bool                  theIsDashed)
{
    if (theShape.IsNull() || myContext.IsNull()) {
        return;
    }

    const double width = DeviceWidth(theWidth);

    Handle(AIS_Shape) object = new AIS_Shape(theShape);
    object->SetColor(theColor);
    object->SetWidth(width);
    RefineTessellation(object);

    if (theIsDashed) {
        // Construction geometry reads as a guide rather than a boundary,
        // which is exactly what the dashes are for.
        Handle(Prs3d_LineAspect) aspect =
            new Prs3d_LineAspect(theColor, Aspect_TOL_DASH, width);
        const Handle(Prs3d_Drawer)& drawer = object->Attributes();
        drawer->SetWireAspect(aspect);
        drawer->SetLineAspect(aspect);
        drawer->SetSeenLineAspect(aspect);
        drawer->SetFreeBoundaryAspect(aspect);
        drawer->SetUnFreeBoundaryAspect(aspect);
    }

    // Sketch points have no edges, so the marker aspect is the only way
    // they show up at all. A ringed point rather than a plain dot, so a
    // deliberate sketch point is still distinguishable from the endpoint
    // markers AddMarkers puts on every curve.
    object->Attributes()->SetPointAspect(
        new Prs3d_PointAspect(Aspect_TOM_O_PLUS, theColor, DeviceWidth(kPointScale)));

    Show(object, AIS_WireFrame, kCurveLayer);
}

void SketchDisplay::AddMarkers(SketchFeature& theSketch, const Quantity_Color& theColor)
{
    // SnapPoints is deliberately the source: the dots then land exactly
    // where the tools snap, so what the user can see and what they can
    // catch are the same set of points.
    std::vector<SketchEntity> markers;
    for (const gp_Pnt2d& point : theSketch.SnapPoints()) {
        markers.push_back(SketchEntity::MakePoint(point));
    }

    const TopoDS_Shape compound = theSketch.BuildCompound(markers);
    if (compound.IsNull() || myContext.IsNull()) {
        return;
    }

    Handle(AIS_Shape) object = new AIS_Shape(compound);
    object->SetColor(theColor);
    object->Attributes()->SetPointAspect(
        new Prs3d_PointAspect(Aspect_TOM_POINT, theColor, DeviceWidth(kMarkerScale)));
    Show(object, AIS_WireFrame, kCurveLayer);
}

void SketchDisplay::ApplyProfileTint(const Handle(AIS_Shape)& theObject,
                                     std::size_t              theIndex) const
{
    if (theObject.IsNull() || theIndex >= myActiveRegions.size()) {
        return;
    }

    const bool isHovered = myHoveredProfile >= 0
                        && static_cast<std::size_t>(myHoveredProfile) == theIndex;
    const bool isChosen =
        ProfileSelection::Instance().Contains(myActiveRegions[theIndex].ref);

    // Picked beats hovered: once a region is chosen, moving the cursor
    // over it must not make it look less chosen.
    if (isChosen) {
        theObject->SetColor(kProfileChosenColor);
        theObject->SetTransparency(kProfileChosenTransparency);
    } else if (isHovered) {
        theObject->SetColor(kProfileHoverColor);
        theObject->SetTransparency(kProfileHoverTransparency);
    } else {
        theObject->SetColor(kProfileFillColor);
        theObject->SetTransparency(kProfileFillTransparency);
    }
}

void SketchDisplay::AddProfileFill(SketchFeature& theSketch)
{
    if (myContext.IsNull()) {
        return;
    }

    try {
        myActiveRegions = theSketch.ProfileRegions();
    } catch (const Standard_Failure&) {
        myActiveRegions.clear();  // a sketch whose regions won't build is simply not filled
        return;
    }

    // Stale picks go before anything is drawn, so a region the user chose
    // and then trimmed away doesn't leave a highlight with nothing under it.
    ProfileSelection::Instance().Prune(myActiveRegions);

    if (myHoveredProfile >= static_cast<int>(myActiveRegions.size())) {
        myHoveredProfile = -1;
    }

    for (std::size_t i = 0; i < myActiveRegions.size(); ++i) {
        const TopoDS_Face& face = myActiveRegions[i].face;
        if (face.IsNull()) {
            myProfileObjects.push_back(Handle(AIS_Shape)());  // keep the lists parallel
            continue;
        }

        Handle(AIS_Shape) object = new AIS_Shape(face);
        RefineTessellation(object);
        // Unlit and with no face boundary: this is a flat tint, not a
        // surface. Lighting it would shade the fill by the plane's angle to
        // the camera, and the boundary would double every curve underneath.
        object->Attributes()->ShadingAspect()->Aspect()->SetShadingModel(Graphic3d_TOSM_UNLIT);
        object->Attributes()->SetFaceBoundaryDraw(Standard_False);
        ApplyProfileTint(object, i);

        Show(object, AIS_Shaded, kFillLayer);
        myProfileObjects.push_back(object);
    }
}

void SketchDisplay::SetHoveredProfile(int theIndex)
{
    if (theIndex >= static_cast<int>(myActiveRegions.size())) {
        theIndex = -1;
    }
    if (theIndex == myHoveredProfile) {
        return;
    }

    const int previous = myHoveredProfile;
    myHoveredProfile = theIndex;

    if (myContext.IsNull()) {
        return;
    }
    // Only the two regions whose state actually changed are touched.
    for (const int index : {previous, theIndex}) {
        if (index < 0 || static_cast<std::size_t>(index) >= myProfileObjects.size()) {
            continue;
        }
        const Handle(AIS_Shape)& object = myProfileObjects[static_cast<std::size_t>(index)];
        if (object.IsNull()) {
            continue;
        }
        ApplyProfileTint(object, static_cast<std::size_t>(index));
        myContext->Redisplay(object, Standard_False);
    }
}

void SketchDisplay::AddSketch(SketchFeature& theSketch, bool theIsActive)
{
    const Quantity_Color& color = theIsActive ? kActiveSketchColor : kSketchColor;
    // The sketch being edited is the thing on screen that matters, and it
    // is drawn over a model faded to near-transparent -- a hairline would
    // read as an artifact rather than as geometry.
    const double width = theIsActive ? kActiveWidth : kInactiveWidth;

    if (theIsActive) {
        // Under the curves, so the outline of a filled region stays as
        // crisp as an unfilled one.
        AddProfileFill(theSketch);
    }

    AddShape(theSketch.ProfileCompound(), color, width, false);
    AddShape(theSketch.ConstructionCompound(), kConstructionColor, kConstructionWidth, true);
    AddShape(theSketch.PointCompound(), color, width);

    if (theIsActive) {
        // Only while editing: endpoint dots on every finished sketch in
        // the document would bury the model they sit on.
        AddMarkers(theSketch, kActiveMarkerColor);
    }
}

void SketchDisplay::AddSelection(SketchFeature& theSketch)
{
    SketchSelection& selection = SketchSelection::Instance();
    selection.Prune(theSketch);
    if (selection.IsEmpty()) {
        return;
    }

    std::vector<SketchEntity> highlighted;
    for (const SketchPointRef& item : selection.Items()) {
        const SketchEntity* entity = theSketch.FindEntity(item.entity);
        if (entity == nullptr) {
            continue;
        }
        if (item.role == SketchPointRole::Whole && entity->IsCurve()) {
            highlighted.push_back(*entity);
            continue;
        }
        // A picked point is shown as a point, so it's obvious the pick was
        // the corner rather than the whole line through it.
        gp_Pnt2d position;
        if (ResolvePoint(theSketch, item, position)) {
            highlighted.push_back(SketchEntity::MakePoint(position));
        }
    }

    AddShape(theSketch.BuildCompound(highlighted), kSelectionColor, kSelectionWidth);
}

void SketchDisplay::AddAnnotations(SketchFeature& theSketch)
{
    if (!myAreConstraintsVisible) {
        return;
    }

    double scale = PixelSize() * kAnnotationPixels;
    if (scale <= 0.0) {
        scale = 1.0;  // no view yet: any sane size beats drawing nothing
    }

    AddShape(theSketch.BuildCompound(SketchAnnotations::ConstraintGlyphs(theSketch, scale)),
             kConstraintColor, kAnnotationWidth);
    AddShape(theSketch.BuildCompound(SketchAnnotations::DimensionGeometry(theSketch, scale)),
             kDimensionColor, kAnnotationWidth);

    if (myContext.IsNull()) {
        return;
    }
    for (const SketchAnnotations::Label& label : SketchAnnotations::DimensionLabels(theSketch)) {
        Handle(AIS_TextLabel) text = new AIS_TextLabel();
        text->SetText(TCollection_ExtendedString(label.text.c_str()));
        text->SetPosition(theSketch.To3d(label.position));
        text->SetColor(kDimensionColor);
        text->SetHeight(kTextHeight);
        Show(text, 0, kCurveLayer);
    }
}

void SketchDisplay::Refresh()
{
    ClearSketchObjects();

    if (myDocument == nullptr || myContext.IsNull() || !myAreSketchesVisible) {
        return;
    }

    for (const FeaturePtr& feature : myDocument->Features()) {
        SketchFeature* sketch = dynamic_cast<SketchFeature*>(feature.get());
        if (sketch == nullptr || !sketch->IsVisible()) {
            continue;
        }

        const bool isActive = sketch->Name() == myActiveSketchName;
        AddSketch(*sketch, isActive);

        // Constraints, dimensions and the selection belong to the sketch
        // being edited; showing them for every finished sketch would bury
        // the model.
        if (isActive) {
            AddAnnotations(*sketch);
            AddSelection(*sketch);
            // Picks up an Offset typed into the properties panel, so the
            // grid keeps lying on the plane the sketch actually sits on.
            SketchView::Instance().Realign(sketch->Position());
        }
    }
}

void SketchDisplay::ShowPreview(const TopoDS_Shape& theShape)
{
    if (myContext.IsNull() || theShape.IsNull()) {
        ClearPreview();
        return;
    }

    if (myPreview.IsNull()) {
        myPreview = new AIS_Shape(theShape);
        myPreview->SetColor(kPreviewColor);
        myPreview->SetWidth(DeviceWidth(kPreviewWidth));
        RefineTessellation(myPreview);
        myPreview->Attributes()->SetPointAspect(
            new Prs3d_PointAspect(Aspect_TOM_POINT, kPreviewColor, DeviceWidth(kMarkerScale)));
        myContext->Display(myPreview, AIS_WireFrame, kNoSelectionMode, Standard_False);
        myContext->SetZLayer(myPreview, kCurveLayer);
        return;
    }

    myPreview->SetShape(theShape);
    myContext->Redisplay(myPreview, Standard_False);
}

void SketchDisplay::ClearPreview()
{
    if (myPreview.IsNull()) {
        return;
    }
    if (!myContext.IsNull()) {
        myContext->Remove(myPreview, Standard_False);
    }
    myPreview.Nullify();
}

void SketchDisplay::Redraw()
{
    if (!myView.IsNull()) {
        myView->Redraw();
    }
}

} // namespace lcad
