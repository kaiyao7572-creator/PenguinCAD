#include "sketch/ModelProfilePicker.h"

#include "OcctViewport.h"
#include "core/Body.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/GeometrySelection.h"
#include "core/ProfileSelection.h"
#include "core/ViewportInteraction.h"
#include "sketch/ProfilePicking.h"
#include "sketch/SketchDisplay.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchTools.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_ViewCube.hxx>
#include <SelectMgr_EntityOwner.hxx>
#include <Standard_Failure.hxx>
#include <StdSelect_ViewerSelector3d.hxx>
#include <V3d_View.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Vec.hxx>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace lcad {

namespace {

// How close in depth a face has to be to a sketch plane to count as ON
// it, in device pixels of the current zoom. A face nearer than that is
// indistinguishable from the plane on screen, and an edge picked a pixel
// or two off the cursor carries a depth that far out of true on an
// oblique view.
constexpr Standard_Integer kCoplanarPixels = 2;

// Floor for the same tolerance, in millimetres, for a view zoomed so far
// in that a pixel is smaller than float noise on a large model.
constexpr double kCoplanarFloor = 1.0e-4;

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        if (theText.isEmpty()) {
            window->statusBar()->clearMessage();
        } else {
            window->statusBar()->showMessage(theText);
        }
    }
}

class ModelProfilePicker : public ViewportInteraction
{
public:
    static ModelProfilePicker& Instance()
    {
        static ModelProfilePicker theInstance;
        return theInstance;
    }

    void Install(const CommandContext& theContext)
    {
        if (myIsInstalled || theContext.viewport == nullptr) {
            return;
        }
        myContext = theContext;
        myContext.viewport->PushInteraction(this);
        myIsInstalled = true;
    }

    // Always pushed, never a tool: "is the user in the middle of
    // something?" must not be answered "yes, the profile picker".
    bool IsExclusive() const override { return false; }

    bool OnMouseMove(const Graphic3d_Vec2i& thePos,
                     Qt::MouseButtons       theButtons,
                     Qt::KeyboardModifiers  theModifiers) override
    {
        (void)theModifiers;

        // The rest of a click this picker took: the viewport never saw the
        // press, so it must not see a drag start from nowhere either.
        if (myPressConsumed) {
            return theButtons.testFlag(Qt::LeftButton);
        }
        if (theButtons != Qt::NoButton) {
            return false;   // orbit, pan or a box select in progress
        }

        SketchDisplay& display = SketchDisplay::Instance();
        Hit hit;
        const bool found = IsIdle() && RegionAt(thePos, hit);
        const bool changed = display.SetModelHover(found ? hit.sketch : std::string(),
                                                   found ? hit.region : -1);
        if (!found) {
            if (changed) {
                display.Redraw();
            }
            return false;   // the viewport's own hover highlight carries on
        }

        // The region owns the cursor, so nothing behind it -- the face a
        // sketch was drawn on, above all -- may still look primed for the
        // click. Consuming the move keeps the viewport from detecting it
        // again straight after; the viewport redraws for a consumed event.
        const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
        if (!aisContext.IsNull()) {
            aisContext->ClearDetected(Standard_False);
        }
        return true;
    }

    bool OnMousePress(const Graphic3d_Vec2i& thePos,
                      Qt::MouseButton        theButton,
                      Qt::KeyboardModifiers  theModifiers) override
    {
        myPressConsumed = false;
        if (theButton != Qt::LeftButton || !IsIdle()) {
            return false;
        }

        // Fusion adds to a selection with Shift as well as Ctrl.
        const bool additive = theModifiers.testFlag(Qt::ControlModifier)
                           || theModifiers.testFlag(Qt::ShiftModifier);
        ProfileSelection& selection = ProfileSelection::Instance();

        Hit hit;
        if (!RegionAt(thePos, hit)) {
            // A click on a body or on empty space is ordinary selection. A
            // plain one starts the selection over, and in Fusion the picked
            // profiles go with everything else; an additive one keeps them.
            if (!additive && !selection.IsEmpty()) {
                selection.Clear();
                ShowStatus(myContext, QString());
            }
            return false;
        }

        // A reference names entity ids in ONE sketch, so a region of a
        // different sketch starts the picks over even when adding.
        selection.SetSketchName(hit.sketch);
        selection.Toggle(hit.ref, additive);

        if (!additive) {
            // A plain click replaces the whole selection, faces and edges
            // included, exactly as a click on a face would.
            const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();
            if (!aisContext.IsNull()) {
                aisContext->ClearSelected(Standard_False);
            }
            GeometrySelection::Instance().Clear();
        }

        ShowStatus(myContext, QString::fromStdString(DescribeProfiles(PickedAreas())));
        myPressConsumed = true;
        return true;
    }

    bool OnMouseRelease(const Graphic3d_Vec2i& thePos,
                        Qt::MouseButton        theButton,
                        Qt::KeyboardModifiers  theModifiers) override
    {
        (void)thePos;
        (void)theModifiers;
        // The viewport's controller never saw the press, so it must not
        // see the release: a release with no press leaves it in a state it
        // cannot make sense of.
        if (myPressConsumed && theButton == Qt::LeftButton) {
            myPressConsumed = false;
            return true;
        }
        return false;
    }

    bool OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        // Escape clears the selection in the model view as it does inside a
        // sketch. Never consumed: anything else that clears on Escape
        // still hears it.
        if (theKey == Qt::Key_Escape && IsIdle() && !ProfileSelection::Instance().IsEmpty()) {
            ProfileSelection::Instance().Clear();
            ShowStatus(myContext, QString());
        }
        return false;
    }

private:
    ModelProfilePicker() = default;

    struct Hit
    {
        std::string sketch;
        int         region = -1;
        ProfileRef  ref;
    };

    // Nothing to do while a tool owns the viewport -- a sketch tool, the
    // plane picker, Measure, a gizmo -- and nothing while a sketch is open,
    // where SketchSelectTool picks regions of the open sketch itself. The
    // tool asked first and declined the event; that is not an invitation
    // for a region behind it to take the click.
    bool IsIdle() const
    {
        if (!myIsInstalled || myContext.viewport == nullptr || myContext.document == nullptr) {
            return false;
        }
        if (myContext.viewport->ExclusiveInteraction() != nullptr
            || SketchSession::Instance().IsActive()) {
            return false;
        }
        return !myContext.AisContext().IsNull() && !myContext.View().IsNull();
    }

    // The region a click at thePos would pick, if one wins the cursor.
    bool RegionAt(const Graphic3d_Vec2i& thePos, Hit& theHit)
    {
        SketchDisplay& display = SketchDisplay::Instance();
        if (!myIsDisplayAttached) {
            // The display only attaches itself when a sketch command runs,
            // and it is what holds the regions this asks about.
            display.Attach(myContext);
            myIsDisplayAttached = true;
        }
        if (!display.AreSketchesVisible()) {
            return false;
        }

        Document& document = *myContext.document;
        std::vector<const SketchFeature*> sketches;
        std::vector<ProfilePickTarget>    targets;
        const std::size_t limit = std::min(document.RollbackIndex(), document.FeatureCount());
        for (std::size_t i = 0; i < limit; ++i) {
            const SketchFeature* sketch =
                dynamic_cast<const SketchFeature*>(document.Features()[i].get());
            // Rolled back or suppressed, a sketch is nothing a feature
            // after the marker could build on.
            if (sketch == nullptr || !sketch->IsVisible() || sketch->IsSuppressed()) {
                continue;
            }
            const std::vector<ProfileRegion>& regions = display.RegionsOf(*sketch);
            if (regions.empty()) {
                continue;
            }
            sketches.push_back(sketch);
            targets.push_back(ProfilePickTarget{sketch->Position(), &regions});
        }
        if (targets.empty()) {
            return false;
        }

        const Handle(V3d_View) view = myContext.View();
        const Handle(AIS_InteractiveContext) aisContext = myContext.AisContext();

        gp_Lin ray;
        try {
            Standard_Real x = 0.0, y = 0.0, z = 0.0, dx = 0.0, dy = 0.0, dz = 0.0;
            view->ConvertWithProj(thePos.x(), thePos.y(), x, y, z, dx, dy, dz);
            ray = gp_Lin(gp_Pnt(x, y, z), gp_Dir(dx, dy, dz));
        } catch (const Standard_Failure&) {
            return false;   // a degenerate view: no ray, no region
        }

        // The body half of the rule: the nearest visible body along the
        // ray...
        std::vector<TopoDS_Shape> bodies;
        for (const BodyPtr& body : document.Bodies()) {
            if (body && body->IsVisible() && !body->Shape().IsNull()) {
                bodies.push_back(body->Shape());
            }
        }
        myBodies.SetBodies(bodies);
        double nearest = myBodies.NearestDepth(ray);

        // ...and anything the ordinary click would select here. That adds
        // the edges picked a few pixels off the cursor, which a bare ray
        // misses, and it is how the view cube is told apart: it floats
        // over everything, so a cursor on it is never on a region.
        aisContext->MoveTo(thePos.x(), thePos.y(), view, Standard_False);
        const Handle(StdSelect_ViewerSelector3d)& selector = aisContext->MainSelector();
        for (Standard_Integer rank = 1; rank <= selector->NbPicked(); ++rank) {
            const Handle(SelectMgr_EntityOwner) owner = selector->Picked(rank);
            if (owner.IsNull()) {
                continue;
            }
            if (!Handle(AIS_ViewCube)::DownCast(owner->Selectable()).IsNull()) {
                return false;
            }
            const double depth =
                gp_Vec(ray.Location(), selector->PickedPoint(rank)).Dot(gp_Vec(ray.Direction()));
            nearest = std::min(nearest, depth);
        }

        const double tolerance = std::max(kCoplanarFloor, view->Convert(kCoplanarPixels));
        const ProfileHit hit = ProfileUnderRay(ray, targets, nearest, tolerance);
        if (!hit.IsHit()) {
            return false;
        }

        const SketchFeature* sketch = sketches[static_cast<std::size_t>(hit.target)];
        const std::vector<ProfileRegion>& regions = *targets[static_cast<std::size_t>(hit.target)].regions;
        theHit.sketch = sketch->Name();
        theHit.region = hit.region;
        theHit.ref = regions[static_cast<std::size_t>(hit.region)].ref;
        return true;
    }

    // Areas of what is picked, for the status bar.
    std::vector<double> PickedAreas()
    {
        std::vector<double> areas;
        const ProfileSelection& selection = ProfileSelection::Instance();
        const SketchFeature* sketch =
            SketchSession::Find(myContext.document, selection.SketchName());
        if (sketch == nullptr) {
            return areas;
        }
        for (const ProfileRegion& region : SketchDisplay::Instance().RegionsOf(*sketch)) {
            if (selection.Contains(region.ref)) {
                areas.push_back(region.area);
            }
        }
        return areas;
    }

    CommandContext myContext;
    BodyRayCaster  myBodies;
    bool           myIsInstalled = false;
    bool           myIsDisplayAttached = false;
    bool           myPressConsumed = false;
};

} // namespace

void InstallModelProfilePicker(const CommandContext& theContext)
{
    ModelProfilePicker::Instance().Install(theContext);
}

} // namespace lcad
