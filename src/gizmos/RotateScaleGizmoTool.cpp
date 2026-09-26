#include "gizmos/RotateScaleGizmoTool.h"
#include "gizmos/TransformFeature.h"

#include "core/Body.h"
#include "core/Units.h"

#include "OcctViewport.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <Aspect_Window.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Iterator.hxx>
#include <V3d_View.hxx>
#include <gp.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <memory>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>
#include <QWidget>

namespace lcad {

namespace {

// The origin axis lines' own colours, so a red ring turns about the red
// axis and nobody has to look it up. Taken from OcctViewport's axes
// rather than invented beside them.
const Quantity_Color kAxisColors[3] = {
    Quantity_Color(0.85, 0.20, 0.20, Quantity_TOC_sRGB),  // X
    Quantity_Color(0.25, 0.80, 0.25, Quantity_TOC_sRGB),  // Y
    Quantity_Color(0.25, 0.45, 0.95, Quantity_TOC_sRGB)   // Z
};

// The press/pull arrow's blue, so the app has one "you can grab this"
// colour rather than three, and its lighter shade under the cursor.
const Quantity_Color kScaleColor(0.20, 0.72, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kHoverColor(0.95, 0.85, 0.35, Quantity_TOC_sRGB);
const Quantity_Color kPreviewColor(0.72, 0.78, 0.86, Quantity_TOC_sRGB);

// Anything decorative must say -1 EXPLICITLY: the two-argument Display
// overload activates the default selection mode, which is how the origin
// axis lines once ended up stealing clicks from bodies. A ring drawn
// round a body sits between the cursor and most of it, so this one would
// have stolen a great deal more.
constexpr Standard_Integer kNoSelectionMode = -1;

// Cube on the end of the scale stalk, as a fraction of the stalk's rest
// length.
constexpr double kScaleCubeShare = 0.09;

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

double DevicePixelRatio(const CommandContext& theContext)
{
    if (theContext.parent != nullptr) {
        const double ratio = theContext.parent->devicePixelRatioF();
        if (ratio > 0.0) {
            return ratio;
        }
    }
    return 1.0;
}

} // namespace

RotateScaleGizmoTool& RotateScaleGizmoTool::Instance()
{
    // Deliberately leaked, same reasoning as MoveGizmoTool::Instance():
    // the AIS handles held here must not be released during static
    // destruction, which runs after the viewer owning the GL context is
    // already gone.
    static RotateScaleGizmoTool* theInstance = new RotateScaleGizmoTool();
    return *theInstance;
}

void RotateScaleGizmoTool::Start(const CommandContext& theContext, Mode theMode)
{
    Stop();  // re-invoking re-reads the selection rather than stacking handles

    myContext = theContext;
    myMode    = theMode;

    if (myContext.document == nullptr || myContext.AisContext().IsNull()) {
        return;  // the viewer is created lazily; the button is greyed out until then
    }
    if (!AttachToSelection()) {
        ShowStatus(myContext, theMode == Mode::Rotate
                                  ? "Select a body first, then Rotate."
                                  : "Select a body first, then Scale.");
        return;
    }

    myContext.document->AddObserver(this);  // AddObserver de-duplicates
    // Set before BuildHandles so the Stop() below has something to undo:
    // Stop() on an inactive tool is a no-op, which would leave the
    // observer registered for the life of the app.
    myIsActive = true;

    BuildHandles();
    if (myHandles.empty()) {
        // Nothing to grab is worse than no tool at all: say so and leave
        // the viewport alone rather than swallowing every click.
        Stop();
        ShowStatus(myContext, "Could not build the handles for that body.");
        return;
    }

    if (myContext.viewport != nullptr) {
        myContext.viewport->PushInteraction(this);
    }

    QString hint = myMode == Mode::Rotate
                       ? QString("Drag a ring to turn '%1' about its centre. Esc cancels.")
                             .arg(QString::fromStdString(myBodyName))
                       : QString("Drag the handle out to grow '%1', in to shrink it. Esc cancels.")
                             .arg(QString::fromStdString(myBodyName));
    if (myBody.IsNull()) {
        // A body with no volume cannot carry a signature, so the feature
        // has nothing to resolve and falls back to the whole model. That
        // is a real difference in what the drag will do, so it is said
        // out loud rather than discovered.
        hint += " (no volume to pick out: the whole model moves)";
    }
    ShowStatus(myContext, hint);
    myContext.Redraw();
}

void RotateScaleGizmoTool::Stop()
{
    if (!myIsActive) {
        return;
    }
    // Only pop when we are still on top: another subsystem may have
    // pushed its handler over ours, and popping would take theirs away
    // instead (see SketchTool::Stop() for the same reasoning).
    if (myContext.viewport != nullptr && myContext.viewport->CurrentInteraction() == this) {
        myContext.viewport->PopInteraction();  // calls OnDeactivated()
    } else {
        OnDeactivated();
    }
}

void RotateScaleGizmoTool::OnDeactivated()
{
    if (myIsDragging) {
        EndDrag(false);  // never leave a half-applied drag hanging
    }
    // Cleared first so a Stop() arriving from inside here finds nothing
    // left to do.
    myIsActive = false;

    ClearPreview();
    ClearHandles();
    RestoreBodies();

    if (myContext.document != nullptr) {
        myContext.document->RemoveObserver(this);
    }
    myBody = CombineBodyRef();
    myBodyName.clear();
    myHoverHandle = -1;
    myDragHandle  = -1;
    myContext.Redraw();
}

void RotateScaleGizmoTool::OnDocumentChanged(Document& theDocument)
{
    (void)theDocument;
    if (myIsRestoring || myIsCommitting || !myIsActive) {
        return;  // that notification is our own, not the model changing under us
    }
    Stop();
}

bool RotateScaleGizmoTool::AttachToSelection()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull() || myContext.document == nullptr) {
        return false;
    }

    // MainWindow displays one AIS_Shape per body, and SelectedInteractive()
    // gives the OWNER even when the pick was one of its faces -- which it
    // usually is, because the default filters have whole bodies off.
    // Matched by shape identity against the document's own bodies, the
    // only signal that means what it says.
    for (aisContext->InitSelected(); aisContext->MoreSelected(); aisContext->NextSelected()) {
        const Handle(AIS_Shape) picked =
            Handle(AIS_Shape)::DownCast(aisContext->SelectedInteractive());
        if (picked.IsNull() || picked->Shape().IsNull()) {
            continue;
        }
        for (const BodyPtr& body : myContext.document->Bodies()) {
            if (!body || body->Shape().IsNull() || !picked->Shape().IsSame(body->Shape())) {
                continue;
            }
            myBodyName = body->Name();
            myPivot    = body->Centroid();
            // Null for anything without a volume; Start() says so.
            myBody = MakeCombineBodyRef(*body);
            return true;
        }
    }
    return false;
}

GizmoScreen RotateScaleGizmoTool::Screen() const
{
    const Handle(V3d_View) view = myContext.View();
    if (view.IsNull() || view->Window().IsNull()) {
        return GizmoScreen();
    }
    Standard_Integer width = 0, height = 0;
    view->Window()->Size(width, height);
    return GizmoScreen(view->Camera(), width, height);
}

double RotateScaleGizmoTool::DevicePixels(double theLogicalPixels) const
{
    return theLogicalPixels * DevicePixelRatio(myContext);
}

double RotateScaleGizmoTool::Reach() const
{
    const double pixels = myMode == Mode::Rotate ? RotateRingRadiusPixels()
                                                 : ScaleHandleLengthPixels();
    return DevicePixels(pixels) * Screen().ModelPerPixel(myPivot);
}

gp_Ax1 RotateScaleGizmoTool::ScaleAxis() const
{
    return gp_Ax1(myPivot, ScaleHandleDirection());
}

TopoDS_Shape RotateScaleGizmoTool::ScaleHandleShape(double theFactor) const
{
    const double length = DevicePixels(ScaleHandleLengthPixels());
    return MakeScaleHandleShape(gp_Ax1(gp::Origin(), ScaleHandleDirection()),
                                length * theFactor, length * kScaleCubeShare);
}

void RotateScaleGizmoTool::BuildHandles()
{
    ClearHandles();
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull()) {
        return;
    }

    // Built in device pixels about the origin and drawn zoom-persistent,
    // anchored on the pivot: OCCT then scales them so one unit is one
    // pixel at every zoom, the way Fusion's manipulators hold their size
    // on screen whatever the wheel does. The hit test asks GizmoScreen for
    // the same scale, so what is drawn and what is grabbable agree.
    const Handle(Graphic3d_TransformPers) persistence =
        new Graphic3d_TransformPers(Graphic3d_TMF_ZoomPers, myPivot);

    auto display = [&](const TopoDS_Shape& theShape, const Quantity_Color& theColor) {
        // A handle that failed to build comes back as an empty compound,
        // not a null shape, and AIS has nothing to draw from one.
        if (theShape.IsNull() || !TopoDS_Iterator(theShape).More()) {
            return;
        }
        Handle(AIS_Shape) object = new AIS_Shape(theShape);
        // The colour is not decoration: an AIS_Shape only materialises
        // its shading aspect once one is set, and displaying one without
        // it crashes the first time the presentation is built.
        object->SetColor(theColor);
        object->SetTransformPersistence(persistence);
        // Topmost has a depth buffer of its own, so the handles draw OVER
        // the body rather than into it. A screen-sized ring is inside a
        // body's silhouette as soon as the body is zoomed up big, and in
        // the plain Top layer the body hid it there -- a manipulator you
        // cannot see is one you cannot grab.
        object->SetZLayer(Graphic3d_ZLayerId_Topmost);
        aisContext->Display(object, AIS_Shaded, kNoSelectionMode, Standard_False);
        myHandles.push_back(object);
    };

    if (myMode == Mode::Rotate) {
        const double radius = DevicePixels(RotateRingRadiusPixels());
        for (int axis = 0; axis < 3; ++axis) {
            display(MakeRotateRingShape(GizmoAxis(axis, gp::Origin()), radius),
                    kAxisColors[axis]);
        }
        // All three or none: a partial set would leave one axis silently
        // un-rotatable, and the index a drag stores is the axis number.
        if (myHandles.size() != 3) {
            ClearHandles();
        }
    } else {
        display(ScaleHandleShape(1.0), kScaleColor);
    }
}

void RotateScaleGizmoTool::ResetHandles()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull() || myMode != Mode::Scale || myHandles.empty()) {
        return;
    }
    const TopoDS_Shape shape = ScaleHandleShape(1.0);
    if (shape.IsNull() || !TopoDS_Iterator(shape).More()) {
        return;
    }
    myHandles.front()->SetShape(shape);
    aisContext->Redisplay(myHandles.front(), Standard_False);
}

void RotateScaleGizmoTool::ClearHandles()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    for (Handle(AIS_Shape)& handle : myHandles) {
        if (!aisContext.IsNull() && !handle.IsNull()) {
            aisContext->Remove(handle, Standard_False);
        }
        handle.Nullify();
    }
    myHandles.clear();
}

void RotateScaleGizmoTool::Highlight(int theHandle)
{
    if (theHandle == myHoverHandle) {
        return;
    }
    myHoverHandle = theHandle;

    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull()) {
        return;
    }
    for (std::size_t i = 0; i < myHandles.size(); ++i) {
        if (myHandles[i].IsNull()) {
            continue;
        }
        const bool lit = static_cast<int>(i) == theHandle;
        const Quantity_Color rest =
            myMode == Mode::Rotate ? kAxisColors[i % 3] : kScaleColor;
        myHandles[i]->SetColor(lit ? kHoverColor : rest);
        aisContext->Redisplay(myHandles[i], Standard_False);
    }
    myContext.Redraw();
}

int RotateScaleGizmoTool::HandleUnder(const Graphic3d_Vec2i& thePos) const
{
    if (!myIsActive || myHandles.empty()) {
        return -1;
    }
    const GizmoScreen screen = Screen();
    const double reach = Reach();
    const double pick  = DevicePixels(GizmoPickRadiusPixels());
    if (myMode == Mode::Scale) {
        return PickScaleHandle(screen, ScaleAxis(), reach, thePos.x(), thePos.y(), pick) ? 0 : -1;
    }
    return PickRotateRing(screen, myPivot, reach, thePos.x(), thePos.y(), pick);
}

bool RotateScaleGizmoTool::OnMousePress(const Graphic3d_Vec2i& thePos,
                                        Qt::MouseButton        theButton,
                                        Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (!myIsActive || theButton != Qt::LeftButton) {
        return false;
    }

    const int handle = HandleUnder(thePos);
    if (handle < 0) {
        return false;  // missed every handle: let normal select/orbit run
    }

    gp_Lin ray;
    if (!Screen().RayThrough(thePos.x(), thePos.y(), ray)) {
        return false;
    }

    if (myMode == Mode::Rotate) {
        if (!myRingDrag.Begin(GizmoAxis(handle, myPivot), ray)) {
            // Edge-on. Refuse rather than start a drag whose angle would
            // swing by whole degrees for every pixel of mouse travel.
            ShowStatus(myContext, "Turn the view: that ring is edge-on.");
            return false;
        }
    } else {
        // The rest length at THIS zoom: the handle is a fixed size on
        // screen, so its length in model units is whatever the zoom makes
        // it now, and the drag's factor is measured against that.
        if (!myScaleDrag.Begin(ScaleAxis(), Reach(), ray)) {
            ShowStatus(myContext, "Turn the view: the scale handle is pointing at you.");
            return false;
        }
    }

    myDragHandle = handle;
    myIsDragging = true;
    HideBodies();
    return true;
}

bool RotateScaleGizmoTool::OnMouseMove(const Graphic3d_Vec2i& thePos,
                                       Qt::MouseButtons       theButtons,
                                       Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (!myIsActive) {
        return false;
    }

    if (!myIsDragging) {
        // Light the handle up under the cursor, and otherwise get out of
        // the way so hover-highlight and orbit still work.
        Highlight(HandleUnder(thePos));
        return false;
    }

    if (!theButtons.testFlag(Qt::LeftButton)) {
        // The button came up somewhere that never reached OnMouseRelease
        // (released outside the window); don't leave the drag stuck open.
        EndDrag(true);
        return true;
    }

    gp_Lin ray;
    if (!Screen().RayThrough(thePos.x(), thePos.y(), ray)) {
        return true;
    }

    if (myMode == Mode::Rotate) {
        // Edge-on mid-drag keeps the last good angle rather than jumping
        // to a number the cursor never asked for.
        myRingDrag.Update(ray);
        ShowStatus(myContext, QString::fromStdString(
                                  FormatValue(myRingDrag.TotalDegrees(), UnitKind::Angle)));
    } else {
        if (!myScaleDrag.Update(ray)) {
            // Dragged onto the pivot or past it. Hold the last good
            // factor: previewing a collapsed or inside-out body, and then
            // committing it on release, is the wrong solid nobody notices.
            ShowStatus(myContext, "A scale has to stay above zero -- drag back out.");
            return true;
        }

        // The stalk GROWS with the drag rather than sliding out along
        // itself, so its base stays on the pivot the scale is about.
        const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
        if (!aisContext.IsNull() && !myHandles.empty() && !myHandles.front().IsNull()) {
            const TopoDS_Shape stretched = ScaleHandleShape(myScaleDrag.Factor());
            if (!stretched.IsNull() && TopoDS_Iterator(stretched).More()) {
                myHandles.front()->SetShape(stretched);
                aisContext->Redisplay(myHandles.front(), Standard_False);
            }
        }
        ShowStatus(myContext,
                   QString("Scale %1x")
                       .arg(QString::fromStdString(
                           FormatValue(myScaleDrag.Factor(), UnitKind::Unitless))));
    }

    UpdatePreview();
    myContext.Redraw();
    return true;
}

bool RotateScaleGizmoTool::OnMouseRelease(const Graphic3d_Vec2i& thePos,
                                          Qt::MouseButton        theButton,
                                          Qt::KeyboardModifiers  theModifiers)
{
    (void)thePos;
    (void)theModifiers;
    if (!myIsDragging || theButton != Qt::LeftButton) {
        return false;
    }
    EndDrag(true);
    return true;
}

bool RotateScaleGizmoTool::OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers)
{
    (void)theModifiers;
    if (!myIsActive || theKey != Qt::Key_Escape) {
        return false;
    }
    if (myIsDragging) {
        EndDrag(false);  // cancel the drag, leave the model untouched, keep the handles
        ShowStatus(myContext, myMode == Mode::Rotate ? "Rotate cancelled." : "Scale cancelled.");
        return true;
    }
    Stop();
    return true;
}

std::shared_ptr<TransformFeature> RotateScaleGizmoTool::MakeFeature() const
{
    double rx = 0.0, ry = 0.0, rz = 0.0;
    if (myMode == Mode::Rotate) {
        // DEGREES from here on: TransformFeature stores degrees, and the
        // single conversion back to radians is inside its Trsf().
        const double degrees = myRingDrag.TotalDegrees();
        if (myDragHandle == 0)      rx = degrees;
        else if (myDragHandle == 1) ry = degrees;
        else if (myDragHandle == 2) rz = degrees;
    }

    // No translation, ever: the pivot is the body's own centroid, so a
    // rotation leaves it exactly where it was and a scale grows the body
    // around it. That is what makes the timeline replay land where the
    // live drag left it.
    auto feature = std::make_shared<TransformFeature>(0.0, 0.0, 0.0, rx, ry, rz, myPivot);
    feature->SetTarget(myBody);
    if (myMode == Mode::Scale) {
        // Already through ScaleFactorFromDrag's refusal, so this cannot
        // fail -- and if it ever did, the feature keeps its factor of one
        // and the drag commits nothing rather than something wrong.
        feature->SetScale(myScaleDrag.Factor());
    }
    return feature;
}

void RotateScaleGizmoTool::UpdatePreview()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull() || myContext.document == nullptr) {
        return;
    }

    // The preview IS the feature, computed against the same input shape
    // it will see on the timeline -- so what the drag shows and what
    // release commits cannot drift apart.
    const std::shared_ptr<TransformFeature> feature = MakeFeature();
    ComputeContext computeContext;
    computeContext.document = myContext.document;

    TopoDS_Shape result;
    std::string  error;
    if (!feature->Compute(computeContext, myContext.document->Shape(), result, error)
        || result.IsNull()) {
        // Keep the last good preview rather than flickering; the same
        // refusal will reach the user on release if it still applies.
        return;
    }

    if (myPreviewObject.IsNull()) {
        myPreviewObject = new AIS_Shape(result);
        myPreviewObject->SetColor(kPreviewColor);  // materialises the shading aspect
        aisContext->Display(myPreviewObject, AIS_Shaded, kNoSelectionMode, Standard_False);
    } else {
        myPreviewObject->SetShape(result);
        aisContext->Redisplay(myPreviewObject, Standard_False);
    }
}

void RotateScaleGizmoTool::ClearPreview()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (!aisContext.IsNull() && !myPreviewObject.IsNull()) {
        aisContext->Remove(myPreviewObject, Standard_False);
    }
    myPreviewObject.Nullify();
}

void RotateScaleGizmoTool::HideBodies()
{
    myHiddenBodies.clear();
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull() || myContext.document == nullptr) {
        return;
    }

    // Matched by shape identity against the document's bodies rather than
    // "every AIS_Shape displayed": sketch curves, the origin planes and
    // our own handles are AIS_Shapes too.
    AIS_ListOfInteractive displayed;
    aisContext->DisplayedObjects(displayed);
    for (AIS_ListOfInteractive::Iterator it(displayed); it.More(); it.Next()) {
        const Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(it.Value());
        if (shape.IsNull() || shape == myPreviewObject) {
            continue;
        }
        for (const BodyPtr& body : myContext.document->Bodies()) {
            if (body && !body->Shape().IsNull() && shape->Shape().IsSame(body->Shape())) {
                aisContext->Erase(shape, Standard_False);
                myHiddenBodies.push_back(shape);
                break;
            }
        }
    }
}

void RotateScaleGizmoTool::RestoreBodies()
{
    if (myHiddenBodies.empty()) {
        return;
    }
    myHiddenBodies.clear();

    // Rather than hand-rebuild each body's display mode, selection mode
    // and activated filters -- four calls that would have to agree with
    // MainWindow::redisplayDocument forever -- ask the window to redisplay
    // the document it already knows how to display. NotifyChanged does not
    // re-evaluate the timeline, so this costs nothing.
    if (myContext.document != nullptr) {
        myIsRestoring = true;
        myContext.document->NotifyChanged();
        myIsRestoring = false;
    }
}

void RotateScaleGizmoTool::EndDrag(bool theApply)
{
    if (!myIsDragging) {
        return;
    }
    myIsDragging = false;

    const double degrees = myRingDrag.TotalDegrees();
    const double factor  = myScaleDrag.Factor();

    ClearPreview();
    ResetHandles();
    RestoreBodies();

    // A click that never actually moved the handle is not an edit. The
    // feature would refuse a zero angle anyway; this stops a stray pixel
    // of travel landing a 0.001-degree turn on the timeline.
    const bool worthCommitting =
        myMode == Mode::Rotate ? std::fabs(degrees) >= MinimumDragAngleDegrees()
                               : std::fabs(factor - 1.0) >= MinimumScaleChange();

    if (theApply && worthCommitting) {
        Commit();  // reads the drags, still set
        return;    // Commit() ends the tool; nothing below would be seen
    }

    myRingDrag   = RingDrag();
    myScaleDrag  = ScaleDrag();
    myDragHandle = -1;
    myContext.Redraw();
}

void RotateScaleGizmoTool::Commit()
{
    if (myContext.document == nullptr) {
        return;
    }

    const std::shared_ptr<TransformFeature> feature = MakeFeature();
    // Named for what it IS in the timeline. TypeName() stays "Move" for
    // every TransformFeature -- it is the class identifier the browser
    // keys off, not a label -- and this is the seam meant for the label.
    feature->SetName(myContext.document->MakeUniqueName(
        myMode == Mode::Rotate ? "Rotate" : "Scale"));

    // AddFeature rebuilds and notifies synchronously, and that
    // notification is ours: without this guard OnDocumentChanged would
    // read it as the model changing underneath us and stop the tool from
    // inside its own commit.
    myIsCommitting = true;
    myContext.document->AddFeature(feature);  // rebuilds, snapshots undo, redisplays
    myContext.document->SetActiveFeature(feature);
    myIsCommitting = false;

    myRingDrag   = RingDrag();
    myScaleDrag  = ScaleDrag();
    myDragHandle = -1;

    // A failed feature already carries its own name in the message,
    // courtesy of Document::Rebuild -- and it stays on the timeline, red,
    // rather than being thrown away, so the pick can be fixed in the
    // properties panel instead of re-dragged.
    const QString message = QString::fromStdString(
        feature->LastError().empty() ? feature->Name() + " created." : feature->LastError());

    // One-shot: the body this tool is holding a pivot and a signature for
    // has just moved or changed size, so both describe something that is
    // no longer there. See the header for why re-deriving them would be
    // guesswork. Said after Stop(), which redraws and would otherwise
    // talk over the result.
    Stop();
    ShowStatus(myContext, message);
}

} // namespace lcad
