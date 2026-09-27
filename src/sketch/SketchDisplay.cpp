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
#include <Aspect_TypeOfDisplayText.hxx>
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
// Bright enough to read at a glance without competing with the curves.
// Fusion's constraint symbols are near-white on the dark canvas; at the
// old grey-blue they were the faintest thing on screen and the one part
// of a sketch that tells you why it behaves as it does.
const Quantity_Color kConstraintColor(0.80, 0.84, 0.90, Quantity_TOC_sRGB);
const Quantity_Color kDimensionColor(0.95, 0.85, 0.55, Quantity_TOC_sRGB);

// Fusion's on-canvas value boxes: a pale plate with near-black text, and
// the box awaiting the next keystroke filled in the same blue everything
// grabbable in this app uses. Dark text on a light plate rather than the
// other way round, because it has to stay readable over a shaded body as
// well as over the empty grid.
// Every curve's own size, shown while a sketch is open. Deliberately
// quieter than a placed dimension: these are there to be read, not to
// drive the geometry, and they must not be mistaken for the dimensions
// that do.
const Quantity_Color kMeasureColor(0.72, 0.76, 0.82, Quantity_TOC_sRGB);

const Quantity_Color kValueBoxColor(0.91, 0.93, 0.95, Quantity_TOC_sRGB);
const Quantity_Color kValueTextColor(0.08, 0.10, 0.13, Quantity_TOC_sRGB);
const Quantity_Color kValueBoxActiveColor(0.20, 0.72, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kValueTextActiveColor(1.00, 1.00, 1.00, Quantity_TOC_sRGB);

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
constexpr double kAnnotationWidth   = 1.4;
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
constexpr Standard_Integer kAnnotationPixels = 13;

// LOGICAL pixels, scaled to the framebuffer by DeviceWidth() like every
// other size here. It used to be passed to AIS raw, which on a 2x display
// drew every number at half the size it was meant to be -- the numbers on
// a sketch are the thing the user is reading, and they were the smallest
// text on screen.
constexpr double kTextHeight    = 13.0;
constexpr double kMeasureHeight = 11.0;
constexpr double kValueBoxHeight = 14.0;

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

// Unlit and with no face boundary: a region fill is a flat tint, not a
// surface. Lighting it would shade the fill by the plane's angle to the
// camera, and the boundary would double every curve underneath.
//
// Only once a colour is set: AIS_Shape materialises its shading aspect on
// SetColor, and reaching into ShadingAspect()->Aspect() before that
// dereferences a null handle -- a segfault no headless test can catch.
void MakeFlatTint(const Handle(AIS_Shape)& theObject)
{
    theObject->Attributes()->ShadingAspect()->Aspect()->SetShadingModel(Graphic3d_TOSM_UNLIT);
    theObject->Attributes()->SetFaceBoundaryDraw(Standard_False);
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

    // This display is the one thing that draws the picks, so it is the one
    // handler. Leaked with the display, so the captured pointer never dies.
    ProfileSelection::Instance().SetChangeHandler([this]() { OnProfileSelectionChanged(); });

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
    for (const Handle(AIS_TextLabel)& label : myPreviewLabels) {
        if (label == theObject) {
            return true;
        }
    }
    for (const Handle(AIS_InteractiveObject)& object : mySketchObjects) {
        if (object == theObject) {
            return true;
        }
    }
    if (!myModelHoverObject.IsNull() && myModelHoverObject == theObject) {
        return true;
    }
    for (const Handle(AIS_Shape)& object : myModelPickObjects) {
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

    // The model view's regions were built from geometry this rebuild may
    // have changed, and its tints name regions by index into them.
    RemoveModelPicks();
    RemoveModelHover();
    myModelRegions.clear();
    myModelHoverSketch.clear();
    myModelHoverIndex = -1;
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

        // The tint goes on FIRST because AIS_Shape only materialises its
        // shading aspect when a colour is set. Reaching into
        // ShadingAspect()->Aspect() before that dereferences a null
        // handle, which segfaults the moment a sketch draws its first
        // closed region -- and no headless test can catch it, because none
        // of this exists without a viewer.
        ApplyProfileTint(object, i);
        MakeFlatTint(object);

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

const std::vector<ProfileRegion>& SketchDisplay::RegionsOf(const SketchFeature& theSketch)
{
    const auto found = myModelRegions.find(theSketch.Name());
    if (found != myModelRegions.end()) {
        return found->second;
    }
    std::vector<ProfileRegion> regions;
    try {
        regions = theSketch.ProfileRegions();
    } catch (const Standard_Failure&) {
        regions.clear();  // a sketch whose regions won't build simply has none to pick
    }
    return myModelRegions.emplace(theSketch.Name(), std::move(regions)).first->second;
}

const SketchFeature* SketchDisplay::PickedSketch() const
{
    const std::string& name = ProfileSelection::Instance().SketchName();
    if (myDocument == nullptr || name.empty()) {
        return nullptr;
    }
    for (const FeaturePtr& feature : myDocument->Features()) {
        const SketchFeature* sketch = dynamic_cast<const SketchFeature*>(feature.get());
        if (sketch != nullptr && sketch->Name() == name) {
            return sketch->IsVisible() ? sketch : nullptr;
        }
    }
    return nullptr;
}

Handle(AIS_Shape) SketchDisplay::ShowRegionFill(const TopoDS_Face&    theFace,
                                                const Quantity_Color& theColor,
                                                Standard_Real         theTransparency)
{
    Handle(AIS_Shape) object = new AIS_Shape(theFace);
    RefineTessellation(object);
    object->SetColor(theColor);  // before MakeFlatTint, which needs the aspect it creates
    object->SetTransparency(theTransparency);
    MakeFlatTint(object);

    myContext->Display(object, AIS_Shaded, kNoSelectionMode, Standard_False);
    myContext->SetZLayer(object, kFillLayer);
    return object;
}

void SketchDisplay::AddModelPicks()
{
    const ProfileSelection& selection = ProfileSelection::Instance();
    if (selection.IsEmpty() || myContext.IsNull() || !myActiveSketchName.empty()) {
        return;
    }
    const SketchFeature* sketch = PickedSketch();
    if (sketch == nullptr) {
        return;
    }
    for (const ProfileRegion& region : RegionsOf(*sketch)) {
        if (region.face.IsNull() || !selection.Contains(region.ref)) {
            continue;
        }
        myModelPickObjects.push_back(
            ShowRegionFill(region.face, kProfileChosenColor, kProfileChosenTransparency));
    }
}

void SketchDisplay::RemoveModelPicks()
{
    if (!myContext.IsNull()) {
        for (const Handle(AIS_Shape)& object : myModelPickObjects) {
            if (!object.IsNull()) {
                myContext->Remove(object, Standard_False);
            }
        }
    }
    myModelPickObjects.clear();
}

void SketchDisplay::UpdateModelHover()
{
    TopoDS_Face face;
    if (myActiveSketchName.empty() && myModelHoverIndex >= 0) {
        const auto found = myModelRegions.find(myModelHoverSketch);
        if (found != myModelRegions.end()
            && static_cast<std::size_t>(myModelHoverIndex) < found->second.size()) {
            const ProfileRegion& region = found->second[static_cast<std::size_t>(myModelHoverIndex)];
            // Picked beats hovered, exactly as in sketch mode: the chosen
            // tint is already on it, and a lighter one laid over it would
            // make it look less chosen.
            const ProfileSelection& selection = ProfileSelection::Instance();
            const bool isPicked =
                selection.SketchName() == myModelHoverSketch && selection.Contains(region.ref);
            if (!isPicked) {
                face = region.face;
            }
        }
    }

    if (face.IsNull() || myContext.IsNull()) {
        RemoveModelHover();
        return;
    }
    if (myModelHoverObject.IsNull()) {
        myModelHoverObject = ShowRegionFill(face, kProfileHoverColor, kProfileHoverTransparency);
        return;
    }
    // One object moved from region to region, not one per region: the
    // hover changes on every few pixels of travel.
    myModelHoverObject->SetShape(face);
    myContext->Redisplay(myModelHoverObject, Standard_False);
}

void SketchDisplay::RemoveModelHover()
{
    if (!myModelHoverObject.IsNull() && !myContext.IsNull()) {
        myContext->Remove(myModelHoverObject, Standard_False);
    }
    myModelHoverObject.Nullify();
}

bool SketchDisplay::SetModelHover(const std::string& theSketchName, int theIndex)
{
    const bool isNone = theSketchName.empty() || theIndex < 0;
    const std::string name = isNone ? std::string() : theSketchName;
    const int index = isNone ? -1 : theIndex;
    if (name == myModelHoverSketch && index == myModelHoverIndex) {
        return false;
    }
    myModelHoverSketch = name;
    myModelHoverIndex = index;
    UpdateModelHover();
    return true;
}

void SketchDisplay::OnProfileSelectionChanged()
{
    if (myIsRefreshing || myContext.IsNull()) {
        return;
    }

    if (!myActiveSketchName.empty()) {
        // Sketch mode draws every region already; only the tints move.
        for (std::size_t i = 0; i < myProfileObjects.size(); ++i) {
            if (myProfileObjects[i].IsNull()) {
                continue;
            }
            ApplyProfileTint(myProfileObjects[i], i);
            myContext->Redisplay(myProfileObjects[i], Standard_False);
        }
    } else {
        RemoveModelPicks();
        AddModelPicks();
        // A region just picked stops wearing the hover tint over its own.
        UpdateModelHover();
    }
    Redraw();
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

void SketchDisplay::AddMeasures(SketchFeature& theSketch)
{
    if (myContext.IsNull()) {
        return;
    }

    double scale = PixelSize() * kAnnotationPixels;
    if (scale <= 0.0) {
        scale = 1.0;  // no view yet: any sane size beats drawing nothing
    }

    for (const SketchAnnotations::Label& label :
         SketchAnnotations::CurveMeasureLabels(theSketch, scale)) {
        Handle(AIS_TextLabel) text = new AIS_TextLabel();
        text->SetText(TCollection_ExtendedString(label.text.c_str()));
        text->SetPosition(theSketch.To3d(label.position));
        text->SetColor(kMeasureColor);
        text->SetHeight(DeviceWidth(kMeasureHeight));
        text->SetHJustification(Graphic3d_HTA_CENTER);
        text->SetVJustification(Graphic3d_VTA_CENTER);
        Show(text, 0, kCurveLayer);
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
    for (const SketchAnnotations::Label& label :
         SketchAnnotations::DimensionLabels(theSketch, scale)) {
        Handle(AIS_TextLabel) text = new AIS_TextLabel();
        text->SetText(TCollection_ExtendedString(label.text.c_str()));
        text->SetPosition(theSketch.To3d(label.position));
        text->SetColor(kDimensionColor);
        text->SetHeight(DeviceWidth(kTextHeight));
        // Centred on the lifted anchor and sitting above it, so the number
        // straddles its dimension line the way a drawing has it rather
        // than trailing off to one side.
        text->SetHJustification(Graphic3d_HTA_CENTER);
        text->SetVJustification(Graphic3d_VTA_BOTTOM);
        Show(text, 0, kCurveLayer);
    }
}

void SketchDisplay::Refresh()
{
    // Pruning the picks below notifies, and this is already drawing them.
    const bool wasRefreshing = myIsRefreshing;
    myIsRefreshing = true;
    RefreshObjects();
    myIsRefreshing = wasRefreshing;
}

void SketchDisplay::RefreshObjects()
{
    ClearSketchObjects();

    if (myDocument == nullptr || myContext.IsNull()) {
        return;
    }
    if (!myAreSketchesVisible) {
        // With no sketch on screen there is nothing to see a pick on, so
        // nothing may stay picked: E would build on regions nobody can see.
        if (myActiveSketchName.empty()) {
            ProfileSelection::Instance().Clear();
        }
        return;
    }

    for (const FeaturePtr& feature : myDocument->Features()) {
        SketchFeature* sketch = dynamic_cast<SketchFeature*>(feature.get());
        if (sketch == nullptr || !sketch->IsVisible()) {
            continue;
        }

        const bool isActive = sketch->Name() == myActiveSketchName;
        AddSketch(*sketch, isActive);

        // Every curve's size, for EVERY sketch, whenever one is open --
        // what is already drawn elsewhere is exactly what a new sketch
        // has to line up with, so its numbers are worth as much as the
        // ones being drawn now. Outside sketch mode they would be clutter
        // over the model, so they go with it.
        if (!myActiveSketchName.empty()) {
            AddMeasures(*sketch);
        }

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

    if (!myActiveSketchName.empty()) {
        return;
    }

    // No sketch open: the picks were made in the model view, against a
    // finished sketch. A rebuild (an undo, an edit to that sketch) may have
    // reshaped or removed what they named, so they are checked against the
    // regions as they are NOW before anything is drawn for them -- and a
    // sketch that is gone or hidden takes its picks with it.
    ProfileSelection& selection = ProfileSelection::Instance();
    if (!selection.IsEmpty()) {
        if (const SketchFeature* picked = PickedSketch()) {
            selection.Prune(RegionsOf(*picked));
        } else {
            selection.Clear();
        }
    }
    AddModelPicks();
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

void SketchDisplay::ShowPreviewLabels(const std::vector<SketchLabel>& theLabels)
{
    if (myContext.IsNull()) {
        return;
    }

    while (myPreviewLabels.size() > theLabels.size()) {
        if (!myPreviewLabels.back().IsNull()) {
            myContext->Remove(myPreviewLabels.back(), Standard_False);
        }
        myPreviewLabels.pop_back();
    }

    for (std::size_t i = 0; i < theLabels.size(); ++i) {
        const TCollection_ExtendedString text(theLabels[i].text.c_str());
        if (i < myPreviewLabels.size()) {
            myPreviewLabels[i]->SetText(text);
            myPreviewLabels[i]->SetPosition(theLabels[i].position);
            // Restyled on every update, not just on creation: Tab moves
            // the highlight between boxes that already exist.
            StyleValueBox(myPreviewLabels[i], theLabels[i].isActive);
            myContext->Redisplay(myPreviewLabels[i], Standard_False);
            continue;
        }

        Handle(AIS_TextLabel) label = new AIS_TextLabel();
        label->SetText(text);
        label->SetPosition(theLabels[i].position);
        StyleValueBox(label, theLabels[i].isActive);
        myContext->Display(label, 0, kNoSelectionMode, Standard_False);
        myContext->SetZLayer(label, kCurveLayer);
        myPreviewLabels.push_back(label);
    }
}

void SketchDisplay::StyleValueBox(const Handle(AIS_TextLabel)& theLabel, bool theIsActive) const
{
    if (theLabel.IsNull()) {
        return;
    }
    // Aspect_TODT_SUBTITLE fills a plate behind the glyphs, which is what
    // makes these read as input boxes rather than as text lying loose on
    // the model.
    theLabel->SetDisplayType(Aspect_TODT_SUBTITLE);
    theLabel->SetColorSubTitle(theIsActive ? kValueBoxActiveColor : kValueBoxColor);
    theLabel->SetColor(theIsActive ? kValueTextActiveColor : kValueTextColor);
    theLabel->SetHeight(DeviceWidth(kValueBoxHeight));
    theLabel->SetHJustification(Graphic3d_HTA_LEFT);
    theLabel->SetVJustification(Graphic3d_VTA_CENTER);
}

void SketchDisplay::ClearPreviewLabels()
{
    if (!myContext.IsNull()) {
        for (const Handle(AIS_TextLabel)& label : myPreviewLabels) {
            if (!label.IsNull()) {
                myContext->Remove(label, Standard_False);
            }
        }
    }
    myPreviewLabels.clear();
}

void SketchDisplay::ClearPreview()
{
    // The readout belongs to the rubber band: every existing call site
    // that ends a curve takes the numbers down with it.
    ClearPreviewLabels();

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
