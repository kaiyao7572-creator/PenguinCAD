#include "gizmos/MoveGizmoTool.h"
#include "gizmos/TransformFeature.h"

#include "OcctViewport.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <AIS_Shape.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_ListOfInteger.hxx>
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

// MainWindow's whole-document shape is the only AIS_Shape ever displayed
// with selection mode 0 active (see MainWindow::redisplayDocument);
// sketch and preview geometry (see SketchDisplay) is always shown
// non-selectable. That's the one observable difference available to us
// for re-finding "the body" after a rebuild swaps its AIS_Shape out from
// under the gizmo.
Handle(AIS_Shape) FindDisplayedBody(const Handle(AIS_InteractiveContext)& theContext)
{
    Handle(AIS_Shape) result;
    if (theContext.IsNull()) {
        return result;
    }

    AIS_ListOfInteractive displayed;
    theContext->DisplayedObjects(displayed);
    for (AIS_ListOfInteractive::Iterator it(displayed); it.More(); it.Next()) {
        Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(it.Value());
        if (shape.IsNull()) {
            continue;
        }

        TColStd_ListOfInteger modes;
        theContext->ActivatedModes(shape, modes);
        for (TColStd_ListOfInteger::Iterator modeIt(modes); modeIt.More(); modeIt.Next()) {
            if (modeIt.Value() == 0) {
                return shape;
            }
        }
    }
    return result;
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
    const Handle(AIS_Shape) body = FindDisplayedBody(aisContext);
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
        // TransformFeature only models translate + rotate, so a scale
        // handle would offer an edit we can't actually carry to the
        // timeline -- hide it entirely rather than let it silently do
        // nothing when dragged.
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
    feature->SetName(myContext.document->MakeUniqueName("Move"));
    myContext.document->AddFeature(feature);  // rebuilds, snapshots undo, redisplays
    // OnDocumentChanged(), fired synchronously by AddFeature()'s rebuild,
    // re-attaches us to the new AIS_Shape MainWindow just displayed.
}

} // namespace lcad
