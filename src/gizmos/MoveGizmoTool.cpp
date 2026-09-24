#include "gizmos/MoveGizmoTool.h"
#include "gizmos/TransformFeature.h"

#include "core/Body.h"

#include "OcctViewport.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <AIS_Shape.hxx>
#include <Standard_Failure.hxx>
#include <V3d_View.hxx>
#include <gp.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_XYZ.hxx>

#include <memory>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

// Re-find a body's AIS object after a rebuild swapped it out from under
// the gizmo.
//
// Matched by SHAPE IDENTITY against the document's own bodies, which is
// the only signal that means what it says. This used to look for the one
// AIS_Shape with selection mode 0 active, back when MainWindow displayed
// the whole document as a single object; that test now answers wrongly in
// both directions. Mode 0 is AIS_Shape::SelectionMode(TopAbs_SHAPE), so
// with the default filters (faces and edges, whole bodies OFF) NO object
// has it and the gizmo detached itself the instant its own drag
// committed -- and with the body filter on, EVERY body has it and the
// first one iterated wins regardless of which was being moved.
Handle(AIS_Shape) FindDisplayedBody(const Handle(AIS_InteractiveContext)& theContext,
                                    const Document*                       theDocument)
{
    Handle(AIS_Shape) result;
    if (theContext.IsNull() || theDocument == nullptr) {
        return result;
    }

    AIS_ListOfInteractive displayed;
    theContext->DisplayedObjects(displayed);
    for (AIS_ListOfInteractive::Iterator it(displayed); it.More(); it.Next()) {
        Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(it.Value());
        if (shape.IsNull()) {
            continue;
        }
        for (const BodyPtr& body : theDocument->Bodies()) {
            if (body && !body->Shape().IsNull() && shape->Shape().IsSame(body->Shape())) {
                return shape;
            }
        }
    }
    return result;
}

// The body a displayed object stands for, as a durable reference.
// MainWindow displays one AIS_Shape per body, so the shape the
// manipulator is attached to IS a body's shape -- matched by identity
// rather than by name, because the name is what we are trying to learn.
CombineBodyRef BodyRefFor(const Handle(AIS_InteractiveObject)& theObject,
                          const Document*                     theDocument)
{
    Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(theObject);
    if (shape.IsNull() || theDocument == nullptr || shape->Shape().IsNull()) {
        return CombineBodyRef();
    }
    for (const BodyPtr& body : theDocument->Bodies()) {
        if (body && !body->Shape().IsNull() && shape->Shape().IsSame(body->Shape())) {
            return MakeCombineBodyRef(*body);
        }
    }
    return CombineBodyRef();
}

} // namespace

MoveGizmoTool& MoveGizmoTool::Instance()
{
    // Deliberately leaked, same reasoning as SketchDisplay::Instance():
    // the Handle(AIS_Manipulator) member must not be released during
    // static destruction, which runs after the Qt/OCCT viewer that owns
    // the GL context is already gone.
    static MoveGizmoTool* theInstance = new MoveGizmoTool();
    return *theInstance;
}

void MoveGizmoTool::Start(const CommandContext& theContext)
{
    myContext = theContext;

    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull() || myContext.document == nullptr) {
        return;
    }

    Handle(AIS_InteractiveObject) target;
    for (aisContext->InitSelected(); aisContext->MoreSelected(); aisContext->NextSelected()) {
        target = aisContext->SelectedInteractive();
        if (!target.IsNull()) {
            break;
        }
    }
    if (target.IsNull()) {
        ShowStatus(myContext, "Select a body first, then Move.");
        return;
    }

    myContext.document->AddObserver(this);  // AddObserver de-duplicates
    AttachTo(target);
    myIsAttached = true;

    if (myContext.viewport != nullptr) {
        myContext.viewport->PushInteraction(this);
    }
    ShowStatus(myContext, "Drag an arrow to move, a ring to rotate. Esc or M again to finish.");
    myContext.Redraw();
}

void MoveGizmoTool::Stop()
{
    if (!myIsAttached) {
        return;
    }
    // Only pop when we're still on top: another subsystem may have pushed
    // its own handler over ours, and popping would take theirs away
    // instead (see SketchTool::Stop() for the same reasoning).
    if (myContext.viewport != nullptr && myContext.viewport->CurrentInteraction() == this) {
        myContext.viewport->PopInteraction();  // calls OnDeactivated()
    } else {
        OnDeactivated();
    }
}

void MoveGizmoTool::OnDeactivated()
{
    if (myIsDragging) {
        EndDrag(false);  // don't leave a half-applied drag hanging
    }
    Detach();
    if (myContext.document != nullptr) {
        myContext.document->RemoveObserver(this);
    }
    myIsAttached = false;
    ShowStatus(myContext, QString());
    myContext.Redraw();
}

void MoveGizmoTool::OnDocumentChanged(Document& theDocument)
{
    (void)theDocument;
    if (!myIsAttached || myIsDragging) {
        return;
    }

    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    const Handle(AIS_Shape) body = FindDisplayedBody(aisContext, myContext.document);
    if (body.IsNull()) {
        // The body vanished from under us (undone away, deleted...): stop
        // rather than sit attached to nothing.
        Stop();
        return;
    }

    AttachTo(body);
    myContext.Redraw();
}

void MoveGizmoTool::AttachTo(const Handle(AIS_InteractiveObject)& theObject)
{
    if (theObject.IsNull()) {
        return;
    }

    if (myManipulator.IsNull()) {
        myManipulator = new AIS_Manipulator();
        // Scaling lives in its own tool (RotateScaleGizmoTool), not here.
        // TransformFeature does model a scale factor now, but a factor of
        // zero or below has to be REFUSED, and AIS_Manipulator has
        // already transformed the object it is attached to by the time
        // its Transform() hands the number back -- there is nothing left
        // to refuse. Hide the handle rather than offer an edit that can
        // only be caught after the damage is on screen.
        myManipulator->SetPart(AIS_MM_Scaling, Standard_False);
        // Lets a single press-drag grab a handle immediately, Fusion
        // style, instead of needing a separate "select the arrow" click
        // before a drag can start.
        myManipulator->SetModeActivationOnDetection(Standard_True);
    } else {
        myManipulator->Detach();
    }

    AIS_Manipulator::OptionsForAttach options;
    options.SetAdjustPosition(Standard_True).SetAdjustSize(Standard_True).SetEnableModes(Standard_True);
    myManipulator->Attach(theObject, options);

    // Resolved here rather than at commit time: by then our own drag has
    // already moved the body, so its centroid no longer describes the
    // shape the feature will be computed against.
    myTargetRef = BodyRefFor(theObject, myContext.document);

    // Force world-aligned handles regardless of how Attach() oriented
    // itself: CommitTransform() below reads back which axis index (0/1/2)
    // produced a drag and assumes those are world X/Y/Z.
    myManipulator->SetPosition(gp_Ax2(myManipulator->Position().Location(), gp::DZ(), gp::DX()));

    myManipulator->EnableMode(AIS_MM_Translation);
    myManipulator->EnableMode(AIS_MM_Rotation);
}

void MoveGizmoTool::Detach()
{
    if (!myManipulator.IsNull()) {
        myManipulator->Detach();  // removes itself from the AIS context too
        myManipulator.Nullify();
    }
}

bool MoveGizmoTool::OnMousePress(const Graphic3d_Vec2i& thePos,
                                 Qt::MouseButton        theButton,
                                 Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (!myIsAttached || theButton != Qt::LeftButton) {
        return false;
    }

    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    const Handle(V3d_View) view = myContext.View();
    if (aisContext.IsNull() || view.IsNull() || myManipulator.IsNull()) {
        return false;
    }

    // Make sure detection is current even without a preceding hover move
    // (e.g. the very first click right after the tool activates).
    aisContext->MoveTo(thePos.x(), thePos.y(), view, Standard_False);
    if (!myManipulator->HasActiveMode()) {
        return false;  // missed every handle -- let normal select/orbit run
    }

    myDragMode  = myManipulator->ActiveMode();
    myDragAxis  = myManipulator->ActiveAxisIndex();
    myDragPivot = myManipulator->Position().Location();

    myManipulator->StartTransform(thePos.x(), thePos.y(), view);
    myIsDragging   = true;
    myDidTransform = false;
    myLastTrsf     = gp_Trsf();
    return true;
}

bool MoveGizmoTool::OnMouseMove(const Graphic3d_Vec2i& thePos,
                                Qt::MouseButtons       theButtons,
                                Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (!myIsAttached || !myIsDragging) {
        return false;  // let ambient hover-highlight and orbit/pan run
    }
    if (!theButtons.testFlag(Qt::LeftButton)) {
        // The button came up somewhere that never reached
        // OnMouseRelease (e.g. released outside the window); don't leave
        // the drag stuck open.
        EndDrag(true);
        return true;
    }

    const Handle(V3d_View) view = myContext.View();
    if (!view.IsNull() && !myManipulator.IsNull()) {
        myLastTrsf = myManipulator->Transform(thePos.x(), thePos.y(), view);
        myDidTransform = true;
    }
    return true;
}

bool MoveGizmoTool::OnMouseRelease(const Graphic3d_Vec2i& thePos,
                                   Qt::MouseButton        theButton,
                                   Qt::KeyboardModifiers  theModifiers)
{
    (void)thePos;
    (void)theModifiers;
    if (!myIsAttached || theButton != Qt::LeftButton || !myIsDragging) {
        return false;
    }
    EndDrag(true);
    return true;
}

bool MoveGizmoTool::OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers)
{
    (void)theModifiers;
    if (!myIsAttached || theKey != Qt::Key_Escape) {
        return false;
    }
    if (myIsDragging) {
        EndDrag(false);  // cancel the in-progress drag, stay attached
        return true;
    }
    Stop();  // no drag in progress: Escape detaches the gizmo entirely
    return true;
}

void MoveGizmoTool::EndDrag(bool theApply)
{
    if (!myIsDragging) {
        return;
    }
    myIsDragging = false;

    if (!myManipulator.IsNull()) {
        myManipulator->StopTransform(theApply ? Standard_True : Standard_False);
    }

    if (theApply && myDidTransform) {
        CommitTransform(myLastTrsf);
    }

    myDidTransform = false;
    myLastTrsf = gp_Trsf();
    myDragMode = AIS_MM_None;
    myDragAxis = -1;
}

void MoveGizmoTool::CommitTransform(const gp_Trsf& theTrsf)
{
    if (myContext.document == nullptr || myDragAxis < 0 || myDragAxis > 2) {
        return;
    }

    double tx = 0.0, ty = 0.0, tz = 0.0;
    double rx = 0.0, ry = 0.0, rz = 0.0;

    try {
        if (myDragMode == AIS_MM_Translation) {
            const gp_XYZ delta = theTrsf.TranslationPart();
            tx = delta.X();
            ty = delta.Y();
            tz = delta.Z();
        } else if (myDragMode == AIS_MM_Rotation) {
            gp_XYZ axis;
            Standard_Real angle = 0.0;
            if (theTrsf.GetRotation(axis, angle)) {
                // GetRotation()'s axis can point either way along the
                // ring's true axis; fold that into the angle's sign so the
                // stored value always means "about +X/+Y/+Z", which is
                // what Parameters() promises the properties panel.
                const double signedAngle =
                    (axis.Coord(myDragAxis + 1) < 0.0) ? -angle : angle;
                const double degrees = signedAngle * kRadToDeg;
                if (myDragAxis == 0)      rx = degrees;
                else if (myDragAxis == 1) ry = degrees;
                else                      rz = degrees;
            }
        } else {
            return;  // scaling is disabled; nothing else should reach here
        }
    } catch (const Standard_Failure&) {
        return;
    }

    if (tx == 0.0 && ty == 0.0 && tz == 0.0 && rx == 0.0 && ry == 0.0 && rz == 0.0) {
        return;  // a click that never actually moved the handle
    }

    auto feature = std::make_shared<TransformFeature>(tx, ty, tz, rx, ry, rz, myDragPivot);
    feature->SetTarget(myTargetRef);  // a null ref still means "everything"
    feature->SetName(myContext.document->MakeUniqueName("Move"));
    myContext.document->AddFeature(feature);  // rebuilds, snapshots undo, redisplays
    // OnDocumentChanged(), fired synchronously by AddFeature()'s rebuild,
    // re-attaches us to the new AIS_Shape MainWindow just displayed.
}

} // namespace lcad
