#pragma once

#include "core/Command.h"
#include "core/ViewportInteraction.h"
#include "sketch/SketchFeature.h"
#include "sketch/SketchInput.h"

#include <gp_Pnt2d.hxx>

#include <string>
#include <vector>

namespace lcad {

// Which sketch the drawing tools draw into.
//
// The sketch is remembered by NAME, never by pointer: undo/redo rebuilds
// the timeline out of clones, so a stored Feature* would be pointing at a
// discarded copy the moment the user hits Ctrl+Z.
class SketchSession
{
public:
    static SketchSession& Instance();

    void Begin(const CommandContext& theContext, const std::string& theSketchName);
    void End(const CommandContext& theContext);

    bool IsActive() const { return !myActiveName.empty(); }
    const std::string& ActiveName() const { return myActiveName; }

    SketchFeature* ActiveSketch(Document* theDocument) const;
    static SketchFeature* Find(Document* theDocument, const std::string& theName);

private:
    SketchSession() = default;

    std::string myActiveName;
};

// Base for the click-to-draw tools. Takes over viewport input, turns
// clicks into points on the sketch plane, and shows a rubber band while
// the mouse moves.
class SketchTool : public ViewportInteraction
{
public:
    // Push onto the viewport, replacing whatever sketch tool was running.
    void Start(const CommandContext& theContext);

    // Pop off the viewport and drop any half-finished input.
    void Stop();

    bool IsRunning() const { return myIsRunning; }

    // One-line hint for the status bar, updated as the tool progresses.
    virtual std::string Hint() const = 0;

    bool OnMousePress(const Graphic3d_Vec2i& thePos,
                      Qt::MouseButton        theButton,
                      Qt::KeyboardModifiers  theModifiers) override;
    bool OnMouseMove(const Graphic3d_Vec2i& thePos,
                     Qt::MouseButtons       theButtons,
                     Qt::KeyboardModifiers  theModifiers) override;
    bool OnMouseRelease(const Graphic3d_Vec2i& thePos,
                        Qt::MouseButton        theButton,
                        Qt::KeyboardModifiers  theModifiers) override;
    bool OnMouseDoubleClick(const Graphic3d_Vec2i& thePos,
                            Qt::MouseButton        theButton,
                            Qt::KeyboardModifiers  theModifiers) override;
    bool OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers) override;
    void OnDeactivated() override;

protected:
    // Drop input collected so far; the tool stays armed for a new curve.
    virtual void Reset() = 0;

    // True while a curve is half-drawn, which is what makes the first
    // Escape cancel the curve instead of ending the tool.
    virtual bool IsCollecting() const = 0;

    virtual void OnPoint(const gp_Pnt2d& thePoint) = 0;
    virtual void OnHover(const gp_Pnt2d& thePoint) = 0;

    // A second click in the same place. Only the tools that need it (the
    // spline's "that's the last point") do anything here.
    virtual void OnDoubleClick(const gp_Pnt2d& thePoint) { (void)thePoint; }

    // Keys other than Escape, which the base class owns. Return true to
    // consume.
    virtual bool OnKey(int theKey, Qt::KeyboardModifiers theModifiers)
    {
        (void)theKey;
        (void)theModifiers;
        return false;
    }

    // Whether the click just handed to OnPoint was this tool's business.
    // A drawing tool always says yes -- every click places a point. The
    // select tool says no when the click hit no sketch geometry, which
    // lets it fall through to the viewport's own picking so a face can
    // still be selected for the next sketch.
    virtual bool ConsumedLastPress() const { return true; }

    // ---- typed input: Fusion's on-canvas value boxes ----

    // The boxes this tool offers at the stage it has reached. Leave
    // theInput empty -- the default -- and the tool keeps exactly the
    // behaviour it had before typed input existed.
    virtual void ArmInput(SketchInput& theInput) { (void)theInput; }

    // Fill the boxes from the cursor. Locked boxes refuse the update
    // themselves, so an override never has to remember the rule.
    virtual void MeasureInput(const gp_Pnt2d& theCursor);

    // Bend a cursor point to whatever the user has typed. The default is
    // a length and an angle measured from the last placed point, which is
    // what a line wants; the box and radial tools override it.
    virtual gp_Pnt2d ApplyInput(const gp_Pnt2d& theCursor) const;

    SketchInput&       Input() { return myInput; }
    const SketchInput& Input() const { return myInput; }

    bool            HasAnchor() const { return myHasAnchor; }
    const gp_Pnt2d& Anchor() const { return myAnchor; }

    // Re-arm the boxes for the stage the tool has just moved to. Called
    // for you after every click and every reset; a tool only needs it
    // when it changes stage some other way.
    void RearmInput();

    // Called after Commit() has put entities into the sketch, with the ids
    // they were given. Tools override it to add the constraints Fusion
    // infers automatically -- the horizontals and coincidences that make a
    // rectangle behave like a rectangle.
    virtual void OnCommitted(SketchFeature& theSketch, const std::vector<int>& theIds)
    {
        (void)theSketch;
        (void)theIds;
    }

    SketchFeature* Sketch() const;

    // Add entities to the sketch and re-evaluate the timeline.
    std::vector<int> Commit(const std::vector<SketchEntity>& theEntities);

    // The two halves of an edit to geometry that already exists: snapshot
    // for undo, change the sketch, then re-evaluate everything downstream.
    // Commit() does both for the create tools; the modify tools, which
    // rewrite entities rather than appending them, call these directly.
    bool BeginEdit();
    void EndEdit();

    void ShowPreview(const std::vector<SketchEntity>& theEntities);
    void ClearPreview();
    void ShowHint();

    // Re-run the preview against the last known cursor position. Tools
    // whose shape depends on more than the cursor -- a polygon's side
    // count -- call this when that other input changes.
    void RefreshPreview();

    // Model-space size of one screen pixel, for the tools that need a
    // zoom-independent feel. Zero when the view isn't up yet.
    double PixelSize() const;

    CommandContext myContext;

private:
    // Screen pixel -> point on the sketch plane, snapped to a nearby
    // entity endpoint when there is one.
    bool PlanePointAt(const Graphic3d_Vec2i& thePos, gp_Pnt2d& theResult) const;

    // Measure, bend and hand on one cursor position. Every path that
    // moves the rubber band goes through here, which is what keeps a
    // typed value in force whether the cursor moved or a digit was typed.
    void DispatchHover(const gp_Pnt2d& theCursor);

    // Float the value boxes beside the cursor.
    void ShowReadout(const gp_Pnt2d& thePoint);

    // Keys belonging to the value boxes. True when consumed.
    bool HandleTypedKey(int theKey, Qt::KeyboardModifiers theModifiers);

    // Enter: place the point the typed values describe, exactly as a
    // click at that spot would have.
    void PlaceTypedPoint();

    SketchInput myInput;

    bool     myIsRunning = false;
    bool     myHasHover  = false;
    gp_Pnt2d myLastHover;

    // The point a new segment is growing from -- the last one the user
    // committed. Hovering locks onto the horizontal/vertical through it,
    // which is the inference that makes drawn lines come out straight.
    bool     myHasAnchor = false;
    gp_Pnt2d myAnchor;
};

// Polygon tools carry a side count the command dialog sets, and which the
// user can nudge with the bracket keys while the rubber band is up.
class SketchPolygonTool : public SketchTool
{
public:
    int Sides() const { return mySides; }
    void SetSides(int theSides);

protected:
    bool OnKey(int theKey, Qt::KeyboardModifiers theModifiers) override;

    int mySides = 6;
};

// The tools themselves. Instances are long-lived: the viewport holds a
// bare pointer to whichever one is active and does not own it.
//
// ---- Create ----
SketchTool& LineTool();
SketchTool& RectangleTool();            // two opposite corners
SketchTool& CentreRectangleTool();
SketchTool& ThreePointRectangleTool();
SketchTool& CircleTool();               // centre + radius
SketchTool& TwoPointCircleTool();       // the two ends of a diameter
SketchTool& ThreePointCircleTool();
SketchTool& ArcTool();                  // centre, start, end
SketchTool& ThreePointArcTool();
SketchTool& TangentArcTool();
SketchPolygonTool& InscribedPolygonTool();
SketchPolygonTool& CircumscribedPolygonTool();
SketchPolygonTool& EdgePolygonTool();
SketchTool& EllipseTool();
SketchTool& SlotTool();
SketchTool& SplineTool();               // interpolates the points clicked
SketchTool& ControlPointSplineTool();   // the points are control poles
SketchTool& ConicTool();                // two ends plus a point on the curve
SketchTool& PointTool();

// Stop whichever drawing tool is running, if any.
void StopActiveSketchTool();

// The tool that owns the viewport right now, or null.
SketchTool* ActiveSketchTool();

} // namespace lcad
