#include "gizmos/MoveGizmoTool.h"
#include "gizmos/TransformFeature.h"

#include "core/Body.h"
#include "core/Units.h"

#include "OcctViewport.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <AIS_Shape.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Standard_Failure.hxx>
#include <V3d_View.hxx>
#include <gp.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_XYZ.hxx>

#include <memory>
#include <vector>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;

// AIS_Manipulator's size, in LOGICAL pixels, drawn zoom-persistent. Its
// arrows come out at about 0.8 of this -- a hundred-odd pixels, which is
// what Fusion's move arrows measure -- and its rings just outside them.
// Sized off the body instead (AdjustSize), a 20mm box got a gizmo ten
// pixels across at the default zoom, buried inside the box it was
// supposed to be moving.
constexpr double kManipulatorPixels = 125.0;

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

// What one drag of the manipulator asked for, as the numbers a
// TransformFeature stores. ONE function for the live readout and the
// commit, so the figure the status bar shows while dragging is the figure
// that lands on the timeline. False for a mode this tool never enables.
bool DecomposeDrag(const gp_Trsf& theTrsf, AIS_ManipulatorMode theMode, int theAxis,
                   double& theTx, double& theTy, double& theTz,
                   double& theRx, double& theRy, double& theRz)
{
    theTx = theTy = theTz = theRx = theRy = theRz = 0.0;
    if (theAxis < 0 || theAxis > 2) {
        return false;
    }
    try {
        // The planar squares between two arrows drag in their plane: a
        // translation all the same, and the manipulator has already moved
        // the body on screen by the time we get here, so refusing it would
        // leave the picture saying one thing and the model another.
        if (theMode == AIS_MM_Translation || theMode == AIS_MM_TranslationPlane) {
            const gp_XYZ delta = theTrsf.TranslationPart();
            theTx = delta.X();
            theTy = delta.Y();
            theTz = delta.Z();
            return true;
        }
        if (theMode == AIS_MM_Rotation) {
            gp_XYZ axis;
            Standard_Real angle = 0.0;
            if (theTrsf.GetRotation(axis, angle)) {
                // GetRotation()'s axis can point either way along the
                // ring's true axis; fold that into the angle's sign so the
                // stored value always means "about +X/+Y/+Z", which is
                // what Parameters() promises the properties panel.
                const double signedAngle = (axis.Coord(theAxis + 1) < 0.0) ? -angle : angle;
                const double degrees = signedAngle * kRadToDeg;
                if (theAxis == 0)      theRx = degrees;
                else if (theAxis == 1) theRy = degrees;
                else                   theRz = degrees;
            }
            return true;
        }
    } catch (const Standard_Failure&) {
        return false;
    }
    return false;  // scaling is disabled; nothing else should reach here
}

// The live readout under a drag, Fusion's distance/angle box as this
// app's status bar shows it (RotateScaleGizmoTool does the same).
QString DragReadout(AIS_ManipulatorMode theMode, int theAxis,
                    double theTx, double theTy, double theTz,
                    double theRx, double theRy, double theRz)
{
    static const char* const kAxisNames[3] = {"X", "Y", "Z"};
    const auto length = [](double theValue) {
        return QString::fromStdString(FormatValue(theValue, UnitKind::Length));
    };
    if (theMode == AIS_MM_Rotation) {
        const double degrees = theAxis == 0 ? theRx : theAxis == 1 ? theRy : theRz;
        return QString("Rotate %1  %2")
            .arg(kAxisNames[theAxis])
            .arg(QString::fromStdString(FormatValue(degrees, UnitKind::Angle)));
    }
    if (theMode == AIS_MM_Translation) {
        const double distance = theAxis == 0 ? theTx : theAxis == 1 ? theTy : theTz;
        return QString("Move %1  %2").arg(kAxisNames[theAxis]).arg(length(distance));
    }
    return QString("Move  X %1  Y %2  Z %3").arg(length(theTx), length(theTy), length(theTz));
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

// The displayed AIS object for the body theRef names, resolved by the
// same volume-and-centre rule the committed TransformFeature resolves its
// target by -- so the gizmo sits on exactly the body the next drag will
// move. Null when nothing matches, or two things match equally well.
Handle(AIS_Shape) FindDisplayedBodyFor(const Handle(AIS_InteractiveContext)& theContext,
                                       const Document*                       theDocument,
                                       const CombineBodyRef&                 theRef)
{
    Handle(AIS_Shape) result;
    if (theContext.IsNull() || theDocument == nullptr || theRef.IsNull()) {
        return result;
    }

    std::vector<TopoDS_Shape> shapes;
    for (const BodyPtr& body : theDocument->Bodies()) {
        shapes.push_back(body ? body->Shape() : TopoDS_Shape());
    }
    std::size_t index = 0;
    if (FindBodyForRef(shapes, theRef, index) != BodyMatch::Found || shapes[index].IsNull()) {
        return result;
    }

    AIS_ListOfInteractive displayed;
    theContext->DisplayedObjects(displayed);
    for (AIS_ListOfInteractive::Iterator it(displayed); it.More(); it.Next()) {
        Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(it.Value());
        if (!shape.IsNull() && shape->Shape().IsSame(shapes[index])) {
            return shape;
        }
    }
    return result;
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
    // Follow the body that was being moved, not the first body on screen:
    // with two bodies, re-attaching to "any body" jumped the gizmo onto the
    // OTHER one the moment a drag committed, and the next drag moved that.
    // Our own commit is followed to where the drag put the body; any other
    // rebuild must leave it where it was, and if it has gone (undone away,
    // deleted, edited out of recognition) the gizmo detaches rather than
    // guess. A body with no volume has no signature to follow, so it falls
    // back to the old rule -- its move acts on the whole model anyway.
    Handle(AIS_Shape) body;
    if (!myTargetRef.IsNull()) {
        body = FindDisplayedBodyFor(aisContext, myContext.document,
                                    myIsCommitting ? myFollowRef : myTargetRef);
    } else {
        body = FindDisplayedBody(aisContext, myContext.document);
    }
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
        // A fixed size on SCREEN, as Fusion's is: in zoom-persistent mode
        // one unit of Size() is one framebuffer pixel at every zoom, so
        // the logical size is scaled by the device ratio here -- handed
        // the logical number, a 2x display would halve the gizmo.
        myManipulator->SetZoomPersistence(Standard_True);
        myManipulator->SetSize(
            static_cast<Standard_ShortReal>(kManipulatorPixels * DevicePixelRatio(myContext)));
        // Topmost has a depth buffer of its own, so the gizmo draws OVER
        // the body it sits in the middle of. In the default layer the body
        // hid all but a few pixels of it, and a zoom-persistent gizmo is
        // wholly inside any body zoomed up bigger than it. It also makes
        // the handles win the pick against the body's own faces.
        myManipulator->SetZLayer(Graphic3d_ZLayerId_Topmost);
    } else {
        myManipulator->Detach();
    }

    // Position only: AdjustSize would size it off the body's bounding box,
    // in model units, undoing the fixed screen size above.
    AIS_Manipulator::OptionsForAttach options;
    options.SetAdjustPosition(Standard_True).SetAdjustSize(Standard_False).SetEnableModes(Standard_True);
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
    if (myManipulator.IsNull()) {
        return;
    }
    // Removed through OUR context, not left to Detach(). Detach() finds
    // the context through the object it is attached to, and after an undo
    // or a delete MainWindow has already removed that body's AIS_Shape --
    // which clears its context -- so Detach() quietly removed nothing and
    // the arrows stayed on screen, orphaned, where the body used to be,
    // still lighting up under the cursor with no tool behind them.
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (!aisContext.IsNull() && myManipulator->HasInteractiveContext()) {
        aisContext->Remove(myManipulator, Standard_False);
    }
    myManipulator->Detach();
    myManipulator.Nullify();
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

        double tx = 0.0, ty = 0.0, tz = 0.0, rx = 0.0, ry = 0.0, rz = 0.0;
        if (DecomposeDrag(myLastTrsf, myDragMode, myDragAxis, tx, ty, tz, rx, ry, rz)) {
            ShowStatus(myContext, DragReadout(myDragMode, myDragAxis, tx, ty, tz, rx, ry, rz));
        }
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

    if (!DecomposeDrag(theTrsf, myDragMode, myDragAxis, tx, ty, tz, rx, ry, rz)) {
        return;
    }
    if (tx == 0.0 && ty == 0.0 && tz == 0.0 && rx == 0.0 && ry == 0.0 && rz == 0.0) {
        return;  // a click that never actually moved the handle
    }

    auto feature = std::make_shared<TransformFeature>(tx, ty, tz, rx, ry, rz, myDragPivot);
    feature->SetTarget(myTargetRef);  // a null ref still means "everything"
    feature->SetName(myContext.document->MakeUniqueName("Move"));

    // Where the body will be once the feature has run: the same signature,
    // carried by the feature's own transform, so the gizmo can find it
    // again among bodies the rebuild has just renumbered.
    myFollowRef = myTargetRef;
    myFollowRef.centre = myFollowRef.centre.Transformed(feature->Trsf());

    // OnDocumentChanged(), fired synchronously by AddFeature()'s rebuild,
    // re-attaches us to the new AIS_Shape MainWindow just displayed.
    myIsCommitting = true;
    myContext.document->AddFeature(feature);  // rebuilds, snapshots undo, redisplays
    myIsCommitting = false;
}

} // namespace lcad
