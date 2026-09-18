#include "sketch/SketchTools.h"

#include "OcctViewport.h"
#include "core/Document.h"
#include "sketch/SketchDisplay.h"
#include "sketch/SketchGeometry.h"
#include "sketch/SketchSelection.h"
#include "sketch/SketchView.h"

#include <V3d_View.hxx>
#include <gp_Pln.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <QMainWindow>
#include <QStatusBar>
#include <QString>

namespace lcad {

namespace {

// Anything shorter than this is a mis-click (or the second half of a
// double click) rather than a curve the user meant to draw.
constexpr double kMinimumLength = 1.0e-6;

// Pixels, converted to model units through the view, within which a new
// point snaps onto an existing sketch endpoint.
constexpr Standard_Integer kSnapPixels = 8;

// Only one drawing tool can own the viewport at a time.
SketchTool* theRunningTool = nullptr;

void ShowStatus(const CommandContext& theContext, const QString& theText)
{
    if (QMainWindow* window = qobject_cast<QMainWindow*>(theContext.parent)) {
        window->statusBar()->showMessage(theText);
    }
}

double AngleOf(const gp_Pnt2d& theCentre, const gp_Pnt2d& thePoint)
{
    return std::atan2(thePoint.Y() - theCentre.Y(), thePoint.X() - theCentre.X());
}

// Signed distance from thePoint to the line through theFirst/theSecond,
// positive to the left. Slots and ellipses both size themselves off this.
double SideOffset(const gp_Pnt2d& theFirst, const gp_Pnt2d& theSecond, const gp_Pnt2d& thePoint)
{
    gp_Vec2d along(theSecond.X() - theFirst.X(), theSecond.Y() - theFirst.Y());
    if (along.SquareMagnitude() <= kMinimumLength * kMinimumLength) {
        return 0.0;
    }
    along.Normalize();
    const gp_Vec2d toPoint(thePoint.X() - theFirst.X(), thePoint.Y() - theFirst.Y());
    return along.X() * toPoint.Y() - along.Y() * toPoint.X();
}

// The coincidences Fusion infers when a closed shape is drawn in one go.
// Without them the corners come apart the first time anything is
// dimensioned.
void ChainCoincident(SketchFeature& theSketch, const std::vector<int>& theIds, bool theClosed)
{
    if (theIds.size() < 2) {
        return;
    }
    const std::size_t last = theClosed ? theIds.size() : theIds.size() - 1;
    for (std::size_t i = 0; i < last; ++i) {
        SketchConstraint constraint;
        constraint.type = SketchConstraintType::Coincident;
        constraint.a = SketchPointRef{theIds[i], SketchPointRole::End};
        constraint.b = SketchPointRef{theIds[(i + 1) % theIds.size()], SketchPointRole::Start};
        theSketch.AddConstraint(constraint);
    }
}

void AddSimpleConstraint(SketchFeature&       theSketch,
                         SketchConstraintType theType,
                         int                  theFirst,
                         int                  theSecond = 0)
{
    SketchConstraint constraint;
    constraint.type = theType;
    constraint.a = SketchPointRef{theFirst, SketchPointRole::Whole};
    if (theSecond != 0) {
        constraint.b = SketchPointRef{theSecond, SketchPointRole::Whole};
    }
    theSketch.AddConstraint(constraint);
}

// Direction a new curve should leave thePoint in so it continues the
// entity that already ends there. Returns false when nothing ends there.
bool OutwardTangentAt(const SketchFeature& theSketch,
                      const gp_Pnt2d&      thePoint,
                      double               theTolerance,
                      gp_Vec2d&            theDirection)
{
    const double squaredTolerance = theTolerance * theTolerance;

    for (const SketchEntity& entity : theSketch.Entities()) {
        if (!entity.IsCurve() || entity.IsSelfClosed() || entity.IsDegenerate()) {
            continue;
        }
        double first = 0.0, last = 0.0;
        if (!SketchGeometry::ParamRange(entity, first, last)) {
            continue;
        }

        // At the tail the curve carries straight on; at the head the new
        // curve leaves the other way, so the tangent is reversed.
        if (entity.EndPoint().SquareDistance(thePoint) <= squaredTolerance) {
            theDirection = SketchGeometry::TangentAt(entity, last);
            return theDirection.SquareMagnitude() > 0.0;
        }
        if (entity.StartPoint().SquareDistance(thePoint) <= squaredTolerance) {
            theDirection = SketchGeometry::TangentAt(entity, first) * -1.0;
            return theDirection.SquareMagnitude() > 0.0;
        }
    }
    return false;
}

} // namespace

// ---- SketchSession ----

SketchSession& SketchSession::Instance()
{
    static SketchSession theInstance;
    return theInstance;
}

SketchFeature* SketchSession::Find(Document* theDocument, const std::string& theName)
{
    if (theDocument == nullptr || theName.empty()) {
        return nullptr;
    }
    for (const FeaturePtr& feature : theDocument->Features()) {
        if (feature && feature->Name() == theName) {
            return dynamic_cast<SketchFeature*>(feature.get());
        }
    }
    return nullptr;
}

SketchFeature* SketchSession::ActiveSketch(Document* theDocument) const
{
    return Find(theDocument, myActiveName);
}

void SketchSession::Begin(const CommandContext& theContext, const std::string& theSketchName)
{
    // A tool left armed against the previous sketch would carry its
    // half-drawn curve over onto a different plane.
    StopActiveSketchTool();

    const bool changed = myActiveName != theSketchName;
    myActiveName = theSketchName;
    if (changed) {
        SketchSelection::Instance().Clear();
    }

    SketchDisplay::Instance().Attach(theContext);
    SketchDisplay::Instance().SetActiveSketchName(myActiveName);

    // Swinging the camera onto the plane, moving the grid under it and
    // fading the model back are what turn "a sketch exists" into "you are
    // in a sketch", so they happen here rather than in the commands --
    // every way into a sketch then feels identical, and End() is their one
    // guaranteed undo.
    if (const SketchFeature* sketch = ActiveSketch(theContext.document)) {
        SketchView::Instance().Enter(theContext, sketch->Position());
    }
    if (theContext.activateTab) {
        theContext.activateTab("Sketch");
    }

    // Selection is the resting state of sketch mode, exactly as it is in
    // Fusion: with no tool armed, clicks pick geometry for the constraint
    // and modify commands.
    SketchSelectTool().Start(theContext);
}

void SketchSession::End(const CommandContext& theContext)
{
    // Cleared before the tool is stopped: a tool shutting down asks the
    // session whether to hand control back to selection, and by now the
    // answer has to be no.
    myActiveName.clear();
    StopActiveSketchTool();
    SketchSelection::Instance().Clear();
    SketchDisplay::Instance().Attach(theContext);
    SketchDisplay::Instance().ClearPreview();
    SketchDisplay::Instance().SetActiveSketchName(std::string());

    // Undoes everything Begin() did to the viewport: grid back on world
    // XY, model back to full opacity, camera free to orbit again.
    SketchView::Instance().Leave(theContext);
    if (theContext.activateTab) {
        theContext.activateTab("Solid");
    }

    SketchDisplay::Instance().Refresh();
    SketchDisplay::Instance().Redraw();
}

// ---- SketchTool ----

void SketchTool::Start(const CommandContext& theContext)
{
    myContext = theContext;
    SketchDisplay::Instance().Attach(theContext);

    if (myIsRunning && theRunningTool == this) {
        // Re-running the active tool just restarts the curve in progress.
        Reset();
        ClearPreview();
        ShowHint();
        return;
    }

    StopActiveSketchTool();

    Reset();
    myIsRunning = true;
    myHasAnchor = false;   // a new tool infers from nothing until its first point
    myHasHover = false;
    theRunningTool = this;

    if (myContext.viewport != nullptr) {
        myContext.viewport->PushInteraction(this);
    }
    ShowHint();
}

void SketchTool::Stop()
{
    if (!myIsRunning) {
        return;
    }
    myIsRunning = false;
    myHasAnchor = false;
    if (theRunningTool == this) {
        theRunningTool = nullptr;
    }

    // Only pop when we're still on top: another subsystem may have pushed
    // its own handler over ours, and popping would take away theirs.
    if (myContext.viewport != nullptr && myContext.viewport->CurrentInteraction() == this) {
        myContext.viewport->PopInteraction();  // calls OnDeactivated()
    } else {
        OnDeactivated();
    }
}

void SketchTool::OnDeactivated()
{
    myIsRunning = false;
    myHasAnchor = false;
    myHasHover = false;
    if (theRunningTool == this) {
        theRunningTool = nullptr;
    }
    Reset();
    ClearPreview();
    ShowStatus(myContext, QString());
    SketchDisplay::Instance().Redraw();

    // Leaving a tool drops back to picking, not to nothing -- the same
    // place Escape leaves you in Fusion.
    if (this != &SketchSelectTool() && SketchSession::Instance().IsActive()) {
        SketchSelectTool().Start(myContext);
    }
}

SketchFeature* SketchTool::Sketch() const
{
    return SketchSession::Instance().ActiveSketch(myContext.document);
}

// True when the sketch already constrains this entity that way. AddConstraint
// does not deduplicate, and a tool that constrains its own geometry (the
// rectangle does) would otherwise end up with the constraint twice.
bool HasConstraintOn(const SketchFeature& theSketch, SketchConstraintType theType, int theId)
{
    for (const SketchConstraint& constraint : theSketch.Constraints()) {
        if (constraint.type == theType
            && (constraint.a.entity == theId || constraint.b.entity == theId)) {
            return true;
        }
    }
    return false;
}

// Fusion stamps a horizontal or vertical constraint on a segment that was
// drawn on-axis, so it stays on-axis when something downstream is
// dimensioned. Inference has already pulled the point exactly onto the
// axis by this stage, so an exact comparison is the right test.
void ConstrainAxisAlignedLines(SketchFeature& theSketch, const std::vector<int>& theIds)
{
    for (int id : theIds) {
        const SketchEntity* entity = theSketch.FindEntity(id);
        if (entity == nullptr || entity->kind != SketchEntity::Kind::Line) {
            continue;
        }
        const gp_Pnt2d start = entity->StartPoint();
        const gp_Pnt2d end = entity->EndPoint();
        const double dx = std::fabs(end.X() - start.X());
        const double dy = std::fabs(end.Y() - start.Y());
        if (dx <= SketchGeometry::kTolerance && dy <= SketchGeometry::kTolerance) {
            continue;   // degenerate
        }

        const SketchConstraintType type = (dy <= SketchGeometry::kTolerance)
                                              ? SketchConstraintType::Horizontal
                                              : (dx <= SketchGeometry::kTolerance)
                                                    ? SketchConstraintType::Vertical
                                                    : SketchConstraintType::Coincident;
        if (type == SketchConstraintType::Coincident) {
            continue;   // genuinely diagonal: leave it free
        }
        if (HasConstraintOn(theSketch, type, id)) {
            continue;
        }
        SketchConstraint constraint;
        constraint.type = type;
        constraint.a = SketchPointRef{id, SketchPointRole::Whole};
        theSketch.AddConstraint(constraint);
    }
}

std::vector<int> SketchTool::Commit(const std::vector<SketchEntity>& theEntities)
{
    std::vector<int> ids;

    SketchFeature* sketch = Sketch();
    if (sketch == nullptr || theEntities.empty() || myContext.document == nullptr) {
        return ids;
    }

    // One undo step per curve, so Ctrl+Z peels a sketch back a curve at a
    // time instead of discarding the whole thing.
    myContext.document->PushUndoSnapshot();
    ids = sketch->AddEntities(theEntities);
    OnCommitted(*sketch, ids);
    ConstrainAxisAlignedLines(*sketch, ids);

    ClearPreview();
    // Rebuild, not NotifyChanged: anything extruded from this sketch has
    // to re-evaluate against the new profile.
    myContext.document->Rebuild();
    return ids;
}

bool SketchTool::BeginEdit()
{
    if (myContext.document == nullptr || Sketch() == nullptr) {
        return false;
    }
    myContext.document->PushUndoSnapshot();
    return true;
}

void SketchTool::EndEdit()
{
    ClearPreview();
    if (myContext.document != nullptr) {
        myContext.document->Rebuild();
    }
}

void SketchTool::ShowPreview(const std::vector<SketchEntity>& theEntities)
{
    SketchFeature* sketch = Sketch();
    if (sketch == nullptr) {
        return;
    }
    SketchDisplay::Instance().ShowPreview(sketch->BuildCompound(theEntities));
}

void SketchTool::ClearPreview()
{
    SketchDisplay::Instance().ClearPreview();
}

void SketchTool::ShowHint()
{
    ShowStatus(myContext, QString::fromStdString(Hint()));
}

void SketchTool::RefreshPreview()
{
    if (myHasHover) {
        OnHover(myLastHover);
    }
}

double SketchTool::PixelSize() const
{
    const Handle(V3d_View) view = myContext.View();
    if (view.IsNull()) {
        return 0.0;
    }
    return view->Convert(1);
}

bool SketchTool::PlanePointAt(const Graphic3d_Vec2i& thePos, gp_Pnt2d& theResult) const
{
    SketchFeature* sketch = Sketch();
    const Handle(V3d_View) view = myContext.View();
    if (sketch == nullptr || view.IsNull()) {
        return false;
    }

    Standard_Real x = 0.0, y = 0.0, z = 0.0;
    Standard_Real vx = 0.0, vy = 0.0, vz = 0.0;
    view->ConvertWithProj(thePos.x(), thePos.y(), x, y, z, vx, vy, vz);

    const gp_Pln plane = sketch->Plane();
    const gp_Vec normal(plane.Axis().Direction());
    const gp_Vec ray(vx, vy, vz);

    const Standard_Real denominator = ray.Dot(normal);
    if (std::fabs(denominator) < 1.0e-9) {
        return false;  // looking edge-on at the plane: the ray never hits it
    }

    const gp_Pnt eye(x, y, z);
    const Standard_Real along = gp_Vec(eye, plane.Location()).Dot(normal) / denominator;
    gp_Pnt2d point = sketch->To2d(eye.Translated(ray * along));

    // Snap onto an existing endpoint. Without it a hand-drawn "closed"
    // polygon misses by a few microns and yields no profile at all, which
    // is the difference between an extrude that works and one that
    // reports an empty sketch.
    const Standard_Real snap = view->Convert(kSnapPixels);
    bool snappedToPoint = false;
    if (snap > 0.0) {
        double best = snap * snap;
        for (const gp_Pnt2d& candidate : sketch->SnapPoints()) {
            const double squared = candidate.SquareDistance(point);
            if (squared < best) {
                best = squared;
                point = candidate;
                snappedToPoint = true;
            }
        }
    }

    // A real endpoint always wins: inference only gets to act when nothing
    // was close enough to snap to, so locking onto an axis can never drag
    // a point off the corner the user was aiming at.
    if (!snappedToPoint && myHasAnchor) {
        point = SketchGeometry::AxisInferred(myAnchor, point);
    }

    theResult = point;
    return true;
}

bool SketchTool::OnMousePress(const Graphic3d_Vec2i& thePos,
                              Qt::MouseButton        theButton,
                              Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (!myIsRunning) {
        return false;
    }
    // Right and middle stay with the viewport so orbit and pan keep
    // working while a tool is armed.
    if (theButton != Qt::LeftButton) {
        return false;
    }

    if (Sketch() == nullptr) {
        Stop();  // the sketch was deleted or undone out from under us
        return true;
    }

    gp_Pnt2d point;
    if (PlanePointAt(thePos, point)) {
        OnPoint(point);
        // Whatever was just placed is what the next segment grows from.
        myAnchor = point;
        myHasAnchor = true;
        ShowHint();
    }
    return ConsumedLastPress();
}

bool SketchTool::OnMouseMove(const Graphic3d_Vec2i& thePos,
                             Qt::MouseButtons       theButtons,
                             Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (!myIsRunning) {
        return false;
    }
    if (theButtons.testFlag(Qt::RightButton) || theButtons.testFlag(Qt::MiddleButton)) {
        return false;  // orbit/pan drag in progress
    }
    if (theButtons.testFlag(Qt::LeftButton) && !ConsumedLastPress()) {
        return false;  // a box-select drag the viewport started, not ours
    }

    gp_Pnt2d point;
    if (PlanePointAt(thePos, point)) {
        myLastHover = point;
        myHasHover = true;
        OnHover(point);
    }
    return true;
}

bool SketchTool::OnMouseRelease(const Graphic3d_Vec2i& thePos,
                                Qt::MouseButton        theButton,
                                Qt::KeyboardModifiers  theModifiers)
{
    (void)thePos;
    (void)theModifiers;
    // Only when the press itself was consumed: the viewport's controller
    // never saw that button go down, and handing it the release would
    // leave it in a state it can't make sense of.
    return myIsRunning && theButton == Qt::LeftButton && ConsumedLastPress();
}

bool SketchTool::OnMouseDoubleClick(const Graphic3d_Vec2i& thePos,
                                    Qt::MouseButton        theButton,
                                    Qt::KeyboardModifiers  theModifiers)
{
    (void)theModifiers;
    if (!myIsRunning || theButton != Qt::LeftButton) {
        return false;
    }

    gp_Pnt2d point;
    if (PlanePointAt(thePos, point)) {
        OnDoubleClick(point);
        ShowHint();
    }
    return true;
}

bool SketchTool::OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers)
{
    if (!myIsRunning) {
        return false;
    }
    if (OnKey(theKey, theModifiers)) {
        // A key that ended the tool -- Escape walking out of the sketch --
        // has already had its say in the status bar, and re-announcing the
        // hint would talk straight over it.
        if (myIsRunning) {
            ShowHint();
        }
        return true;
    }
    if (theKey != Qt::Key_Escape) {
        return false;
    }

    // First Escape abandons the curve being drawn, second one exits the
    // tool -- the behaviour every CAD package has.
    if (IsCollecting()) {
        Reset();
        ClearPreview();
        ShowHint();
        return true;
    }

    Stop();
    return true;
}

// ---- SketchPolygonTool ----

void SketchPolygonTool::SetSides(int theSides)
{
    mySides = std::min(std::max(theSides, 3), 64);
}

bool SketchPolygonTool::OnKey(int theKey, Qt::KeyboardModifiers theModifiers)
{
    (void)theModifiers;
    // Brackets nudge the side count without leaving the rubber band, the
    // way Fusion's on-canvas field does.
    if (theKey == Qt::Key_BracketRight || theKey == Qt::Key_Plus || theKey == Qt::Key_Equal) {
        SetSides(mySides + 1);
        RefreshPreview();
        return true;
    }
    if (theKey == Qt::Key_BracketLeft || theKey == Qt::Key_Minus) {
        SetSides(mySides - 1);
        RefreshPreview();
        return true;
    }
    return false;
}

// ---- concrete tools ----

namespace {

class LineToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        return myHasStart ? "Line: click the next point. Esc ends the chain."
                          : "Line: click the start point. Esc exits the tool.";
    }

protected:
    void Reset() override
    {
        myHasStart = false;
        myPreviousId = 0;
    }
    bool IsCollecting() const override { return myHasStart; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (!myHasStart) {
            myStart = thePoint;
            myHasStart = true;
            return;
        }
        if (myStart.SquareDistance(thePoint) <= kMinimumLength * kMinimumLength) {
            return;
        }

        const std::vector<int> ids = Commit({SketchEntity::MakeLine(myStart, thePoint)});
        myPreviousId = ids.empty() ? 0 : ids.front();
        myStart = thePoint;  // chain on, Fusion-style, until Escape
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (!myHasStart) {
            return;
        }
        ShowPreview({SketchEntity::MakeLine(myStart, thePoint)});
    }

    void OnCommitted(SketchFeature& theSketch, const std::vector<int>& theIds) override
    {
        // Each segment is pinned to the one before it, so the chain stays
        // a chain when anything downstream moves.
        if (myPreviousId == 0 || theIds.empty()) {
            return;
        }
        SketchConstraint constraint;
        constraint.type = SketchConstraintType::Coincident;
        constraint.a = SketchPointRef{myPreviousId, SketchPointRole::End};
        constraint.b = SketchPointRef{theIds.front(), SketchPointRole::Start};
        theSketch.AddConstraint(constraint);
    }

private:
    bool     myHasStart = false;
    gp_Pnt2d myStart;
    int      myPreviousId = 0;
};

// Shared behaviour for the three rectangle tools: they differ only in
// which points they collect and how the four sides fall out of them.
class RectangleToolBase : public SketchTool
{
protected:
    void OnCommitted(SketchFeature& theSketch, const std::vector<int>& theIds) override
    {
        if (theIds.size() != 4) {
            return;
        }
        ChainCoincident(theSketch, theIds, true);
        ApplyShapeConstraints(theSketch, theIds);
    }

    // Axis-aligned rectangles are held square by horizontals and
    // verticals; a rotated one by a right angle plus two parallels.
    virtual void ApplyShapeConstraints(SketchFeature& theSketch, const std::vector<int>& theIds)
    {
        AddSimpleConstraint(theSketch, SketchConstraintType::Horizontal, theIds[0]);
        AddSimpleConstraint(theSketch, SketchConstraintType::Horizontal, theIds[2]);
        AddSimpleConstraint(theSketch, SketchConstraintType::Vertical, theIds[1]);
        AddSimpleConstraint(theSketch, SketchConstraintType::Vertical, theIds[3]);
    }
};

class RectangleToolImpl : public RectangleToolBase
{
public:
    std::string Hint() const override
    {
        return myHasCorner ? "Rectangle: click the opposite corner. Esc cancels."
                           : "Rectangle: click the first corner. Esc exits the tool.";
    }

protected:
    void Reset() override { myHasCorner = false; }
    bool IsCollecting() const override { return myHasCorner; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (!myHasCorner) {
            myCorner = thePoint;
            myHasCorner = true;
            return;
        }
        const std::vector<SketchEntity> sides =
            SketchGeometry::RectangleTwoPoint(myCorner, thePoint);
        if (sides.size() != 4 || !IsValid(thePoint)) {
            return;
        }
        Commit(sides);
        myHasCorner = false;
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (!myHasCorner || !IsValid(thePoint)) {
            return;
        }
        ShowPreview(SketchGeometry::RectangleTwoPoint(myCorner, thePoint));
    }

private:
    bool IsValid(const gp_Pnt2d& theOpposite) const
    {
        return std::fabs(theOpposite.X() - myCorner.X()) > kMinimumLength
            && std::fabs(theOpposite.Y() - myCorner.Y()) > kMinimumLength;
    }

    bool     myHasCorner = false;
    gp_Pnt2d myCorner;
};

class CentreRectangleToolImpl : public RectangleToolBase
{
public:
    std::string Hint() const override
    {
        return myHasCentre ? "Center Rectangle: click a corner. Esc cancels."
                           : "Center Rectangle: click the centre. Esc exits the tool.";
    }

protected:
    void Reset() override { myHasCentre = false; }
    bool IsCollecting() const override { return myHasCentre; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (!myHasCentre) {
            myCentre = thePoint;
            myHasCentre = true;
            return;
        }
        if (!IsValid(thePoint)) {
            return;
        }
        Commit(SketchGeometry::RectangleCentre(myCentre, thePoint));
        myHasCentre = false;
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (!myHasCentre || !IsValid(thePoint)) {
            return;
        }
        ShowPreview(SketchGeometry::RectangleCentre(myCentre, thePoint));
    }

private:
    bool IsValid(const gp_Pnt2d& theCorner) const
    {
        return std::fabs(theCorner.X() - myCentre.X()) > kMinimumLength
            && std::fabs(theCorner.Y() - myCentre.Y()) > kMinimumLength;
    }

    bool     myHasCentre = false;
    gp_Pnt2d myCentre;
};

class ThreePointRectangleToolImpl : public RectangleToolBase
{
public:
    std::string Hint() const override
    {
        switch (myStage) {
            case Stage::First:  return "3-Point Rectangle: click one end of the first edge.";
            case Stage::Second: return "3-Point Rectangle: click the other end of that edge.";
            case Stage::Third:  return "3-Point Rectangle: click to set the width.";
        }
        return "3-Point Rectangle";
    }

protected:
    void Reset() override { myStage = Stage::First; }
    bool IsCollecting() const override { return myStage != Stage::First; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        switch (myStage) {
            case Stage::First:
                myFirst = thePoint;
                myStage = Stage::Second;
                return;

            case Stage::Second:
                if (myFirst.Distance(thePoint) <= kMinimumLength) {
                    return;
                }
                mySecond = thePoint;
                myStage = Stage::Third;
                return;

            case Stage::Third: {
                const std::vector<SketchEntity> sides =
                    SketchGeometry::RectangleThreePoint(myFirst, mySecond, thePoint);
                if (sides.size() != 4) {
                    return;
                }
                Commit(sides);
                myStage = Stage::First;
                return;
            }
        }
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        switch (myStage) {
            case Stage::First:
                return;
            case Stage::Second:
                if (myFirst.Distance(thePoint) > kMinimumLength) {
                    ShowPreview({SketchEntity::MakeLine(myFirst, thePoint)});
                }
                return;
            case Stage::Third: {
                const std::vector<SketchEntity> sides =
                    SketchGeometry::RectangleThreePoint(myFirst, mySecond, thePoint);
                if (!sides.empty()) {
                    ShowPreview(sides);
                }
                return;
            }
        }
    }

    void ApplyShapeConstraints(SketchFeature& theSketch, const std::vector<int>& theIds) override
    {
        // A rotated rectangle can't use horizontals; a right angle at one
        // corner plus parallel opposite sides does the same job.
        AddSimpleConstraint(theSketch, SketchConstraintType::Perpendicular, theIds[0], theIds[1]);
        AddSimpleConstraint(theSketch, SketchConstraintType::Parallel, theIds[0], theIds[2]);
        AddSimpleConstraint(theSketch, SketchConstraintType::Parallel, theIds[1], theIds[3]);
    }

private:
    enum class Stage { First, Second, Third };

    Stage    myStage = Stage::First;
    gp_Pnt2d myFirst;
    gp_Pnt2d mySecond;
};

class CircleToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        return myHasCentre ? "Circle: click a point on the rim. Esc cancels."
                           : "Circle: click the centre. Esc exits the tool.";
    }

protected:
    void Reset() override { myHasCentre = false; }
    bool IsCollecting() const override { return myHasCentre; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (!myHasCentre) {
            myCentre = thePoint;
            myHasCentre = true;
            return;
        }

        const double radius = myCentre.Distance(thePoint);
        if (radius <= kMinimumLength) {
            return;
        }

        Commit({SketchEntity::MakeCircle(myCentre, radius)});
        myHasCentre = false;
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (!myHasCentre) {
            return;
        }
        const double radius = myCentre.Distance(thePoint);
        if (radius <= kMinimumLength) {
            return;
        }
        ShowPreview({SketchEntity::MakeCircle(myCentre, radius)});
    }

private:
    bool     myHasCentre = false;
    gp_Pnt2d myCentre;
};

class TwoPointCircleToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        return myHasFirst ? "2-Point Circle: click the far end of the diameter. Esc cancels."
                          : "2-Point Circle: click one end of the diameter. Esc exits the tool.";
    }

protected:
    void Reset() override { myHasFirst = false; }
    bool IsCollecting() const override { return myHasFirst; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (!myHasFirst) {
            myFirst = thePoint;
            myHasFirst = true;
            return;
        }
        SketchEntity circle;
        if (!SketchGeometry::CircleTwoPoint(myFirst, thePoint, circle)) {
            return;
        }
        Commit({circle});
        myHasFirst = false;
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        SketchEntity circle;
        if (myHasFirst && SketchGeometry::CircleTwoPoint(myFirst, thePoint, circle)) {
            ShowPreview({circle});
        }
    }

private:
    bool     myHasFirst = false;
    gp_Pnt2d myFirst;
};

class ThreePointCircleToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        switch (myCount) {
            case 0:  return "3-Point Circle: click the first point on the rim.";
            case 1:  return "3-Point Circle: click the second point.";
            default: return "3-Point Circle: click the third point.";
        }
    }

protected:
    void Reset() override { myCount = 0; }
    bool IsCollecting() const override { return myCount > 0; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (myCount < 2) {
            myPoints[myCount++] = thePoint;
            return;
        }
        SketchEntity circle;
        if (!SketchGeometry::CircleThreePoint(myPoints[0], myPoints[1], thePoint, circle)) {
            return;  // collinear picks have no circumcircle
        }
        Commit({circle});
        myCount = 0;
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (myCount == 1) {
            ShowPreview({SketchEntity::MakeLine(myPoints[0], thePoint)});
            return;
        }
        if (myCount == 2) {
            SketchEntity circle;
            if (SketchGeometry::CircleThreePoint(myPoints[0], myPoints[1], thePoint, circle)) {
                ShowPreview({circle});
            }
        }
    }

private:
    int      myCount = 0;
    gp_Pnt2d myPoints[2];
};

// Centre-start-end arc. Three unambiguous clicks, unlike a three-point
// arc, which needs a circumcircle and degrades badly on collinear picks.
class ArcToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        switch (myStage) {
            case Stage::Centre: return "Arc: click the centre. Esc exits the tool.";
            case Stage::Start:  return "Arc: click the start point (sets the radius). Esc cancels.";
            case Stage::End:    return "Arc: click the end point; the arc sweeps counter-clockwise.";
        }
        return "Arc";
    }

protected:
    void Reset() override { myStage = Stage::Centre; }
    bool IsCollecting() const override { return myStage != Stage::Centre; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        switch (myStage) {
            case Stage::Centre:
                myCentre = thePoint;
                myStage = Stage::Start;
                return;

            case Stage::Start: {
                const double radius = myCentre.Distance(thePoint);
                if (radius <= kMinimumLength) {
                    return;
                }
                myRadius = radius;
                myStartAngle = AngleOf(myCentre, thePoint);
                myStage = Stage::End;
                return;
            }

            case Stage::End: {
                const double endAngle = AngleOf(myCentre, thePoint);
                if (std::fabs(endAngle - myStartAngle) <= 1.0e-9) {
                    return;
                }
                Commit({SketchEntity::MakeArc(myCentre, myRadius, myStartAngle, endAngle)});
                myStage = Stage::Centre;
                return;
            }
        }
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        switch (myStage) {
            case Stage::Centre:
                return;

            case Stage::Start: {
                const double radius = myCentre.Distance(thePoint);
                if (radius <= kMinimumLength) {
                    return;
                }
                // Full circle plus the radius line: shows both the size
                // being chosen and where the arc will start.
                ShowPreview({SketchEntity::MakeCircle(myCentre, radius),
                             SketchEntity::MakeLine(myCentre, thePoint)});
                return;
            }

            case Stage::End: {
                const double endAngle = AngleOf(myCentre, thePoint);
                if (std::fabs(endAngle - myStartAngle) <= 1.0e-9) {
                    return;
                }
                ShowPreview({SketchEntity::MakeArc(myCentre, myRadius, myStartAngle, endAngle)});
                return;
            }
        }
    }

private:
    enum class Stage { Centre, Start, End };

    Stage    myStage = Stage::Centre;
    gp_Pnt2d myCentre;
    double   myRadius = 0.0;
    double   myStartAngle = 0.0;
};

class ThreePointArcToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        switch (myCount) {
            case 0:  return "3-Point Arc: click the start point. Esc exits the tool.";
            case 1:  return "3-Point Arc: click the end point.";
            default: return "3-Point Arc: click a point the arc passes through.";
        }
    }

protected:
    void Reset() override { myCount = 0; }
    bool IsCollecting() const override { return myCount > 0; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (myCount < 2) {
            myPoints[myCount++] = thePoint;
            return;
        }
        SketchEntity arc;
        if (!SketchGeometry::ArcThreePoint(myPoints[0], thePoint, myPoints[1], arc)) {
            return;
        }
        Commit({arc});
        myCount = 0;
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (myCount == 1) {
            ShowPreview({SketchEntity::MakeLine(myPoints[0], thePoint)});
            return;
        }
        if (myCount == 2) {
            SketchEntity arc;
            if (SketchGeometry::ArcThreePoint(myPoints[0], thePoint, myPoints[1], arc)) {
                ShowPreview({arc});
            } else {
                // Straight through: show the chord so the rubber band
                // never just vanishes.
                ShowPreview({SketchEntity::MakeLine(myPoints[0], myPoints[1])});
            }
        }
    }

private:
    int      myCount = 0;
    gp_Pnt2d myPoints[2];
};

class TangentArcToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        return myHasStart
                   ? "Tangent Arc: click the end point. Esc cancels."
                   : "Tangent Arc: click an endpoint of an existing curve. Esc exits the tool.";
    }

protected:
    void Reset() override { myHasStart = false; }
    bool IsCollecting() const override { return myHasStart; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (!myHasStart) {
            if (!CaptureStart(thePoint)) {
                return;
            }
            myHasStart = true;
            return;
        }

        SketchEntity arc;
        if (!SketchGeometry::ArcTangent(myStart, myDirection, thePoint, arc)) {
            return;
        }
        Commit({arc});
        myHasStart = false;
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (!myHasStart) {
            return;
        }
        SketchEntity arc;
        if (SketchGeometry::ArcTangent(myStart, myDirection, thePoint, arc)) {
            ShowPreview({arc});
        }
    }

private:
    // The whole point of a tangent arc is that it continues the curve it
    // starts from, so the start click has to land on one.
    bool CaptureStart(const gp_Pnt2d& thePoint)
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return false;
        }
        const double tolerance = std::max(PixelSize() * 8.0, 1.0e-4);
        if (!OutwardTangentAt(*sketch, thePoint, tolerance, myDirection)) {
            return false;
        }
        myStart = thePoint;
        return true;
    }

    bool     myHasStart = false;
    gp_Pnt2d myStart;
    gp_Vec2d myDirection;
};

// The three polygon flavours differ only in what the second click means,
// so they share everything but that one call.
class PolygonToolImpl : public SketchPolygonTool
{
public:
    enum class Mode { Inscribed, Circumscribed, Edge };

    explicit PolygonToolImpl(Mode theMode)
        : myMode(theMode)
    {
    }

    std::string Hint() const override
    {
        const std::string count = std::to_string(mySides) + "-sided ([ and ] change it)";
        if (!myHasFirst) {
            return myMode == Mode::Edge
                       ? "Polygon on Edge: click one end of an edge -- " + count
                       : "Polygon: click the centre -- " + count;
        }
        switch (myMode) {
            case Mode::Inscribed:     return "Polygon: click a vertex -- " + count;
            case Mode::Circumscribed: return "Polygon: click the middle of an edge -- " + count;
            case Mode::Edge:          return "Polygon: click the other end of the edge -- " + count;
        }
        return "Polygon";
    }

protected:
    void Reset() override { myHasFirst = false; }
    bool IsCollecting() const override { return myHasFirst; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (!myHasFirst) {
            myFirst = thePoint;
            myHasFirst = true;
            return;
        }
        const std::vector<SketchEntity> sides = Build(thePoint);
        if (sides.empty()) {
            return;
        }
        Commit(sides);
        myHasFirst = false;
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (!myHasFirst) {
            return;
        }
        const std::vector<SketchEntity> sides = Build(thePoint);
        if (!sides.empty()) {
            ShowPreview(sides);
        }
    }

    void OnCommitted(SketchFeature& theSketch, const std::vector<int>& theIds) override
    {
        ChainCoincident(theSketch, theIds, true);
        // Equal sides are what make it a regular polygon rather than a
        // closed chain that happens to look like one.
        for (std::size_t i = 1; i < theIds.size(); ++i) {
            AddSimpleConstraint(theSketch, SketchConstraintType::Equal, theIds[i - 1], theIds[i]);
        }
    }

private:
    std::vector<SketchEntity> Build(const gp_Pnt2d& theReference) const
    {
        switch (myMode) {
            case Mode::Inscribed:
                return SketchGeometry::PolygonInscribed(myFirst, theReference, mySides);
            case Mode::Circumscribed:
                return SketchGeometry::PolygonCircumscribed(myFirst, theReference, mySides);
            case Mode::Edge:
                return SketchGeometry::PolygonOnEdge(myFirst, theReference, mySides);
        }
        return {};
    }

    Mode     myMode;
    bool     myHasFirst = false;
    gp_Pnt2d myFirst;
};

class EllipseToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        switch (myStage) {
            case Stage::Centre: return "Ellipse: click the centre. Esc exits the tool.";
            case Stage::Major:  return "Ellipse: click the end of the major axis.";
            case Stage::Minor:  return "Ellipse: click to set the minor axis.";
        }
        return "Ellipse";
    }

protected:
    void Reset() override { myStage = Stage::Centre; }
    bool IsCollecting() const override { return myStage != Stage::Centre; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        switch (myStage) {
            case Stage::Centre:
                myCentre = thePoint;
                myStage = Stage::Major;
                return;

            case Stage::Major: {
                const double radius = myCentre.Distance(thePoint);
                if (radius <= kMinimumLength) {
                    return;
                }
                myMajorRadius = radius;
                myRotation = AngleOf(myCentre, thePoint);
                myMajorEnd = thePoint;
                myStage = Stage::Minor;
                return;
            }

            case Stage::Minor: {
                const double minor = MinorRadiusAt(thePoint);
                if (minor <= kMinimumLength) {
                    return;
                }
                Commit({SketchEntity::MakeEllipse(myCentre, myMajorRadius, minor, myRotation)});
                myStage = Stage::Centre;
                return;
            }
        }
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        switch (myStage) {
            case Stage::Centre:
                return;
            case Stage::Major:
                if (myCentre.Distance(thePoint) > kMinimumLength) {
                    ShowPreview({SketchEntity::MakeLine(myCentre, thePoint)});
                }
                return;
            case Stage::Minor: {
                const double minor = MinorRadiusAt(thePoint);
                if (minor > kMinimumLength) {
                    ShowPreview(
                        {SketchEntity::MakeEllipse(myCentre, myMajorRadius, minor, myRotation)});
                }
                return;
            }
        }
    }

private:
    // The minor radius is how far off the major axis the cursor is, so
    // sliding along the axis doesn't change the shape.
    double MinorRadiusAt(const gp_Pnt2d& thePoint) const
    {
        return std::fabs(SideOffset(myCentre, myMajorEnd, thePoint));
    }

    enum class Stage { Centre, Major, Minor };

    Stage    myStage = Stage::Centre;
    gp_Pnt2d myCentre;
    gp_Pnt2d myMajorEnd;
    double   myMajorRadius = 0.0;
    double   myRotation = 0.0;
};

class SlotToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        switch (myStage) {
            case Stage::First:  return "Slot: click the first centre. Esc exits the tool.";
            case Stage::Second: return "Slot: click the second centre.";
            case Stage::Width:  return "Slot: click to set the width.";
        }
        return "Slot";
    }

protected:
    void Reset() override { myStage = Stage::First; }
    bool IsCollecting() const override { return myStage != Stage::First; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        switch (myStage) {
            case Stage::First:
                myFirst = thePoint;
                myStage = Stage::Second;
                return;

            case Stage::Second:
                if (myFirst.Distance(thePoint) <= kMinimumLength) {
                    return;
                }
                mySecond = thePoint;
                myStage = Stage::Width;
                return;

            case Stage::Width: {
                const std::vector<SketchEntity> pieces =
                    SketchGeometry::Slot(myFirst, mySecond, WidthAt(thePoint));
                if (pieces.empty()) {
                    return;
                }
                Commit(pieces);
                myStage = Stage::First;
                return;
            }
        }
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        switch (myStage) {
            case Stage::First:
                return;
            case Stage::Second:
                if (myFirst.Distance(thePoint) > kMinimumLength) {
                    ShowPreview({SketchEntity::MakeLine(myFirst, thePoint)});
                }
                return;
            case Stage::Width: {
                const std::vector<SketchEntity> pieces =
                    SketchGeometry::Slot(myFirst, mySecond, WidthAt(thePoint));
                if (!pieces.empty()) {
                    ShowPreview(pieces);
                }
                return;
            }
        }
    }

    void OnCommitted(SketchFeature& theSketch, const std::vector<int>& theIds) override
    {
        ChainCoincident(theSketch, theIds, true);
    }

private:
    double WidthAt(const gp_Pnt2d& thePoint) const
    {
        return std::fabs(SideOffset(myFirst, mySecond, thePoint)) * 2.0;
    }

    enum class Stage { First, Second, Width };

    Stage    myStage = Stage::First;
    gp_Pnt2d myFirst;
    gp_Pnt2d mySecond;
};

class SplineToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        if (myPoints.size() < 2) {
            return "Spline: click fit points. Esc exits the tool.";
        }
        return "Spline: click more points, or press Enter / double-click to finish.";
    }

protected:
    void Reset() override { myPoints.clear(); }
    bool IsCollecting() const override { return !myPoints.empty(); }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (!myPoints.empty()
            && myPoints.back().SquareDistance(thePoint) <= kMinimumLength * kMinimumLength) {
            return;
        }
        myPoints.push_back(thePoint);
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (myPoints.empty()) {
            return;
        }
        std::vector<gp_Pnt2d> preview = myPoints;
        preview.push_back(thePoint);
        const SketchEntity spline = SketchEntity::MakeSpline(preview);
        if (!spline.IsDegenerate()) {
            ShowPreview({spline});
        }
    }

    void OnDoubleClick(const gp_Pnt2d& thePoint) override
    {
        // The press that opened the double click already added this point,
        // so only a genuinely new one is worth keeping.
        if (myPoints.empty()
            || myPoints.back().SquareDistance(thePoint) > kMinimumLength * kMinimumLength) {
            myPoints.push_back(thePoint);
        }
        Finish();
    }

    bool OnKey(int theKey, Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        if (theKey != Qt::Key_Return && theKey != Qt::Key_Enter) {
            return false;
        }
        Finish();
        return true;
    }

private:
    void Finish()
    {
        if (myPoints.size() >= 2) {
            const SketchEntity spline = SketchEntity::MakeSpline(myPoints);
            if (!spline.IsDegenerate()) {
                Commit({spline});
            }
        }
        myPoints.clear();
        ClearPreview();
    }

    std::vector<gp_Pnt2d> myPoints;
};

// Same three clicks as the spline tool's first three, but the points are
// CONTROL poles: the curve is pulled towards them rather than through
// them. Two different tools because they are two different intentions,
// and a user who wants one is never served by the other.
class ControlPointSplineToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        if (myPoints.size() < 2) {
            return "Control point spline: click control points. Esc exits the tool.";
        }
        return "Control point spline: click more, or press Enter / double-click to finish.";
    }

protected:
    void Reset() override { myPoints.clear(); }
    bool IsCollecting() const override { return !myPoints.empty(); }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (!myPoints.empty()
            && myPoints.back().SquareDistance(thePoint) <= kMinimumLength * kMinimumLength) {
            return;
        }
        myPoints.push_back(thePoint);
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (myPoints.empty()) {
            return;
        }
        std::vector<gp_Pnt2d> preview = myPoints;
        preview.push_back(thePoint);
        const SketchEntity spline = SketchEntity::MakeControlPointSpline(preview);
        if (!spline.IsDegenerate()) {
            ShowPreview({spline});
        }
    }

    void OnDoubleClick(const gp_Pnt2d& thePoint) override
    {
        if (myPoints.empty()
            || myPoints.back().SquareDistance(thePoint) > kMinimumLength * kMinimumLength) {
            myPoints.push_back(thePoint);
        }
        Finish();
    }

    bool OnKey(int theKey, Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        if (theKey != Qt::Key_Return && theKey != Qt::Key_Enter) {
            return false;
        }
        Finish();
        return true;
    }

private:
    void Finish()
    {
        if (myPoints.size() >= 2) {
            const SketchEntity spline = SketchEntity::MakeControlPointSpline(myPoints);
            if (!spline.IsDegenerate()) {
                Commit({spline});
            }
        }
        myPoints.clear();
        ClearPreview();
    }

    std::vector<gp_Pnt2d> myPoints;
};

// Two ends, then a point the curve must pass THROUGH.
//
// The apex -- where the two end tangents meet -- is what the geometry
// actually stores, but it is not a thing a user can point at. So the
// third click is the shoulder, the visible middle of the curve, and the
// apex is derived from it: shoulder = (1 - rho) * chordMidpoint +
// rho * apex, so apex = midpoint + (shoulder - midpoint) / rho. Changing
// rho with the bracket keys then slides the apex while the curve keeps
// passing through the point that was clicked, which is the behaviour that
// makes rho explorable rather than abstract.
class ConicToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        if (myPoints.empty()) {
            return "Conic: click the start point. Esc exits the tool.";
        }
        if (myPoints.size() == 1) {
            return "Conic: click the end point.";
        }
        return "Conic: click a point on the curve. [ and ] change rho ("
             + FormatRho() + ": " + Family() + ").";
    }

protected:
    void Reset() override { myPoints.clear(); }
    bool IsCollecting() const override { return !myPoints.empty(); }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        if (myPoints.size() < 2) {
            if (!myPoints.empty()
                && myPoints.back().SquareDistance(thePoint) <= kMinimumLength * kMinimumLength) {
                return;
            }
            myPoints.push_back(thePoint);
            return;
        }

        SketchEntity conic;
        if (!BuildFrom(thePoint, conic)) {
            return;
        }
        Commit({conic});
        myPoints.clear();
        ClearPreview();
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        if (myPoints.empty()) {
            return;
        }
        if (myPoints.size() == 1) {
            // Nothing to bulge towards yet, so show the chord the conic
            // will span rather than nothing at all.
            const SketchEntity chord = SketchEntity::MakeLine(myPoints.front(), thePoint);
            if (!chord.IsDegenerate()) {
                ShowPreview({chord});
            }
            return;
        }

        SketchEntity conic;
        if (BuildFrom(thePoint, conic)) {
            ShowPreview({conic});
        }
    }

    bool OnKey(int theKey, Qt::KeyboardModifiers theModifiers) override
    {
        (void)theModifiers;
        if (theKey == Qt::Key_BracketRight || theKey == Qt::Key_Plus || theKey == Qt::Key_Equal) {
            SetRho(myRho + kRhoStep);
            RefreshPreview();
            return true;
        }
        if (theKey == Qt::Key_BracketLeft || theKey == Qt::Key_Minus) {
            SetRho(myRho - kRhoStep);
            RefreshPreview();
            return true;
        }
        return false;
    }

private:
    static constexpr double kRhoStep = 0.05;

    void SetRho(double theValue)
    {
        // Held clear of both ends: at 0 the curve collapses onto the
        // chord and at 1 it runs off to the apex.
        myRho = std::min(std::max(theValue, 0.05), 0.95);
    }

    std::string FormatRho() const
    {
        char text[16];
        std::snprintf(text, sizeof(text), "rho %.2f", myRho);
        return std::string(text);
    }

    std::string Family() const
    {
        if (myRho < 0.5 - 1.0e-9) {
            return "ellipse";
        }
        return myRho > 0.5 + 1.0e-9 ? "hyperbola" : "parabola";
    }

    bool BuildFrom(const gp_Pnt2d& theShoulder, SketchEntity& theResult) const
    {
        if (myPoints.size() < 2) {
            return false;
        }
        const gp_Pnt2d start = myPoints[0];
        const gp_Pnt2d end = myPoints[1];
        const gp_Pnt2d middle(0.5 * (start.X() + end.X()), 0.5 * (start.Y() + end.Y()));

        const gp_Vec2d bulge(theShoulder.X() - middle.X(), theShoulder.Y() - middle.Y());
        if (bulge.SquareMagnitude() <= kMinimumLength * kMinimumLength) {
            return false;  // no bulge means no conic, only the chord
        }

        const gp_Pnt2d apex(middle.X() + bulge.X() / myRho, middle.Y() + bulge.Y() / myRho);
        theResult = SketchEntity::MakeConic(start, apex, end, myRho);
        return !theResult.IsDegenerate();
    }

    std::vector<gp_Pnt2d> myPoints;
    double                myRho = 0.5;
};

class PointToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        return "Point: click to place a sketch point. Esc exits the tool.";
    }

protected:
    void Reset() override {}
    bool IsCollecting() const override { return false; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        Commit({SketchEntity::MakePoint(thePoint)});
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        ShowPreview({SketchEntity::MakePoint(thePoint)});
    }
};

} // namespace

SketchTool& LineTool()
{
    static LineToolImpl theTool;
    return theTool;
}

SketchTool& RectangleTool()
{
    static RectangleToolImpl theTool;
    return theTool;
}

SketchTool& CentreRectangleTool()
{
    static CentreRectangleToolImpl theTool;
    return theTool;
}

SketchTool& ThreePointRectangleTool()
{
    static ThreePointRectangleToolImpl theTool;
    return theTool;
}

SketchTool& CircleTool()
{
    static CircleToolImpl theTool;
    return theTool;
}

SketchTool& TwoPointCircleTool()
{
    static TwoPointCircleToolImpl theTool;
    return theTool;
}

SketchTool& ThreePointCircleTool()
{
    static ThreePointCircleToolImpl theTool;
    return theTool;
}

SketchTool& ArcTool()
{
    static ArcToolImpl theTool;
    return theTool;
}

SketchTool& ThreePointArcTool()
{
    static ThreePointArcToolImpl theTool;
    return theTool;
}

SketchTool& TangentArcTool()
{
    static TangentArcToolImpl theTool;
    return theTool;
}

SketchPolygonTool& InscribedPolygonTool()
{
    static PolygonToolImpl theTool(PolygonToolImpl::Mode::Inscribed);
    return theTool;
}

SketchPolygonTool& CircumscribedPolygonTool()
{
    static PolygonToolImpl theTool(PolygonToolImpl::Mode::Circumscribed);
    return theTool;
}

SketchPolygonTool& EdgePolygonTool()
{
    static PolygonToolImpl theTool(PolygonToolImpl::Mode::Edge);
    return theTool;
}

SketchTool& EllipseTool()
{
    static EllipseToolImpl theTool;
    return theTool;
}

SketchTool& SlotTool()
{
    static SlotToolImpl theTool;
    return theTool;
}

SketchTool& SplineTool()
{
    static SplineToolImpl theTool;
    return theTool;
}

SketchTool& ControlPointSplineTool()
{
    static ControlPointSplineToolImpl theTool;
    return theTool;
}

SketchTool& ConicTool()
{
    static ConicToolImpl theTool;
    return theTool;
}

SketchTool& PointTool()
{
    static PointToolImpl theTool;
    return theTool;
}

void StopActiveSketchTool()
{
    if (theRunningTool != nullptr) {
        theRunningTool->Stop();
    }
}

SketchTool* ActiveSketchTool()
{
    return theRunningTool;
}

} // namespace lcad
