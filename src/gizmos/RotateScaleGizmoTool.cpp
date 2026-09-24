#include "gizmos/RotateScaleGizmoTool.h"
#include "gizmos/TransformFeature.h"

#include "core/Body.h"
#include "core/Units.h"

#include "OcctViewport.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Iterator.hxx>
#include <V3d_View.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <memory>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>
#include <QWidget>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;

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

// How close to a handle a click has to land, in LOGICAL pixels, scaled by
// the device ratio at use. A pixel radius is what the user perceives as
// "on the ring"; a model-space radius would grow and shrink with the zoom.
constexpr double kPickRadiusPixels = 10.0;

// Points round a ring for the hit test. At 64 the gap between samples is
// under a tenth of the radius, so the straight segments between them are
// well inside the pick radius of the true circle at any sane zoom.
constexpr int kRingSamples = 64;

// The scale handle is only grabbable along its outer half. The inner half
// runs through the body, where a click means "select this face" far more
// often than it means "resize".
constexpr double kScaleGrabFrom = 0.5;

// Cube on the end of the scale stalk, as a fraction of the stand-off.
constexpr double kScaleCubeShare = 0.09;

// A body too small to measure still needs handles someone can hit.
constexpr double kFallbackBodySize = 10.0;

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

// Corner to corner: the only length scale a body of any shape offers.
double BodySizeOf(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return 0.0;
    }
    try {
        Bnd_Box box;
        BRepBndLib::Add(theShape, box, Standard_False);
        if (box.IsVoid()) {
            return 0.0;
        }
        Standard_Real x1 = 0, y1 = 0, z1 = 0, x2 = 0, y2 = 0, z2 = 0;
        box.Get(x1, y1, z1, x2, y2, z2);
        return gp_Pnt(x1, y1, z1).Distance(gp_Pnt(x2, y2, z2));
    } catch (const Standard_Failure&) {
        return 0.0;
    }
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
        ShowStatus(myContext, "That body is too small to put handles on.");
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
            myBodySize = BodySizeOf(body->Shape());
            if (!(myBodySize > 0.0)) {
                myBodySize = kFallbackBodySize;
            }
            // Null for anything without a volume; Start() says so.
            myBody = MakeCombineBodyRef(*body);
            myReach = myMode == Mode::Rotate ? RotateRingRadius(myBodySize)
                                             : ScaleHandleOffset(myBodySize);
            return true;
        }
    }
    return false;
}

gp_Ax1 RotateScaleGizmoTool::ScaleAxis() const
{
    return gp_Ax1(myPivot, ScaleHandleDirection());
}

void RotateScaleGizmoTool::BuildHandles()
{
    ClearHandles();
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull()) {
        return;
    }

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
        aisContext->Display(object, AIS_Shaded, kNoSelectionMode, Standard_False);
        // The handles wrap round the body they belong to; without a top
        // layer they disappear into it from half the viewing angles.
        aisContext->SetZLayer(object, Graphic3d_ZLayerId_Top);
        myHandles.push_back(object);
    };

    if (myMode == Mode::Rotate) {
        for (int axis = 0; axis < 3; ++axis) {
            display(MakeRotateRingShape(GizmoAxis(axis, myPivot), myReach), kAxisColors[axis]);
        }
        // All three or none: a partial set would leave one axis silently
        // un-rotatable, and the index a drag stores is the axis number.
        if (myHandles.size() != 3) {
            ClearHandles();
        }
    } else {
        display(MakeScaleHandleShape(ScaleAxis(), myReach, myReach * kScaleCubeShare),
                kScaleColor);
    }
}

void RotateScaleGizmoTool::ResetHandles()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull() || myMode != Mode::Scale || myHandles.empty()) {
        return;
    }
    const TopoDS_Shape shape =
        MakeScaleHandleShape(ScaleAxis(), myReach, myReach * kScaleCubeShare);
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

bool RotateScaleGizmoTool::RayThrough(const Graphic3d_Vec2i& thePos, gp_Lin& theRay) const
{
    const Handle(V3d_View) view = myContext.View();
    if (view.IsNull()) {
        return false;
    }
    try {
        Standard_Real x = 0.0, y = 0.0, z = 0.0, vx = 0.0, vy = 0.0, vz = 0.0;
        view->ConvertWithProj(thePos.x(), thePos.y(), x, y, z, vx, vy, vz);
        theRay = gp_Lin(gp_Pnt(x, y, z), gp_Dir(vx, vy, vz));
        return true;
    } catch (const Standard_Failure&) {
        return false;  // a degenerate projection direction, nothing to drag along
    }
}

int RotateScaleGizmoTool::HandleUnder(const Graphic3d_Vec2i& thePos) const
{
    const Handle(V3d_View) view = myContext.View();
    if (view.IsNull() || !myIsActive || myHandles.empty()) {
        return -1;
    }

    const double radius = kPickRadiusPixels * DevicePixelRatio(myContext);

    try {
        if (myMode == Mode::Scale) {
            // Only the outer half of the stalk, plus the cube on its end:
            // the inner half runs through the body, where a click almost
            // always means "pick this face".
            const gp_Ax1 axis = ScaleAxis();
            const gp_Vec along(axis.Direction());
            const gp_Pnt from = axis.Location().Translated(along * (myReach * kScaleGrabFrom));
            const gp_Pnt to   = axis.Location().Translated(along * myReach);

            Standard_Integer fromX = 0, fromY = 0, toX = 0, toY = 0;
            view->Convert(from.X(), from.Y(), from.Z(), fromX, fromY);
            view->Convert(to.X(), to.Y(), to.Z(), toX, toY);
            return DistanceToSegment2d(thePos.x(), thePos.y(), fromX, fromY, toX, toY) <= radius
                       ? 0
                       : -1;
        }

        // The nearest ring wins, so where two cross on screen the click
        // goes to the one actually under the cursor rather than to
        // whichever happens to be tested first.
        int    winner = -1;
        double best   = radius;
        for (int axis = 0; axis < 3; ++axis) {
            const std::vector<gp_Pnt> points =
                SampleRing(GizmoAxis(axis, myPivot), myReach, kRingSamples);
            if (points.size() < 3) {
                continue;
            }

            std::vector<double> xs(points.size(), 0.0);
            std::vector<double> ys(points.size(), 0.0);
            for (std::size_t i = 0; i < points.size(); ++i) {
                Standard_Integer px = 0, py = 0;
                view->Convert(points[i].X(), points[i].Y(), points[i].Z(), px, py);
                xs[i] = px;
                ys[i] = py;
            }

            for (std::size_t i = 0; i < points.size(); ++i) {
                const std::size_t next = (i + 1) % points.size();  // close the loop
                const double distance = DistanceToSegment2d(thePos.x(), thePos.y(),
                                                            xs[i], ys[i], xs[next], ys[next]);
                if (distance < best) {
                    best   = distance;
                    winner = axis;
                }
            }
        }
        return winner;
    } catch (const Standard_Failure&) {
        return -1;
    }
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
    if (!RayThrough(thePos, ray)) {
        return false;
    }

    if (myMode == Mode::Rotate) {
        double angle = 0.0;
        if (!AngleOnPlane(GizmoAxis(handle, myPivot), ray, angle)) {
            // Edge-on. Refuse rather than start a drag whose angle would
            // swing by whole degrees for every pixel of mouse travel.
            ShowStatus(myContext, "Turn the view: that ring is edge-on.");
            return false;
        }
        myLastAngle  = angle;
        myTotalAngle = 0.0;
    } else {
        if (!ClosestPointOnAxis(ScaleAxis(), ray, myDragStart)) {
            ShowStatus(myContext, "Turn the view: the scale handle is pointing at you.");
            return false;
        }
        myFactor = 1.0;
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
    if (!RayThrough(thePos, ray)) {
        return true;
    }

    if (myMode == Mode::Rotate) {
        double angle = 0.0;
        if (!AngleOnPlane(GizmoAxis(myDragHandle, myPivot), ray, angle)) {
            // Orbited edge-on mid-drag. Keep the last good angle rather
            // than jumping to a number the cursor never asked for.
            return true;
        }
        // Accumulated from WRAPPED differences, so a drag can pass half a
        // turn, or cross the frame's zero, without the total jumping sign.
        myTotalAngle += WrapAngle(angle - myLastAngle);
        myLastAngle = angle;
        ShowStatus(myContext, QString::fromStdString(
                                  FormatValue(myTotalAngle * kRadToDeg, UnitKind::Angle)));
    } else {
        gp_Pnt onAxis;
        const gp_Ax1 axis = ScaleAxis();
        if (!ClosestPointOnAxis(axis, ray, onAxis)) {
            return true;
        }
        const double travel = DragDistanceAlongAxis(axis, myDragStart, onAxis);
        double factor = 1.0;
        if (!ScaleFactorFromDrag(myReach, travel, factor)) {
            // Dragged onto the pivot or past it. Hold the last good
            // factor: previewing a collapsed or inside-out body, and then
            // committing it on release, is the wrong solid nobody notices.
            ShowStatus(myContext, "A scale has to stay above zero -- drag back out.");
            return true;
        }
        myFactor = factor;

        // The stalk GROWS with the drag rather than sliding out along
        // itself, so its base stays on the pivot the scale is about.
        const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
        if (!aisContext.IsNull() && !myHandles.empty() && !myHandles.front().IsNull()) {
            const TopoDS_Shape stretched = MakeScaleHandleShape(
                axis, myReach * myFactor, myReach * kScaleCubeShare);
            if (!stretched.IsNull() && TopoDS_Iterator(stretched).More()) {
                myHandles.front()->SetShape(stretched);
                aisContext->Redisplay(myHandles.front(), Standard_False);
            }
        }
        ShowStatus(myContext,
                   QString("Scale %1x")
                       .arg(QString::fromStdString(FormatValue(myFactor, UnitKind::Unitless))));
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
        const double degrees = myTotalAngle * kRadToDeg;
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
        feature->SetScale(myFactor);
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

    const double degrees = myTotalAngle * kRadToDeg;
    const double factor  = myFactor;

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
        Commit();  // reads myTotalAngle / myFactor, still set
        return;    // Commit() ends the tool; nothing below would be seen
    }

    myTotalAngle = 0.0;
    myFactor     = 1.0;
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

    myTotalAngle = 0.0;
    myFactor     = 1.0;
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
