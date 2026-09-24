#include "gizmos/PressPullGizmoTool.h"

#include "core/Body.h"
#include "core/GeometrySelection.h"
#include "core/Units.h"
#include "features/PressPullFeature.h"

#include "OcctViewport.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
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

// Fusion's manipulator blue, and the lighter shade it takes under the
// cursor. Matched to the sketch selection colour so the app has one
// "you can grab this" colour rather than three.
const Quantity_Color kArrowColor(0.20, 0.72, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kArrowHoverColor(0.62, 0.88, 1.00, Quantity_TOC_sRGB);
const Quantity_Color kPreviewColor(0.72, 0.78, 0.86, Quantity_TOC_sRGB);

// Anything decorative must say -1 explicitly: the two-argument Display
// overload activates the default selection mode, which is how the origin
// axis lines once ended up stealing clicks from bodies.
constexpr Standard_Integer kNoSelectionMode = -1;

// How close to the arrow a click has to land, in LOGICAL pixels, scaled
// by the device ratio at use. A pixel radius is what the user perceives
// as "on the arrow"; a model-space radius would grow and shrink with the
// zoom.
constexpr double kPickRadiusPixels = 10.0;

// Shorter than this and the drag was a click. The feature refuses a zero
// distance anyway; this stops a stray pixel of travel landing a 0.001mm
// feature on the timeline.
constexpr double kMinDragDistance = 1.0e-4;

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

PressPullGizmoTool& PressPullGizmoTool::Instance()
{
    // Deliberately leaked, same reasoning as MoveGizmoTool::Instance():
    // the AIS handles held here must not be released during static
    // destruction, which runs after the viewer owning the GL context is
    // already gone.
    static PressPullGizmoTool* theInstance = new PressPullGizmoTool();
    return *theInstance;
}

void PressPullGizmoTool::Sync(const CommandContext& theContext)
{
    if (myIsRestoring || myIsDragging) {
        return;  // mid-drag the arrow belongs to the drag, not to the poll
    }

    // The gate. Without it every single-face selection grew an arrow,
    // because this runs off a 300ms poll and on every selection change.
    if (!myIsEnabled) {
        Disarm();
        return;
    }

    myContext = theContext;
    if (myContext.document == nullptr || myContext.AisContext().IsNull()) {
        return;  // the viewer is created lazily; retried on the next poll
    }
    myContext.document->AddObserver(this);  // AddObserver de-duplicates

    const GeometryRef face = GeometrySelection::Instance().SoleItem(EntityType::BRepFace);
    if (face.IsNull()) {
        Disarm();  // nothing picked, or two faces picked: SoleItem refuses to guess
        return;
    }

    // Another TOOL owns the mouse (a sketch is being drawn, the move
    // gizmo is up). Checked before the "same face as last time" shortcut
    // below, because a tool can be pushed over us while the selection
    // stands still -- and an arrow that no longer receives its own drags
    // is worse than no arrow.
    //
    // ExclusiveInteraction(), not CurrentInteraction(): the view cube is
    // pushed for the app's lifetime, so "is anything pushed?" is always
    // true and this guard would refuse to ever show the arrow.
    if (myContext.viewport != nullptr
        && myContext.viewport->ExclusiveInteraction() != nullptr
        && myContext.viewport->ExclusiveInteraction() != this) {
        Disarm();
        return;
    }

    // myFaceKey records which pick has already been ruled on, arrow or no
    // arrow. Without that, a face we cannot press/pull would have its
    // refusal re-decided -- and its message re-shown -- every 300ms.
    const std::string key = face.Encode();
    if (key == myFaceKey) {
        return;
    }

    Disarm();      // clears myFaceKey
    myFaceKey = key;

    TopoDS_Shape resolved;
    if (!ResolveGeometryRefInShape(myContext.document->Shape(), face, resolved)
        || resolved.ShapeType() != TopAbs_FACE) {
        return;  // a stale pick; a rebuild will clear the key and we retry
    }

    PressPullArrow arrow;
    std::string error;
    if (!ComputePressPullArrow(TopoDS::Face(resolved), arrow, error)) {
        ShowStatus(myContext, QString::fromStdString(error));
        return;
    }

    Arm(face, arrow);
}

void PressPullGizmoTool::Start(const CommandContext& theContext)
{
    myIsEnabled = true;
    Sync(theContext);   // arm immediately if a face is already picked
}

void PressPullGizmoTool::Stop()
{
    myIsEnabled = false;
    Disarm();
    if (myContext.document != nullptr) {
        myContext.Redraw();
    }
}

void PressPullGizmoTool::Arm(const GeometryRef& theFace, const PressPullArrow& theArrow)
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull()) {
        return;
    }

    // An arrow that failed to build comes back as an empty compound, not
    // a null shape, and AIS has nothing to draw from one.
    const TopoDS_Shape shape = MakePressPullArrowShape(theArrow.axis, theArrow.length);
    if (shape.IsNull() || !TopoDS_Iterator(shape).More()) {
        return;
    }

    myFace  = theFace;
    myArrow = theArrow;

    myArrowObject = new AIS_Shape(shape);
    // The colour is not decoration: an AIS_Shape only materialises its
    // shading aspect once one is set, and displaying one without it
    // crashes the first time the presentation is built.
    myArrowObject->SetColor(kArrowColor);
    aisContext->Display(myArrowObject, AIS_Shaded, kNoSelectionMode, Standard_False);
    // The arrow stands ON the face it belongs to; without a top layer it
    // z-fights with it and half-disappears.
    aisContext->SetZLayer(myArrowObject, Graphic3d_ZLayerId_Top);

    myIsArmed    = true;
    myIsHovering = false;
    myDistance   = 0.0;

    if (myContext.viewport != nullptr && myContext.viewport->CurrentInteraction() != this) {
        myContext.viewport->PushInteraction(this);
    }
    // Deliberately says nothing here. Arming happens on the same
    // selection change that puts the face's area in the status bar, and
    // it would overwrite it -- the measurement is the more useful of the
    // two, and an arrow standing on the face already says it can be
    // dragged.
    myContext.Redraw();
}

void PressPullGizmoTool::Disarm()
{
    if (!myIsArmed) {
        myFaceKey.clear();
        return;
    }
    if (myIsDragging) {
        EndDrag(false);  // never leave a half-applied drag hanging
    }

    // Cleared first so the OnDeactivated() that PopInteraction() calls
    // back into finds nothing left to do.
    myIsArmed = false;
    myFaceKey.clear();
    myFace = GeometryRef();

    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (!aisContext.IsNull() && !myArrowObject.IsNull()) {
        aisContext->Remove(myArrowObject, Standard_False);
    }
    myArrowObject.Nullify();
    ClearPreview();

    // Only pop when we are still on top: another subsystem may have
    // pushed its handler over ours, and popping would take theirs away.
    if (myContext.viewport != nullptr && myContext.viewport->CurrentInteraction() == this) {
        myContext.viewport->PopInteraction();
    }
    myContext.Redraw();
}

void PressPullGizmoTool::OnDeactivated()
{
    Disarm();
}

void PressPullGizmoTool::OnDocumentChanged(Document& theDocument)
{
    (void)theDocument;
    if (myIsRestoring) {
        return;  // that rebuild notification is our own restore, not a model change
    }
    Disarm();  // the face has moved or gone; Sync() decides about a new arrow
}

bool PressPullGizmoTool::RayThrough(const Graphic3d_Vec2i& thePos, gp_Lin& theRay) const
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

bool PressPullGizmoTool::IsOnArrow(const Graphic3d_Vec2i& thePos) const
{
    const Handle(V3d_View) view = myContext.View();
    if (view.IsNull() || !myIsArmed) {
        return false;
    }

    const gp_Pnt tail = myArrow.axis.Location();
    const gp_Pnt tip  = tail.Translated(gp_Vec(myArrow.axis.Direction()) * myArrow.length);

    Standard_Integer tailX = 0, tailY = 0, tipX = 0, tipY = 0;
    view->Convert(tail.X(), tail.Y(), tail.Z(), tailX, tailY);
    view->Convert(tip.X(), tip.Y(), tip.Z(), tipX, tipY);

    const double radius = kPickRadiusPixels * DevicePixelRatio(myContext);
    return DistanceToSegment2d(thePos.x(), thePos.y(), tailX, tailY, tipX, tipY) <= radius;
}

bool PressPullGizmoTool::OnMousePress(const Graphic3d_Vec2i& thePos,
                                      Qt::MouseButton        theButton,
                                      Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (!myIsArmed || theButton != Qt::LeftButton || !IsOnArrow(thePos)) {
        return false;  // missed the arrow: let normal select/orbit run
    }

    gp_Lin ray;
    if (!RayThrough(thePos, ray) || !ClosestPointOnAxis(myArrow.axis, ray, myDragStart)) {
        // Looking straight down the arrow. Refuse rather than start a
        // drag whose distance would jump about wildly.
        ShowStatus(myContext, "Turn the view: the arrow is pointing at you.");
        return false;
    }

    myIsDragging = true;
    myDistance   = 0.0;
    HideBodies();
    return true;
}

bool PressPullGizmoTool::OnMouseMove(const Graphic3d_Vec2i& thePos,
                                     Qt::MouseButtons       theButtons,
                                     Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (!myIsArmed) {
        return false;
    }

    if (!myIsDragging) {
        // Light the arrow up under the cursor, and otherwise get out of
        // the way so hover-highlight and orbit still work.
        const bool hovering = IsOnArrow(thePos);
        if (hovering != myIsHovering) {
            myIsHovering = hovering;
            const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
            if (!aisContext.IsNull() && !myArrowObject.IsNull()) {
                myArrowObject->SetColor(hovering ? kArrowHoverColor : kArrowColor);
                aisContext->Redisplay(myArrowObject, Standard_False);
                myContext.Redraw();
            }
        }
        return false;
    }

    if (!theButtons.testFlag(Qt::LeftButton)) {
        // The button came up somewhere that never reached OnMouseRelease
        // (released outside the window); don't leave the drag stuck open.
        EndDrag(true);
        return true;
    }

    gp_Lin ray;
    gp_Pnt onAxis;
    if (RayThrough(thePos, ray) && ClosestPointOnAxis(myArrow.axis, ray, onAxis)) {
        myDistance = DragDistanceAlongAxis(myArrow.axis, myDragStart, onAxis);
        MoveArrowTo(myDistance);
        UpdatePreview(myDistance);
        ShowStatus(myContext,
                   QString::fromStdString(FormatValue(myDistance, UnitKind::Length,
                                                      DefaultLengthUnit())));
        myContext.Redraw();
    }
    return true;
}

bool PressPullGizmoTool::OnMouseRelease(const Graphic3d_Vec2i& thePos,
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

bool PressPullGizmoTool::OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers)
{
    (void)theModifiers;
    if (!myIsArmed || theKey != Qt::Key_Escape) {
        return false;
    }
    if (myIsDragging) {
        EndDrag(false);  // cancel the drag, leave the model untouched, keep the arrow
        ShowStatus(myContext, "Press/Pull cancelled.");
        return true;
    }
    Disarm();
    return true;
}

void PressPullGizmoTool::MoveArrowTo(double theDistance)
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull() || myArrowObject.IsNull()) {
        return;
    }
    gp_Trsf trsf;
    trsf.SetTranslation(gp_Vec(myArrow.axis.Direction()) * theDistance);
    aisContext->SetLocation(myArrowObject, TopLoc_Location(trsf));
}

void PressPullGizmoTool::UpdatePreview(double theDistance)
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull() || myContext.document == nullptr) {
        return;
    }
    if (std::fabs(theDistance) < kMinDragDistance) {
        ClearPreview();
        return;
    }

    // The preview IS the feature, computed against the same input shape it
    // will see on the timeline -- so what the drag shows and what release
    // commits cannot drift apart.
    PressPullFeature feature(myFace, theDistance);
    ComputeContext computeContext;
    computeContext.document = myContext.document;

    TopoDS_Shape result;
    std::string error;
    if (!feature.Compute(computeContext, myContext.document->Shape(), result, error)
        || result.IsNull()) {
        // Distances that produce nothing valid (cutting a body clean in
        // half, say) keep the last good preview rather than flickering.
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

void PressPullGizmoTool::ClearPreview()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (!aisContext.IsNull() && !myPreviewObject.IsNull()) {
        aisContext->Remove(myPreviewObject, Standard_False);
    }
    myPreviewObject.Nullify();
}

void PressPullGizmoTool::HideBodies()
{
    myHiddenBodies.clear();
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull() || myContext.document == nullptr) {
        return;
    }

    // Matched by shape identity against the document's bodies rather than
    // by "every AIS_Shape displayed": sketch curves and the origin planes
    // are AIS_Shapes too, and hiding those mid-drag would be a surprise.
    AIS_ListOfInteractive displayed;
    aisContext->DisplayedObjects(displayed);
    for (AIS_ListOfInteractive::Iterator it(displayed); it.More(); it.Next()) {
        const Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(it.Value());
        if (shape.IsNull() || shape == myArrowObject || shape == myPreviewObject) {
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

void PressPullGizmoTool::RestoreBodies()
{
    if (myHiddenBodies.empty()) {
        return;
    }
    myHiddenBodies.clear();

    // Rather than hand-rebuild each body's display mode, selection mode
    // and activated filters -- four calls that would have to agree exactly
    // with MainWindow::redisplayDocument forever -- ask the window to
    // redisplay the document it already knows how to display. NotifyChanged
    // does not re-evaluate the timeline, so this costs nothing.
    //
    // It no-ops when we are already inside a notification -- an undo
    // arriving mid-drag, say -- and that is fine: the notification that
    // got us here is reaching MainWindow's own observer in the same
    // round, and that redisplays every body whether we ask or not.
    if (myContext.document != nullptr) {
        myIsRestoring = true;
        myContext.document->NotifyChanged();
        myIsRestoring = false;
    }
}

void PressPullGizmoTool::EndDrag(bool theApply)
{
    if (!myIsDragging) {
        return;
    }
    myIsDragging = false;

    const double distance = myDistance;
    myDistance = 0.0;

    ClearPreview();
    MoveArrowTo(0.0);
    RestoreBodies();

    if (theApply && std::fabs(distance) >= kMinDragDistance) {
        myDistance = distance;
        Commit();
        myDistance = 0.0;
    }
    myContext.Redraw();
}

void PressPullGizmoTool::Commit()
{
    if (myContext.document == nullptr || myFace.IsNull()) {
        return;
    }

    auto feature = std::make_shared<PressPullFeature>(myFace, myDistance);
    myContext.document->AddFeature(feature);  // rebuilds, snapshots undo, redisplays
    myContext.document->SetActiveFeature(feature);

    if (!feature->LastError().empty()) {
        // Already carries the feature name, courtesy of Document::Rebuild.
        ShowStatus(myContext, QString::fromStdString(feature->LastError()));
    } else {
        ShowStatus(myContext, QString::fromStdString(feature->Name()) + " created.");
    }

    // The face that was dragged does not exist any more, so leaving it
    // selected would leave a highlight on gone geometry and let a second
    // drag act on a stale reference.
    GeometrySelection::Instance().Clear();
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (!aisContext.IsNull()) {
        aisContext->ClearSelected(Standard_False);
    }
}

} // namespace lcad
