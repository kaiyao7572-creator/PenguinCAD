#include "sketch/SketchSelection.h"

#include "core/Document.h"
#include "sketch/SketchDisplay.h"
#include "sketch/SketchGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

// Characteristic points are easier to hit than the curves they belong to,
// so a corner wins over the lines meeting at it well before the cursor is
// actually on top of it.
constexpr double kPointBias = 1.6;

// Pixels of slop around a click, in both cases.
constexpr double kPickPixels = 9.0;

} // namespace

SketchSelection& SketchSelection::Instance()
{
    static SketchSelection theInstance;
    return theInstance;
}

void SketchSelection::Add(const SketchPointRef& theRef, bool theAdditive)
{
    if (!theRef.IsValid()) {
        if (!theAdditive) {
            myItems.clear();
        }
        return;
    }

    if (!theAdditive) {
        myItems.clear();
        myItems.push_back(theRef);
        return;
    }

    // Ctrl+clicking something already picked takes it back out, the way
    // every other selection in the app behaves.
    if (Contains(theRef)) {
        Remove(theRef);
        return;
    }
    myItems.push_back(theRef);
}

void SketchSelection::Remove(const SketchPointRef& theRef)
{
    myItems.erase(std::remove_if(myItems.begin(), myItems.end(),
                                 [&theRef](const SketchPointRef& theItem) {
                                     return theItem == theRef;
                                 }),
                  myItems.end());
}

void SketchSelection::Clear()
{
    myItems.clear();
}

bool SketchSelection::Contains(const SketchPointRef& theRef) const
{
    return std::find(myItems.begin(), myItems.end(), theRef) != myItems.end();
}

std::vector<int> SketchSelection::EntityIds() const
{
    std::vector<int> ids;
    ids.reserve(myItems.size());
    for (const SketchPointRef& item : myItems) {
        if (std::find(ids.begin(), ids.end(), item.entity) == ids.end()) {
            ids.push_back(item.entity);
        }
    }
    return ids;
}

void SketchSelection::Prune(const SketchFeature& theSketch)
{
    myItems.erase(std::remove_if(myItems.begin(), myItems.end(),
                                 [&theSketch](const SketchPointRef& theItem) {
                                     return theSketch.FindEntity(theItem.entity) == nullptr;
                                 }),
                  myItems.end());
}

bool ResolvePoint(const SketchFeature&  theSketch,
                  const SketchPointRef& theRef,
                  gp_Pnt2d&             theResult)
{
    const SketchEntity* entity = theSketch.FindEntity(theRef.entity);
    if (entity == nullptr) {
        return false;
    }
    switch (theRef.role) {
        case SketchPointRole::Start:  theResult = entity->StartPoint(); return true;
        case SketchPointRole::End:    theResult = entity->EndPoint(); return true;
        case SketchPointRole::Centre:
        case SketchPointRole::Whole:  theResult = entity->CentrePoint(); return true;
    }
    return false;
}

bool PickSketchEntity(const SketchFeature& theSketch,
                      const gp_Pnt2d&      thePoint,
                      double               theTolerance,
                      SketchPointRef&      theResult)
{
    double bestPointDistance = theTolerance * kPointBias;
    SketchPointRef bestPoint;

    double bestCurveDistance = theTolerance;
    SketchPointRef bestCurve;

    for (const SketchEntity& entity : theSketch.Entities()) {
        if (entity.id == 0) {
            continue;
        }

        auto considerPoint = [&](const gp_Pnt2d& theCandidate, SketchPointRole theRole) {
            const double distance = theCandidate.Distance(thePoint);
            if (distance < bestPointDistance) {
                bestPointDistance = distance;
                bestPoint = SketchPointRef{entity.id, theRole};
            }
        };

        if (!entity.IsCurve()) {
            considerPoint(entity.first, SketchPointRole::Whole);
            continue;
        }

        if (!entity.IsSelfClosed()) {
            considerPoint(entity.StartPoint(), SketchPointRole::Start);
            considerPoint(entity.EndPoint(), SketchPointRole::End);
        }
        switch (entity.kind) {
            case SketchEntity::Kind::Circle:
            case SketchEntity::Kind::Arc:
            case SketchEntity::Kind::Ellipse:
                considerPoint(entity.first, SketchPointRole::Centre);
                break;
            case SketchEntity::Kind::Line:
                considerPoint(entity.CentrePoint(), SketchPointRole::Centre);
                break;
            case SketchEntity::Kind::Spline:
            case SketchEntity::Kind::Point:
                break;
        }

        double param = 0.0;
        const double distance = SketchGeometry::DistanceTo(entity, thePoint, param);
        if (distance < bestCurveDistance) {
            bestCurveDistance = distance;
            bestCurve = SketchPointRef{entity.id, SketchPointRole::Whole};
        }
    }

    if (bestPoint.IsValid()) {
        theResult = bestPoint;
        return true;
    }
    if (bestCurve.IsValid()) {
        theResult = bestCurve;
        return true;
    }
    return false;
}

namespace {

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

// The resting state of sketch mode. It never draws anything; it only
// decides what the constraint, dimension and modify commands act on.
class SelectToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        const std::size_t count = SketchSelection::Instance().Count();
        if (count == 0) {
            return "Sketch: click geometry to select it. Ctrl+click adds, Del removes.";
        }
        return std::to_string(count)
             + " selected - pick a constraint or modify tool. Esc clears.";
    }

protected:
    void Reset() override {}
    bool IsCollecting() const override { return false; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }

        SketchPointRef picked;
        const double tolerance = std::max(PixelSize() * kPickPixels, 1.0e-4);
        myHitSomething = PickSketchEntity(*sketch, thePoint, tolerance, picked);

        SketchSelection::Instance().Add(picked, myAdditive);
        SketchDisplay::Instance().Refresh();
        SketchDisplay::Instance().Redraw();
    }

    // A click that found no sketch geometry belongs to the viewport, so
    // face picking and box selection keep working while a sketch is open.
    bool ConsumedLastPress() const override { return myHitSomething; }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }

        // Show what a click would land on, so picking a specific endpoint
        // out of a crowded corner isn't guesswork.
        SketchPointRef hovered;
        const double tolerance = std::max(PixelSize() * kPickPixels, 1.0e-4);
        if (!PickSketchEntity(*sketch, thePoint, tolerance, hovered)) {
            ClearPreview();
            return;
        }

        const SketchEntity* entity = sketch->FindEntity(hovered.entity);
        if (entity == nullptr) {
            ClearPreview();
            return;
        }

        if (hovered.role == SketchPointRole::Whole && entity->IsCurve()) {
            ShowPreview({*entity});
            return;
        }
        gp_Pnt2d position;
        if (ResolvePoint(*sketch, hovered, position)) {
            ShowPreview({SketchEntity::MakePoint(position)});
        }
    }

    bool OnMousePress(const Graphic3d_Vec2i& thePos,
                      Qt::MouseButton        theButton,
                      Qt::KeyboardModifiers  theModifiers) override
    {
        // Stashed for OnPoint, which the base class calls without the
        // modifiers it was pressed with.
        myAdditive = theModifiers.testFlag(Qt::ControlModifier);
        return SketchTool::OnMousePress(thePos, theButton, theModifiers);
    }

    bool OnKey(int theKey, Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;

        if (theKey == Qt::Key_Escape) {
            // Escape always walks one step out and never stops working:
            // it empties the selection first, and once there is nothing
            // left to clear it leaves the sketch. Combined with the
            // drawing tools (first Escape drops the curve, second drops
            // the tool back to here), Escape is a reliable way out of
            // wherever the user is.
            if (!SketchSelection::Instance().IsEmpty()) {
                SketchSelection::Instance().Clear();
                SketchDisplay::Instance().Refresh();
                SketchDisplay::Instance().Redraw();
                return true;
            }
            const CommandContext context = myContext;
            SketchSession::Instance().End(context);
            ShowStatus(context, "Sketch finished.");
            return true;
        }

        if (theKey != Qt::Key_Delete && theKey != Qt::Key_Backspace) {
            return false;
        }
        DeleteSelection();
        return true;
    }

private:
    void DeleteSelection()
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr || myContext.document == nullptr) {
            return;
        }
        const std::vector<int> ids = SketchSelection::Instance().EntityIds();
        if (ids.empty()) {
            return;
        }

        myContext.document->PushUndoSnapshot();
        for (const int id : ids) {
            sketch->RemoveEntity(id);
        }
        SketchSelection::Instance().Clear();
        ClearPreview();
        myContext.document->Rebuild();
        ShowStatus(myContext, QString("Deleted %1 sketch entities.")
                                  .arg(static_cast<int>(ids.size())));
    }

    bool myAdditive = false;
    bool myHitSomething = false;
};

} // namespace

SketchTool& SketchSelectTool()
{
    static SelectToolImpl theTool;
    return theTool;
}

} // namespace lcad
