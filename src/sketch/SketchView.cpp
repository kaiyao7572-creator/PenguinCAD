#include "sketch/SketchView.h"

#include "core/Command.h"
#include "sketch/SketchDisplay.h"

#include <AIS_AnimationCamera.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <AIS_Shape.hxx>
#include <Graphic3d_Camera.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <V3d_Viewer.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <memory>

#include <QObject>
#include <QTimer>

namespace lcad {

namespace {

// How far back the rest of the model fades while a sketch is open. Enough
// that sketch curves read first, not so much that the body it is being
// drawn on stops being a usable reference.
constexpr Standard_Real kDimTransparency = 0.78;

// Seconds the "look at the plane" swing takes. Long enough to read as a
// move rather than a jump cut, short enough not to be in the way.
constexpr Standard_Real kLookAtSeconds = 0.4;

// Frame interval and the tick budget that backstops it. The budget exists
// because the animation is driven by a plain timer rather than by a render
// loop: if a frame is ever missed the camera must still end up exactly on
// the plane, so the last tick snaps it there.
constexpr int kFrameMilliseconds = 16;
constexpr int kMaxFrames = static_cast<int>(kLookAtSeconds * 1000.0) / kFrameMilliseconds + 8;

// The app has no render loop of its own -- OCCT only redraws in response
// to an event -- so a camera animation needs something to pump it. One
// timer serves every look-at; starting a new one cancels whatever swing
// was still in flight.
//
// Deliberately leaked, like SketchDisplay: it outlives static destruction
// order, which runs after the Qt/OCCT viewer is already gone.
QTimer& AnimationTimer()
{
    static QTimer* theTimer = new QTimer();
    return *theTimer;
}

// Everything the user would call "the model": shaded solids. Sketch
// curves, the origin axes, the view cube and the annotation labels all
// fall out here, which is what keeps the fade from swallowing the sketch
// it is supposed to be showing off.
bool IsModelObject(const Handle(AIS_InteractiveObject)& theObject)
{
    const Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(theObject);
    if (shape.IsNull() || shape->Shape().IsNull()) {
        return false;
    }
    if (SketchDisplay::Instance().OwnsObject(theObject)) {
        return false;
    }
    return TopExp_Explorer(shape->Shape(), TopAbs_FACE).More();
}

} // namespace

SketchView& SketchView::Instance()
{
    // Leaked for the same reason SketchDisplay is: the AIS handles below
    // must not be released during static destruction.
    static SketchView* theInstance = new SketchView();
    return *theInstance;
}

void SketchView::Enter(const CommandContext& theContext, const gp_Ax3& theSketchPlane)
{
    myAisContext = theContext.AisContext();
    myView = theContext.View();
    myIsActive = true;

    LookAt(theSketchPlane);
    Realign(theSketchPlane);
    RefreshDimming();
}

void SketchView::Realign(const gp_Ax3& theSketchPlane)
{
    if (!myIsActive || myView.IsNull() || myView->Viewer().IsNull()) {
        return;
    }
    // The construction grid IS the viewer's privileged plane, so moving
    // that plane is what makes the grid lie under the sketch instead of
    // under the world.
    myView->Viewer()->SetPrivilegedPlane(theSketchPlane);
}

void SketchView::Leave(const CommandContext& theContext)
{
    if (myAisContext.IsNull()) {
        myAisContext = theContext.AisContext();
    }
    if (myView.IsNull()) {
        myView = theContext.View();
    }

    myIsActive = false;
    Undim();

    // Back to the Z-up world grid OcctViewport set up at startup. The
    // camera is deliberately left where it is: the user has usually just
    // spent a while framing the sketch, and yanking the view somewhere
    // else on Finish would throw that away. Orbit is free again the
    // moment the sketch tools let go of the mouse.
    if (!myView.IsNull() && !myView->Viewer().IsNull()) {
        myView->Viewer()->SetPrivilegedPlane(
            gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0), gp_Dir(1.0, 0.0, 0.0)));
    }
    if (!myView.IsNull()) {
        myView->Redraw();
    }
}

void SketchView::RefreshDimming()
{
    if (!myIsActive || myAisContext.IsNull()) {
        return;
    }

    // Recorded transparencies belong to AIS objects that a rebuild may
    // already have thrown away, so the record starts again from what is
    // on screen right now rather than being patched up.
    Undim();

    AIS_ListOfInteractive objects;
    myAisContext->DisplayedObjects(objects);
    for (AIS_ListOfInteractive::Iterator it(objects); it.More(); it.Next()) {
        const Handle(AIS_InteractiveObject)& object = it.Value();
        if (object.IsNull() || !IsModelObject(object)) {
            continue;
        }
        Dimmed entry;
        entry.object = object;
        entry.transparency = object->Transparency();
        if (entry.transparency >= kDimTransparency) {
            continue;  // already at least this faint; leave it alone
        }
        myDimmed.push_back(entry);
        myAisContext->SetTransparency(object, kDimTransparency, Standard_False);
    }
}

void SketchView::Undim()
{
    if (myAisContext.IsNull()) {
        myDimmed.clear();
        return;
    }

    for (const Dimmed& entry : myDimmed) {
        if (entry.object.IsNull() || !myAisContext->IsDisplayed(entry.object)) {
            continue;
        }
        if (entry.transparency <= 0.0) {
            myAisContext->UnsetTransparency(entry.object, Standard_False);
        } else {
            myAisContext->SetTransparency(entry.object, entry.transparency, Standard_False);
        }
    }
    myDimmed.clear();
}

void SketchView::LookAt(const gp_Ax3& thePlane)
{
    if (myView.IsNull()) {
        return;
    }
    const Handle(Graphic3d_Camera) camera = myView->Camera();
    if (camera.IsNull()) {
        return;
    }

    // Stand off along the plane's own normal with its X axis running right
    // across the screen, which is what makes the sketch read as flat paper.
    // The standoff keeps the current distance so the swing changes the
    // angle only -- zoom is the user's business.
    const gp_Dir normal = thePlane.Direction();
    const Standard_Real distance = camera->Distance() > 0.0 ? camera->Distance() : 500.0;

    Handle(Graphic3d_Camera) target = new Graphic3d_Camera(camera);
    target->SetEyeAndCenter(thePlane.Location().Translated(gp_Vec(normal) * distance),
                            thePlane.Location());
    target->SetUp(thePlane.YDirection());

    Handle(Graphic3d_Camera) start = new Graphic3d_Camera(camera);

    Handle(AIS_AnimationCamera) animation = new AIS_AnimationCamera("SketchLookAt", myView);
    animation->SetCameraStart(start);
    animation->SetCameraEnd(target);
    animation->SetOwnDuration(kLookAtSeconds);
    animation->StartTimer(0.0, 1.0, Standard_True, Standard_False);

    QTimer& timer = AnimationTimer();
    // A swing already in flight would otherwise keep driving the camera
    // towards the previous plane.
    QObject::disconnect(&timer, nullptr, nullptr, nullptr);

    const Handle(V3d_View) view = myView;
    auto frames = std::make_shared<int>(0);
    QObject::connect(&timer, &QTimer::timeout, [animation, target, view, frames]() {
        QTimer& self = AnimationTimer();
        ++(*frames);

        const bool expired = *frames >= kMaxFrames;
        if (!expired && !animation->IsStopped()) {
            animation->UpdateTimer();
        } else {
            // However the animation ended, the camera has to land exactly
            // on the plane -- an almost-aligned sketch view is worse than
            // no animation at all.
            animation->Stop();
            view->Camera()->Copy(target);
            self.stop();
        }

        view->Invalidate();
        view->Redraw();
    });
    timer.start(kFrameMilliseconds);
}

} // namespace lcad
