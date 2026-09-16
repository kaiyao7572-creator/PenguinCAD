#include "sketch/SketchDisplay.h"

#include "core/Command.h"
#include "sketch/SketchAnnotations.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchSelection.h"
#include "sketch/SketchView.h"

#include <AIS_DisplayMode.hxx>
#include <AIS_TextLabel.hxx>
#include <Aspect_TypeOfLine.hxx>
#include <Aspect_TypeOfMarker.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <TCollection_ExtendedString.hxx>

namespace lcad {

namespace {

// Sketch geometry has to read as sketch geometry at a glance, so it gets
// its own palette rather than the shape's default yellow-grey.
const Quantity_Color kActiveSketchColor(0.36, 0.86, 1.00, Quantity_TOC_RGB);
const Quantity_Color kSketchColor(0.58, 0.70, 0.88, Quantity_TOC_RGB);
const Quantity_Color kPreviewColor(1.00, 0.78, 0.25, Quantity_TOC_RGB);
const Quantity_Color kConstructionColor(0.95, 0.60, 0.20, Quantity_TOC_RGB);
const Quantity_Color kSelectionColor(0.25, 1.00, 0.45, Quantity_TOC_RGB);
const Quantity_Color kConstraintColor(0.75, 0.75, 0.85, Quantity_TOC_RGB);
const Quantity_Color kDimensionColor(1.00, 0.95, 0.55, Quantity_TOC_RGB);

// No selection: sketch lines would otherwise sit between the cursor and
// the faces the user is trying to pick, and picking inside a sketch is
// handled in sketch coordinates by SketchSelection instead.
constexpr Standard_Integer kNoSelectionMode = -1;

// Glyphs and arrowheads are sized in pixels so they stay readable at any
// zoom; this is how many.
constexpr Standard_Integer kAnnotationPixels = 11;

constexpr double kTextHeight = 14.0;

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
}

double SketchDisplay::PixelSize() const
{
    if (myView.IsNull()) {
        return 0.0;
    }
    return myView->Convert(1);
}

void SketchDisplay::AddShape(const TopoDS_Shape&   theShape,
                             const Quantity_Color& theColor,
                             double                theWidth,
                             bool                  theIsDashed)
{
    if (theShape.IsNull() || myContext.IsNull()) {
        return;
    }

    Handle(AIS_Shape) object = new AIS_Shape(theShape);
    object->SetColor(theColor);
    object->SetWidth(theWidth);

    if (theIsDashed) {
        // Construction geometry reads as a guide rather than a boundary,
        // which is exactly what the dashes are for.
        Handle(Prs3d_LineAspect) aspect =
            new Prs3d_LineAspect(theColor, Aspect_TOL_DASH, theWidth);
        const Handle(Prs3d_Drawer)& drawer = object->Attributes();
        drawer->SetWireAspect(aspect);
        drawer->SetLineAspect(aspect);
        drawer->SetSeenLineAspect(aspect);
        drawer->SetFreeBoundaryAspect(aspect);
        drawer->SetUnFreeBoundaryAspect(aspect);
    }

    // Sketch points have no edges, so the marker aspect is the only way
    // they show up at all.
    object->Attributes()->SetPointAspect(
        new Prs3d_PointAspect(Aspect_TOM_O_PLUS, theColor, 1.6));

    myContext->Display(object, AIS_WireFrame, kNoSelectionMode, Standard_False);
    // Sketches are usually drawn right on a face of the body they belong
    // to; the top layer keeps them out of the z-fight.
    myContext->SetZLayer(object, Graphic3d_ZLayerId_Top);
    mySketchObjects.push_back(object);
}

void SketchDisplay::AddSketch(SketchFeature& theSketch, bool theIsActive)
{
    const Quantity_Color& color = theIsActive ? kActiveSketchColor : kSketchColor;
    // The sketch being edited is the thing on screen that matters, and it
    // is drawn over a model faded to near-transparent -- a hairline would
    // read as an artifact rather than as geometry.
    const double width = theIsActive ? 2.6 : 1.8;

    AddShape(theSketch.ProfileCompound(), color, width, false);
    AddShape(theSketch.ConstructionCompound(), kConstructionColor, 1.5, true);
    AddShape(theSketch.PointCompound(), color, 1.0);
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

    AddShape(theSketch.BuildCompound(highlighted), kSelectionColor, 3.5);
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
             kConstraintColor, 1.5);
    AddShape(theSketch.BuildCompound(SketchAnnotations::DimensionGeometry(theSketch, scale)),
             kDimensionColor, 1.5);

    if (myContext.IsNull()) {
        return;
    }
    for (const SketchAnnotations::Label& label : SketchAnnotations::DimensionLabels(theSketch)) {
        Handle(AIS_TextLabel) text = new AIS_TextLabel();
        text->SetText(TCollection_ExtendedString(label.text.c_str()));
        text->SetPosition(theSketch.To3d(label.position));
        text->SetColor(kDimensionColor);
        text->SetHeight(kTextHeight);
        myContext->Display(text, 0, kNoSelectionMode, Standard_False);
        myContext->SetZLayer(text, Graphic3d_ZLayerId_Top);
        mySketchObjects.push_back(text);
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
        myPreview->SetWidth(2.0);
        myPreview->Attributes()->SetPointAspect(
            new Prs3d_PointAspect(Aspect_TOM_O_PLUS, kPreviewColor, 1.6));
        myContext->Display(myPreview, AIS_WireFrame, kNoSelectionMode, Standard_False);
        myContext->SetZLayer(myPreview, Graphic3d_ZLayerId_Top);
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
