#include "sketch/SketchPlanePicker.h"

#include "OcctViewport.h"
#include "sketch/SketchDisplay.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchSelection.h"
#include "sketch/SketchTools.h"

#include <AIS_DisplayMode.hxx>
#include <AIS_ListOfInteractive.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Prs3d_Drawer.hxx>
#include <Quantity_Color.hxx>
#include <Standard_Failure.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TColStd_ListIteratorOfListOfInteger.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <V3d_View.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <utility>

#include <QCursor>
#include <QMainWindow>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

// Half-width of the origin rectangles, in mm. Big enough to be an easy
// click target on an empty document, small enough not to bury a part that
// is already there.
constexpr Standard_Real kPlaneHalfExtent = 60.0;

// Translucent enough to see the model through, opaque enough to read as a
// surface rather than an outline.
constexpr Standard_Real kPlaneTransparency = 0.7;
constexpr Standard_Real kHoverTransparency = 0.35;

// Each origin plane is tinted after the axes it contains, which is how
// Fusion's browser colours them and the fastest way to tell three
// identical rectangles apart mid-click.
const Quantity_Color kPlaneXYColor(0.86, 0.45, 0.42, Quantity_TOC_RGB);
const Quantity_Color kPlaneXZColor(0.48, 0.80, 0.48, Quantity_TOC_RGB);
const Quantity_Color kPlaneYZColor(0.42, 0.58, 0.95, Quantity_TOC_RGB);

// One colour for whatever is under the cursor, so "this is the one you are
// about to pick" never has to be inferred from a shade of the base tint.
const Quantity_Color kHoverColor(0.55, 0.92, 1.00, Quantity_TOC_RGB);

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

// Highlighting a translucent plane by painting it solid would hide the
// model behind it, so the hover style keeps the plane translucent -- just
// brighter and a good deal more opaque than its resting state.
//
// Display mode and Z layer have to be set explicitly: a fresh drawer
// defaults to mode 0 (wireframe) and the default layer, so a highlighted
// plane would otherwise drop to an outline and change depth the instant
// the cursor touched it.
Handle(Prs3d_Drawer) HoverStyle()
{
    Handle(Prs3d_Drawer) drawer = new Prs3d_Drawer();
    drawer->SetColor(kHoverColor);
    drawer->SetTransparency(static_cast<Standard_ShortReal>(kHoverTransparency));
    drawer->SetDisplayMode(AIS_Shaded);
    drawer->SetZLayer(Graphic3d_ZLayerId_UNKNOWN);
    return drawer;
}

// Anything the user could sensibly sketch on top of: a displayed solid.
// Sketch curves and the picker's own rectangles are excluded by the
// caller, which knows which objects those are.
bool HasFaces(const Handle(AIS_InteractiveObject)& theObject)
{
    const Handle(AIS_Shape) shape = Handle(AIS_Shape)::DownCast(theObject);
    if (shape.IsNull() || shape->Shape().IsNull()) {
        return false;
    }
    return TopExp_Explorer(shape->Shape(), TopAbs_FACE).More();
}

} // namespace

bool SketchPlaneFromFace(const TopoDS_Shape& theShape, gp_Ax3& theResult)
{
    if (theShape.IsNull() || theShape.ShapeType() != TopAbs_FACE) {
        return false;
    }

    const TopoDS_Face face = TopoDS::Face(theShape);
    Handle(Geom_Plane) plane = Handle(Geom_Plane)::DownCast(BRep_Tool::Surface(face));
    if (plane.IsNull()) {
        return false;
    }

    const gp_Pln pln = plane->Pln();
    gp_Dir normal = pln.Axis().Direction();
    // A REVERSED face has its outward normal opposite the underlying
    // surface's, and outward is the direction an extrude off this face
    // should default to.
    if (face.Orientation() == TopAbs_REVERSED) {
        normal.Reverse();
    }
    theResult = gp_Ax3(pln.Location(), normal, pln.XAxis().Direction());
    return true;
}

SketchPlanePicker& SketchPlanePicker::Instance()
{
    // Leaked on purpose, like the sketch tools: the viewport holds a bare
    // pointer to whichever handler is active and does not own it, and the
    // AIS handles must outlive static destruction.
    static SketchPlanePicker* theInstance = new SketchPlanePicker();
    return *theInstance;
}

void SketchPlanePicker::Start(const CommandContext& theContext, Handler theOnPicked)
{
    Stop();

    // A half-drawn curve from the sketch that was open would keep eating
    // the clicks meant for the plane.
    StopActiveSketchTool();

    myContext = theContext;
    myOnPicked = std::move(theOnPicked);
    myPressConsumed = false;

    if (myContext.AisContext().IsNull() || myContext.View().IsNull()
        || myContext.viewport == nullptr) {
        ShowStatus(myContext, "The viewport isn't ready yet.");
        myOnPicked = nullptr;
        return;
    }

    myIsRunning = true;
    ShowPlanes();
    ActivateModelFaces();

    myContext.viewport->PushInteraction(this);
    if (OcctNativeWindow* window = myContext.viewport->NativeWindow()) {
        window->setCursor(QCursor(Qt::PointingHandCursor));
    }

    ShowStatus(myContext, "Select a plane or a planar face to sketch on - Esc cancels.");
    myContext.Redraw();
}

void SketchPlanePicker::Stop()
{
    if (!myIsRunning) {
        return;
    }
    myIsRunning = false;

    // Only pop when we're still on top: another subsystem may have pushed
    // its own handler over ours, and popping would take away theirs.
    if (myContext.viewport != nullptr && myContext.viewport->CurrentInteraction() == this) {
        myContext.viewport->PopInteraction();  // calls OnDeactivated()
    } else {
        OnDeactivated();
    }
}

void SketchPlanePicker::OnDeactivated()
{
    myIsRunning = false;
    myPressConsumed = false;
    myOnPicked = nullptr;

    // Both exits land here -- picked and cancelled alike -- which is what
    // guarantees the three rectangles never outlive the pick.
    HidePlanes();
    RestoreModelSelection();

    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (!aisContext.IsNull()) {
        // The last hovered plane is gone; its highlight must not outlive it.
        aisContext->ClearDetected(Standard_False);
    }

    if (myContext.viewport != nullptr) {
        if (OcctNativeWindow* window = myContext.viewport->NativeWindow()) {
            window->unsetCursor();
        }
    }
    myContext.Redraw();

    // Cancelling out of a pick that was started from inside an open sketch
    // has to land back in that sketch, not in nothing -- otherwise clicks
    // stop picking sketch geometry with no visible reason why.
    if (SketchSession::Instance().IsActive()) {
        SketchSelectTool().Start(myContext);
    }
}

void SketchPlanePicker::ShowPlanes()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull()) {
        return;
    }

    struct Origin
    {
        gp_Ax3                position;
        const Quantity_Color* color;
    };
    const Origin origins[] = {{SketchFeature::PlaneXY(), &kPlaneXYColor},
                              {SketchFeature::PlaneXZ(), &kPlaneXZColor},
                              {SketchFeature::PlaneYZ(), &kPlaneYZColor}};

    for (const Origin& origin : origins) {
        TopoDS_Shape face;
        try {
            face = BRepBuilderAPI_MakeFace(gp_Pln(origin.position),
                                           -kPlaneHalfExtent, kPlaneHalfExtent,
                                           -kPlaneHalfExtent, kPlaneHalfExtent);
        } catch (const Standard_Failure&) {
            continue;  // a plane we can't build simply isn't offered
        }
        if (face.IsNull()) {
            continue;
        }

        Handle(AIS_Shape) object = new AIS_Shape(face);
        object->SetColor(*origin.color);
        object->SetTransparency(kPlaneTransparency);
        object->SetDynamicHilightAttributes(HoverStyle());

        // Selection mode 0 -- the whole rectangle -- so a click anywhere
        // on it counts, and so MoveTo highlights it on hover.
        aisContext->Display(object, AIS_Shaded, 0, Standard_False);

        Candidate candidate;
        candidate.object = object;
        candidate.position = origin.position;
        myPlanes.push_back(candidate);
    }
}

void SketchPlanePicker::HidePlanes()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    for (const Candidate& candidate : myPlanes) {
        if (!aisContext.IsNull() && !candidate.object.IsNull()) {
            aisContext->Remove(candidate.object, Standard_False);
        }
    }
    myPlanes.clear();
}

void SketchPlanePicker::ActivateModelFaces()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (aisContext.IsNull()) {
        return;
    }

    AIS_ListOfInteractive objects;
    aisContext->DisplayedObjects(objects);
    for (AIS_ListOfInteractive::Iterator it(objects); it.More(); it.Next()) {
        const Handle(AIS_InteractiveObject)& object = it.Value();
        if (object.IsNull() || !HasFaces(object)
            || SketchDisplay::Instance().OwnsObject(object)) {
            continue;
        }
        bool isPicker = false;
        for (const Candidate& candidate : myPlanes) {
            if (candidate.object == object) {
                isPicker = true;
                break;
            }
        }
        if (isPicker) {
            continue;
        }

        // Remembered rather than assumed: the object may have had several
        // modes on, or none, and Finish has to put back exactly that.
        SelectionState state;
        state.object = object;
        aisContext->ActivatedModes(object, state.modes);
        myRestore.push_back(state);

        aisContext->Deactivate(object);
        aisContext->Activate(object, AIS_Shape::SelectionMode(TopAbs_FACE));
    }
}

void SketchPlanePicker::RestoreModelSelection()
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    if (!aisContext.IsNull()) {
        for (const SelectionState& state : myRestore) {
            if (state.object.IsNull() || !aisContext->IsDisplayed(state.object)) {
                continue;
            }
            aisContext->Deactivate(state.object);
            for (TColStd_ListIteratorOfListOfInteger it(state.modes); it.More(); it.Next()) {
                aisContext->Activate(state.object, it.Value());
            }
        }
    }
    myRestore.clear();
}

bool SketchPlanePicker::Resolve(const Graphic3d_Vec2i& thePos, gp_Ax3& theResult)
{
    const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
    const Handle(V3d_View) view = myContext.View();
    if (aisContext.IsNull() || view.IsNull()) {
        return false;
    }

    aisContext->MoveTo(thePos.x(), thePos.y(), view, Standard_False);
    if (!aisContext->HasDetected()) {
        return false;
    }

    const Handle(AIS_InteractiveObject) detected = aisContext->DetectedInteractive();
    for (const Candidate& candidate : myPlanes) {
        if (candidate.object == detected) {
            theResult = candidate.position;
            return true;
        }
    }

    // Face-level picks come back as a BRep owner carrying the sub-shape.
    // Asked for through the owner rather than through the context's
    // DetectedShape(), which OCCT 7.9 deprecates along with local context.
    const Handle(StdSelect_BRepOwner) owner =
        Handle(StdSelect_BRepOwner)::DownCast(aisContext->DetectedOwner());
    if (owner.IsNull() || !owner->HasShape()) {
        return false;
    }
    return SketchPlaneFromFace(owner->Shape(), theResult);
}

bool SketchPlanePicker::OnMousePress(const Graphic3d_Vec2i& thePos,
                                     Qt::MouseButton        theButton,
                                     Qt::KeyboardModifiers  theModifiers)
{
    (void)thePos;
    (void)theModifiers;
    if (!myIsRunning) {
        return false;
    }
    // Middle and right stay with the viewport so pan, orbit (Shift+middle)
    // and the marking menu keep working while the planes are up -- picking
    // the right face usually means spinning the model first.
    if (theButton != Qt::LeftButton) {
        return false;
    }

    // Every left click belongs to the pick, hit or miss: letting a miss
    // through would start a rubber-band selection over the planes.
    //
    // The plane is committed on release, not here, so that this handler is
    // still on top of the interaction stack when the matching release
    // arrives. Committing on press would pop it first and leave the
    // release to whatever sketch tool took over -- a tool that never saw
    // the press and can make no sense of the release.
    myPressConsumed = true;
    return true;
}

bool SketchPlanePicker::OnMouseMove(const Graphic3d_Vec2i& thePos,
                                    Qt::MouseButtons       theButtons,
                                    Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (!myIsRunning) {
        return false;
    }
    // Any held button is a drag the viewport owns -- a pan, an orbit, a
    // marking-menu gesture or the tail of a click.
    if (theButtons != Qt::NoButton) {
        return false;
    }

    gp_Ax3 hovered;
    const bool overPlane = Resolve(thePos, hovered);
    ShowStatus(myContext,
               overPlane ? "Click to sketch on this plane - Esc cancels."
                         : "Select a plane or a planar face to sketch on - Esc cancels.");
    // Consumed: Resolve has already done the MoveTo that drives the
    // highlight, and letting the viewport run its own would pick twice per
    // mouse move for the same answer.
    return true;
}

bool SketchPlanePicker::OnMouseRelease(const Graphic3d_Vec2i& thePos,
                                       Qt::MouseButton        theButton,
                                       Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (theButton != Qt::LeftButton || !myPressConsumed) {
        return false;
    }
    myPressConsumed = false;

    gp_Ax3 picked;
    if (!myIsRunning || !Resolve(thePos, picked)) {
        return true;  // a click on nothing: the planes stay up
    }

    // Taken before the teardown, which clears them, and called after it,
    // so the handler is free to push its own viewport interaction.
    Handler handler = myOnPicked;
    Stop();
    if (handler) {
        handler(picked);
    }
    return true;
}

bool SketchPlanePicker::OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers)
{
    (void)theModifiers;
    if (!myIsRunning || theKey != Qt::Key_Escape) {
        return false;
    }

    const CommandContext context = myContext;
    Stop();
    ShowStatus(context, "Sketch cancelled.");
    return true;
}

} // namespace lcad
