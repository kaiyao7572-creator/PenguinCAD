#include "sketch/SketchModifyTools.h"

#include "core/Document.h"
#include "sketch/SketchAnnotations.h"
#include "sketch/SketchDialogs.h"
#include "sketch/SketchDisplay.h"
#include "sketch/SketchGeometry.h"
#include "sketch/SketchSelection.h"

#include <gp_Vec2d.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include <QString>

namespace lcad {

namespace {

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

// Pixels of slop around a click, matching the select tool's feel.
constexpr double kPickPixels = 9.0;

constexpr double kTolerance = SketchGeometry::kTolerance;

gp_Vec2d Perpendicular(const gp_Vec2d& theVector)
{
    return gp_Vec2d(-theVector.Y(), theVector.X());
}

// Nearest curve under a point, ignoring the characteristic-point bias the
// select tool applies -- a modify tool always means the curve itself.
int CurveAt(const SketchFeature& theSketch,
            const gp_Pnt2d&      thePoint,
            double               theTolerance,
            double&              theParam)
{
    int best = 0;
    double bestDistance = theTolerance;

    for (const SketchEntity& entity : theSketch.Entities()) {
        if (entity.id == 0 || !entity.IsCurve() || entity.IsDegenerate()) {
            continue;
        }
        double param = 0.0;
        const double distance = SketchGeometry::DistanceTo(entity, thePoint, param);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = entity.id;
            theParam = param;
        }
    }
    return best;
}

// Every constraint touching an entity, by id. Trimming or extending moves
// the endpoints a constraint was pinning, so they have to go with it --
// otherwise the solver drags the geometry straight back.
void DropConstraintsOn(SketchFeature& theSketch, int theEntity)
{
    std::vector<int> doomed;
    for (const SketchConstraint& constraint : theSketch.Constraints()) {
        if (constraint.a.entity == theEntity || constraint.b.entity == theEntity
         || constraint.c.entity == theEntity) {
            doomed.push_back(constraint.id);
        }
    }
    for (const int id : doomed) {
        theSketch.RemoveConstraint(id);
    }
}

// Constraints joining exactly these two entities, of one type.
void DropConstraintsBetween(SketchFeature&       theSketch,
                            int                  theFirst,
                            int                  theSecond,
                            SketchConstraintType theType)
{
    std::vector<int> doomed;
    for (const SketchConstraint& constraint : theSketch.Constraints()) {
        if (constraint.type != theType) {
            continue;
        }
        const bool joins = (constraint.a.entity == theFirst && constraint.b.entity == theSecond)
                        || (constraint.a.entity == theSecond && constraint.b.entity == theFirst);
        if (joins) {
            doomed.push_back(constraint.id);
        }
    }
    for (const int id : doomed) {
        theSketch.RemoveConstraint(id);
    }
}

bool SamePoint(const gp_Pnt2d& theFirst, const gp_Pnt2d& theSecond)
{
    return theFirst.SquareDistance(theSecond) <= kTolerance * kTolerance;
}

// Walk the shared endpoints out from one entity to find the whole run of
// connected curves -- what Offset treats as a single chain.
std::vector<int> ConnectedChain(const SketchFeature& theSketch, int theSeed)
{
    std::vector<int> chain;
    const SketchEntity* seed = theSketch.FindEntity(theSeed);
    if (seed == nullptr || !seed->IsCurve()) {
        return chain;
    }
    chain.push_back(theSeed);
    if (seed->IsSelfClosed()) {
        return chain;  // a circle is a chain all by itself
    }

    std::vector<int> used{theSeed};
    auto grow = [&](gp_Pnt2d theFrontier, bool theAppend) {
        for (;;) {
            const SketchEntity* next = nullptr;
            for (const SketchEntity& candidate : theSketch.Entities()) {
                if (candidate.id == 0 || !candidate.IsCurve() || candidate.IsSelfClosed()
                 || candidate.IsDegenerate()) {
                    continue;
                }
                if (std::find(used.begin(), used.end(), candidate.id) != used.end()) {
                    continue;
                }
                if (SamePoint(candidate.StartPoint(), theFrontier)
                 || SamePoint(candidate.EndPoint(), theFrontier)) {
                    next = &candidate;
                    break;
                }
            }
            if (next == nullptr) {
                return;
            }

            used.push_back(next->id);
            if (theAppend) {
                chain.push_back(next->id);
            } else {
                chain.insert(chain.begin(), next->id);
            }
            theFrontier = SamePoint(next->StartPoint(), theFrontier) ? next->EndPoint()
                                                                    : next->StartPoint();
        }
    };

    grow(seed->EndPoint(), true);
    grow(seed->StartPoint(), false);
    return chain;
}

// Representative centre of a set of entities, used as the anchor a
// pattern grows from.
gp_Pnt2d CentroidOf(const SketchFeature& theSketch, const std::vector<int>& theIds)
{
    double x = 0.0, y = 0.0;
    int count = 0;
    for (const int id : theIds) {
        const SketchEntity* entity = theSketch.FindEntity(id);
        if (entity == nullptr) {
            continue;
        }
        const gp_Pnt2d centre = entity->CentrePoint();
        x += centre.X();
        y += centre.Y();
        ++count;
    }
    if (count == 0) {
        return gp_Pnt2d(0.0, 0.0);
    }
    return gp_Pnt2d(x / count, y / count);
}

std::vector<SketchEntity> EntitiesOf(const SketchFeature& theSketch,
                                     const std::vector<int>& theIds)
{
    std::vector<SketchEntity> entities;
    entities.reserve(theIds.size());
    for (const int id : theIds) {
        if (const SketchEntity* entity = theSketch.FindEntity(id)) {
            entities.push_back(*entity);
        }
    }
    return entities;
}

// ---- fillet ----

class FilletToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        switch (myStage) {
            case Stage::First:  return "Fillet: click the first line. Esc exits the tool.";
            case Stage::Second: return "Fillet: click the second line.";
            case Stage::Radius: return "Fillet: move to size the arc, then click. Esc cancels.";
        }
        return "Fillet";
    }

protected:
    void Reset() override { myStage = Stage::First; }
    bool IsCollecting() const override { return myStage != Stage::First; }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }

        switch (myStage) {
            case Stage::First: {
                const int picked = PickLine(*sketch, thePoint);
                if (picked == 0) {
                    return;
                }
                myFirstId = picked;
                myNearFirst = thePoint;
                myStage = Stage::Second;
                return;
            }

            case Stage::Second: {
                const int picked = PickLine(*sketch, thePoint);
                if (picked == 0 || picked == myFirstId) {
                    return;
                }
                mySecondId = picked;
                myNearSecond = thePoint;
                myStage = Stage::Radius;
                return;
            }

            case Stage::Radius: {
                SketchGeometry::FilletResult fillet;
                if (!Build(*sketch, thePoint, fillet)) {
                    return;
                }
                Apply(*sketch, fillet);
                myStage = Stage::First;
                return;
            }
        }
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }

        if (myStage != Stage::Radius) {
            const int picked = PickLine(*sketch, thePoint);
            const SketchEntity* entity = picked != 0 ? sketch->FindEntity(picked) : nullptr;
            if (entity != nullptr) {
                ShowPreview({*entity});
            } else {
                ClearPreview();
            }
            return;
        }

        SketchGeometry::FilletResult fillet;
        if (Build(*sketch, thePoint, fillet)) {
            ShowPreview({fillet.arc, fillet.firstTrimmed, fillet.secondTrimmed});
        }
    }

private:
    int PickLine(const SketchFeature& theSketch, const gp_Pnt2d& thePoint) const
    {
        double param = 0.0;
        const int picked = CurveAt(theSketch, thePoint, Tolerance(), param);
        const SketchEntity* entity = picked != 0 ? theSketch.FindEntity(picked) : nullptr;
        // Only straight lines: rounding between two arcs needs a different
        // construction, and Fusion's own sketch fillet is line-to-line.
        if (entity == nullptr || entity->kind != SketchEntity::Kind::Line) {
            return 0;
        }
        return picked;
    }

    double Tolerance() const { return std::max(PixelSize() * kPickPixels, 1.0e-4); }

    // Radius from the cursor: how far along the corner's bisector the
    // cursor sits is exactly how big an arc fits there.
    bool Build(const SketchFeature&          theSketch,
               const gp_Pnt2d&               theCursor,
               SketchGeometry::FilletResult& theResult) const
    {
        const SketchEntity* first = theSketch.FindEntity(myFirstId);
        const SketchEntity* second = theSketch.FindEntity(mySecondId);
        if (first == nullptr || second == nullptr) {
            return false;
        }

        const std::vector<SketchGeometry::Intersection> hits =
            SketchGeometry::Intersect(*first, *second, true);
        if (hits.empty()) {
            return false;
        }

        const gp_Pnt2d corner = hits.front().point;
        gp_Vec2d toFirst(myNearFirst.X() - corner.X(), myNearFirst.Y() - corner.Y());
        gp_Vec2d toSecond(myNearSecond.X() - corner.X(), myNearSecond.Y() - corner.Y());
        if (toFirst.SquareMagnitude() <= kTolerance || toSecond.SquareMagnitude() <= kTolerance) {
            return false;
        }
        toFirst.Normalize();
        toSecond.Normalize();

        gp_Vec2d bisector = toFirst + toSecond;
        if (bisector.SquareMagnitude() <= kTolerance) {
            return false;
        }
        bisector.Normalize();

        const double cosine = std::min(std::max(toFirst.Dot(toSecond), -1.0), 1.0);
        const double half = std::acos(cosine) * 0.5;
        if (half <= kTolerance || half >= kPi * 0.5 - kTolerance) {
            return false;
        }

        const gp_Vec2d toCursor(theCursor.X() - corner.X(), theCursor.Y() - corner.Y());
        const double along = std::max(toCursor.Dot(bisector), 0.0);
        const double maximum =
            SketchGeometry::MaximumFilletRadius(*first, *second, myNearFirst, myNearSecond);
        double radius = along * std::sin(half);
        if (maximum > 0.0) {
            radius = std::min(radius, maximum);
        }
        if (radius <= kTolerance) {
            return false;
        }

        return SketchGeometry::FilletLines(*first, *second, myNearFirst, myNearSecond, radius,
                                           theResult);
    }

    void Apply(SketchFeature& theSketch, const SketchGeometry::FilletResult& theFillet)
    {
        if (!BeginEdit()) {
            return;
        }

        // The two lines no longer meet, so whatever pinned them together
        // at the old corner has to go before the arc takes its place.
        DropConstraintsBetween(theSketch, myFirstId, mySecondId,
                               SketchConstraintType::Coincident);

        theSketch.ReplaceEntity(myFirstId, theFillet.firstTrimmed);
        theSketch.ReplaceEntity(mySecondId, theFillet.secondTrimmed);
        const int arcId = theSketch.AddEntity(theFillet.arc);

        Join(theSketch, myFirstId, arcId, theFillet.firstTrimmed, theFillet.arc);
        Join(theSketch, mySecondId, arcId, theFillet.secondTrimmed, theFillet.arc);

        EndEdit();
    }

    // Pin the trimmed line to whichever end of the arc it now meets, and
    // hold the two tangent while anything else moves.
    static void Join(SketchFeature&      theSketch,
                     int                 theLineId,
                     int                 theArcId,
                     const SketchEntity& theLine,
                     const SketchEntity& theArc)
    {
        const gp_Pnt2d arcStart = theArc.StartPoint();
        const gp_Pnt2d arcEnd = theArc.EndPoint();

        const double toStart = std::min(theLine.first.SquareDistance(arcStart),
                                        theLine.second.SquareDistance(arcStart));
        const double toEnd = std::min(theLine.first.SquareDistance(arcEnd),
                                      theLine.second.SquareDistance(arcEnd));

        const SketchPointRole arcRole =
            toStart <= toEnd ? SketchPointRole::Start : SketchPointRole::End;
        const gp_Pnt2d arcPoint = arcRole == SketchPointRole::Start ? arcStart : arcEnd;
        const SketchPointRole lineRole =
            theLine.first.SquareDistance(arcPoint) <= theLine.second.SquareDistance(arcPoint)
                ? SketchPointRole::Start
                : SketchPointRole::End;

        SketchConstraint coincident;
        coincident.type = SketchConstraintType::Coincident;
        coincident.a = SketchPointRef{theLineId, lineRole};
        coincident.b = SketchPointRef{theArcId, arcRole};
        theSketch.AddConstraint(coincident);

        SketchConstraint tangent;
        tangent.type = SketchConstraintType::Tangent;
        tangent.a = SketchPointRef{theLineId, SketchPointRole::Whole};
        tangent.b = SketchPointRef{theArcId, SketchPointRole::Whole};
        theSketch.AddConstraint(tangent);
    }

    enum class Stage { First, Second, Radius };

    Stage    myStage = Stage::First;
    int      myFirstId = 0;
    int      mySecondId = 0;
    gp_Pnt2d myNearFirst;
    gp_Pnt2d myNearSecond;
};

// ---- trim ----

class TrimToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        return "Trim: click the piece of a curve to remove. Esc exits the tool.";
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

        int id = 0;
        double from = 0.0, to = 0.0;
        if (!Locate(*sketch, thePoint, id, from, to)) {
            return;
        }

        const SketchEntity* entity = sketch->FindEntity(id);
        if (entity == nullptr) {
            return;
        }
        const std::vector<SketchEntity> pieces =
            SketchGeometry::RemoveRange(*entity, from, to);

        if (!BeginEdit()) {
            return;
        }
        // The endpoints a constraint was holding have just moved, so the
        // constraints go with the piece that was cut out.
        DropConstraintsOn(*sketch, id);

        if (pieces.empty()) {
            sketch->RemoveEntity(id);
        } else {
            sketch->ReplaceEntity(id, pieces.front());
            for (std::size_t i = 1; i < pieces.size(); ++i) {
                sketch->AddEntity(pieces[i]);
            }
        }
        SketchSelection::Instance().Clear();
        EndEdit();
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }

        int id = 0;
        double from = 0.0, to = 0.0;
        if (!Locate(*sketch, thePoint, id, from, to)) {
            ClearPreview();
            return;
        }

        // Preview what would go, not what would stay -- the same red
        // highlight Fusion puts under the cursor.
        SketchEntity doomed = *sketch->FindEntity(id);
        if (SketchGeometry::SetParamWindow(doomed, from, to)) {
            ShowPreview({doomed});
        } else {
            ClearPreview();
        }
    }

private:
    bool Locate(const SketchFeature& theSketch,
                const gp_Pnt2d&      thePoint,
                int&                 theId,
                double&              theFrom,
                double&              theTo) const
    {
        double param = 0.0;
        theId = CurveAt(theSketch, thePoint, std::max(PixelSize() * kPickPixels, 1.0e-4), param);
        if (theId == 0) {
            return false;
        }
        const SketchEntity* entity = theSketch.FindEntity(theId);
        return entity != nullptr
            && SketchGeometry::PieceBetweenCrossings(*entity, theSketch.Entities(), param,
                                                     theFrom, theTo);
    }
};

// ---- extend ----

class ExtendToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        return "Extend: click near the end of a curve to run it on. Esc exits the tool.";
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

        int id = 0;
        SketchEntity extended;
        if (!Build(*sketch, thePoint, id, extended)) {
            return;
        }

        if (!BeginEdit()) {
            return;
        }
        DropConstraintsOn(*sketch, id);
        sketch->ReplaceEntity(id, extended);
        EndEdit();
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }

        int id = 0;
        SketchEntity extended;
        if (Build(*sketch, thePoint, id, extended)) {
            ShowPreview({extended});
        } else {
            ClearPreview();
        }
    }

private:
    bool Build(const SketchFeature& theSketch,
               const gp_Pnt2d&      thePoint,
               int&                 theId,
               SketchEntity&        theResult) const
    {
        double param = 0.0;
        theId = CurveAt(theSketch, thePoint, std::max(PixelSize() * kPickPixels, 1.0e-4), param);
        if (theId == 0) {
            return false;
        }

        const SketchEntity* entity = theSketch.FindEntity(theId);
        if (entity == nullptr || entity->IsSelfClosed()) {
            return false;  // a closed curve has no end to extend
        }

        double first = 0.0, last = 0.0;
        if (!SketchGeometry::ParamRange(*entity, first, last)) {
            return false;
        }

        // Which end the cursor is nearer decides which way the curve runs
        // on, which works for a straight line and an arc alike.
        const bool atEnd = thePoint.SquareDistance(entity->EndPoint())
                        <= thePoint.SquareDistance(entity->StartPoint());
        const double period = (entity->kind == SketchEntity::Kind::Line
                            || entity->kind == SketchEntity::Kind::Spline)
                                  ? 0.0
                                  : kTwoPi;

        double best = 0.0;
        bool found = false;
        for (const SketchEntity& other : theSketch.Entities()) {
            if (other.id == theId || other.id == 0 || !other.IsCurve() || other.IsDegenerate()) {
                continue;
            }
            for (const SketchGeometry::Intersection& hit :
                 SketchGeometry::Intersect(*entity, other, true)) {
                double candidate = hit.paramA;
                if (period > 0.0) {
                    // Wrap the crossing round to the far side of the end
                    // being extended, so the nearest one ahead wins.
                    if (atEnd) {
                        while (candidate <= last + kTolerance) {
                            candidate += period;
                        }
                        while (candidate > last + period) {
                            candidate -= period;
                        }
                    } else {
                        while (candidate >= first - kTolerance) {
                            candidate -= period;
                        }
                        while (candidate < first - period) {
                            candidate += period;
                        }
                    }
                } else if (atEnd ? candidate <= last + kTolerance
                                 : candidate >= first - kTolerance) {
                    continue;
                }

                if (!found || (atEnd ? candidate < best : candidate > best)) {
                    best = candidate;
                    found = true;
                }
            }
        }

        if (!found) {
            return false;
        }

        theResult = *entity;
        return atEnd ? SketchGeometry::SetParamWindow(theResult, first, best)
                     : SketchGeometry::SetParamWindow(theResult, best, last);
    }
};

// ---- offset ----

class OffsetToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        return myChain.empty() ? "Offset: click a curve. Esc exits the tool."
                               : "Offset: move to set the distance and side, then click.";
    }

protected:
    void Reset() override
    {
        myChain.clear();
        mySeed = 0;
    }
    bool IsCollecting() const override { return !myChain.empty(); }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }

        if (myChain.empty()) {
            double param = 0.0;
            mySeed = CurveAt(*sketch, thePoint,
                             std::max(PixelSize() * kPickPixels, 1.0e-4), param);
            if (mySeed == 0) {
                return;
            }
            myChain = ConnectedChain(*sketch, mySeed);
            return;
        }

        const std::vector<SketchEntity> offsets = Build(*sketch, thePoint);
        if (offsets.empty()) {
            return;
        }

        // Offsets are new curves rather than edits to old ones, so Commit
        // handles the undo snapshot and the rebuild.
        Commit(offsets);
        Reset();
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }

        if (myChain.empty()) {
            double param = 0.0;
            const int picked =
                CurveAt(*sketch, thePoint, std::max(PixelSize() * kPickPixels, 1.0e-4), param);
            if (picked == 0) {
                ClearPreview();
                return;
            }
            ShowPreview(EntitiesOf(*sketch, ConnectedChain(*sketch, picked)));
            return;
        }

        const std::vector<SketchEntity> offsets = Build(*sketch, thePoint);
        if (offsets.empty()) {
            ClearPreview();
        } else {
            ShowPreview(offsets);
        }
    }

private:
    // Signed distance from the cursor to the curve that was clicked, which
    // is both how far to offset and which side to go.
    double DistanceAt(const SketchFeature& theSketch, const gp_Pnt2d& thePoint) const
    {
        const SketchEntity* seed = theSketch.FindEntity(mySeed);
        if (seed == nullptr) {
            return 0.0;
        }
        double param = 0.0;
        if (!SketchGeometry::NearestParam(*seed, thePoint, param, true)) {
            return 0.0;
        }

        gp_Vec2d tangent = SketchGeometry::TangentAt(*seed, param);
        if (tangent.SquareMagnitude() <= kTolerance) {
            return 0.0;
        }
        tangent.Normalize();

        const gp_Pnt2d on = SketchGeometry::PointAt(*seed, param);
        const gp_Vec2d normal = Perpendicular(tangent);
        return normal.X() * (thePoint.X() - on.X()) + normal.Y() * (thePoint.Y() - on.Y());
    }

    std::vector<SketchEntity> Build(const SketchFeature& theSketch,
                                    const gp_Pnt2d&      thePoint) const
    {
        const double distance = DistanceAt(theSketch, thePoint);
        if (std::fabs(distance) <= kTolerance) {
            return {};
        }

        std::vector<SketchEntity> source = EntitiesOf(theSketch, myChain);
        std::vector<SketchEntity> offsets;
        offsets.reserve(source.size());
        for (const SketchEntity& entity : source) {
            SketchEntity offset;
            if (SketchGeometry::Offset(entity, distance, offset) && !offset.IsDegenerate()) {
                offsets.push_back(offset);
            }
        }

        if (offsets.size() == source.size()) {
            Reconnect(source, offsets);
        }
        return offsets;
    }

    // Offsetting each piece on its own leaves gaps at the corners; pulling
    // neighbours back to where their offsets cross closes them, which is
    // what makes an offset chain still a chain.
    static void Reconnect(const std::vector<SketchEntity>& theSource,
                          std::vector<SketchEntity>&       theOffsets)
    {
        for (std::size_t i = 0; i + 1 < theOffsets.size(); ++i) {
            const gp_Pnt2d corner = theSource[i].EndPoint();
            if (!SamePoint(corner, theSource[i + 1].StartPoint())) {
                continue;
            }

            const std::vector<SketchGeometry::Intersection> hits =
                SketchGeometry::Intersect(theOffsets[i], theOffsets[i + 1], true);
            if (hits.empty()) {
                continue;
            }

            const SketchGeometry::Intersection* best = &hits.front();
            for (const SketchGeometry::Intersection& hit : hits) {
                if (hit.point.SquareDistance(corner) < best->point.SquareDistance(corner)) {
                    best = &hit;
                }
            }

            // Only the outer end of each neighbour is kept; the inner ends
            // both move to the crossing.
            double firstLow = 0.0, secondHigh = 0.0, ignored = 0.0;
            if (!SketchGeometry::ParamRange(theOffsets[i], firstLow, ignored)
             || !SketchGeometry::ParamRange(theOffsets[i + 1], ignored, secondHigh)) {
                continue;
            }

            SketchEntity trimmedFirst = theOffsets[i];
            if (SketchGeometry::SetParamWindow(trimmedFirst, firstLow, best->paramA)) {
                theOffsets[i] = trimmedFirst;
            }
            SketchEntity trimmedSecond = theOffsets[i + 1];
            if (SketchGeometry::SetParamWindow(trimmedSecond, best->paramB, secondHigh)) {
                theOffsets[i + 1] = trimmedSecond;
            }
        }
    }

    std::vector<int> myChain;
    int              mySeed = 0;
};

// ---- mirror ----

// Mirror and the two patterns all work on whatever is selected. The
// selection is read fresh every time rather than captured when the tool
// starts, so the user can re-pick without leaving the tool.
class MirrorToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        return SketchSelection::Instance().IsEmpty()
                   ? "Mirror: select the geometry to mirror first. Esc exits the tool."
                   : "Mirror: click the line to mirror about. Esc exits the tool.";
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
        const std::vector<SketchEntity> copies = Build(*sketch, thePoint);
        if (copies.empty()) {
            return;
        }
        Commit(copies);
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }
        const std::vector<SketchEntity> copies = Build(*sketch, thePoint);
        if (copies.empty()) {
            ClearPreview();
        } else {
            ShowPreview(copies);
        }
    }

private:
    std::vector<SketchEntity> Build(const SketchFeature& theSketch,
                                    const gp_Pnt2d&      thePoint) const
    {
        double param = 0.0;
        const int axisId =
            CurveAt(theSketch, thePoint, std::max(PixelSize() * kPickPixels, 1.0e-4), param);
        const SketchEntity* axis = axisId != 0 ? theSketch.FindEntity(axisId) : nullptr;
        if (axis == nullptr || axis->kind != SketchEntity::Kind::Line) {
            return {};
        }

        std::vector<SketchEntity> copies;
        for (const int id : SketchSelection::Instance().EntityIds()) {
            if (id == axisId) {
                continue;  // mirroring the axis onto itself achieves nothing
            }
            const SketchEntity* entity = theSketch.FindEntity(id);
            if (entity == nullptr) {
                continue;
            }
            SketchEntity copy = SketchGeometry::Mirrored(*entity, axis->first, axis->second);
            if (!copy.IsDegenerate()) {
                copies.push_back(copy);
            }
        }
        return copies;
    }
};

// ---- patterns ----

class RectangularPatternImpl : public SketchRectangularPatternTool
{
public:
    std::string Hint() const override
    {
        return SketchSelection::Instance().IsEmpty()
                   ? "Rectangular Pattern: select the geometry to copy first. Esc exits."
                   : "Rectangular Pattern: move to set the spacing, then click. Esc exits.";
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
        const std::vector<SketchEntity> copies = Build(*sketch, thePoint);
        if (copies.empty()) {
            return;
        }
        Commit(copies);
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }
        const std::vector<SketchEntity> copies = Build(*sketch, thePoint);
        if (copies.empty()) {
            ClearPreview();
        } else {
            ShowPreview(copies);
        }
    }

private:
    std::vector<SketchEntity> Build(const SketchFeature& theSketch,
                                    const gp_Pnt2d&      thePoint) const
    {
        const std::vector<int> ids = SketchSelection::Instance().EntityIds();
        if (ids.empty()) {
            return {};
        }

        // The cursor's offset from the selection is one step of the grid,
        // so dragging further spreads the copies out.
        const gp_Pnt2d anchor = CentroidOf(theSketch, ids);
        const gp_Vec2d step(thePoint.X() - anchor.X(), thePoint.Y() - anchor.Y());
        if (step.SquareMagnitude() <= kTolerance) {
            return {};
        }

        const std::vector<SketchEntity> source = EntitiesOf(theSketch, ids);
        std::vector<SketchEntity> copies;
        for (int across = 0; across < myAcross; ++across) {
            for (int down = 0; down < myDown; ++down) {
                if (across == 0 && down == 0) {
                    continue;  // the original is already there
                }
                const gp_Vec2d offset(step.X() * across, step.Y() * down);
                for (const SketchEntity& entity : source) {
                    SketchEntity copy = SketchGeometry::Translated(entity, offset);
                    copy.id = 0;
                    copies.push_back(copy);
                }
            }
        }
        return copies;
    }
};

class CircularPatternImpl : public SketchCircularPatternTool
{
public:
    std::string Hint() const override
    {
        return SketchSelection::Instance().IsEmpty()
                   ? "Circular Pattern: select the geometry to copy first. Esc exits."
                   : "Circular Pattern: click the centre to rotate about. Esc exits.";
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
        const std::vector<SketchEntity> copies = Build(*sketch, thePoint);
        if (copies.empty()) {
            return;
        }
        Commit(copies);
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }
        const std::vector<SketchEntity> copies = Build(*sketch, thePoint);
        if (copies.empty()) {
            ClearPreview();
        } else {
            ShowPreview(copies);
        }
    }

private:
    std::vector<SketchEntity> Build(const SketchFeature& theSketch,
                                    const gp_Pnt2d&      theCentre) const
    {
        const std::vector<int> ids = SketchSelection::Instance().EntityIds();
        if (ids.empty() || myCount < 2) {
            return {};
        }

        // A full turn divides evenly among all the copies; a partial one
        // puts the last copy on the far edge of the span.
        const bool fullTurn = std::fabs(std::fabs(myTotalAngle) - kTwoPi) <= 1.0e-6;
        const double step = fullTurn ? myTotalAngle / myCount : myTotalAngle / (myCount - 1);

        const std::vector<SketchEntity> source = EntitiesOf(theSketch, ids);
        std::vector<SketchEntity> copies;
        for (int i = 1; i < myCount; ++i) {
            for (const SketchEntity& entity : source) {
                SketchEntity copy = SketchGeometry::Rotated(entity, theCentre, step * i);
                copy.id = 0;
                copies.push_back(copy);
            }
        }
        return copies;
    }
};

// ---- dimension ----

// Fusion has one dimension command whose meaning comes from what is
// picked. This works the same way: collect up to two picks, decide what
// they can measure, then place the number.
class DimensionToolImpl : public SketchTool
{
public:
    std::string Hint() const override
    {
        if (myPlacing) {
            return "Dimension: move the text into place and click. Esc cancels.";
        }
        if (myPicks.empty()) {
            return "Dimension: click a line, a circle, or a point. Esc exits the tool.";
        }
        return "Dimension: click a second entity, or click empty space to dimension just this one.";
    }

protected:
    void Reset() override
    {
        myPicks.clear();
        myPlacing = false;
    }
    bool IsCollecting() const override { return myPlacing || !myPicks.empty(); }

    void OnPoint(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }

        if (myPlacing) {
            Place(*sketch, thePoint);
            return;
        }

        SketchPointRef picked;
        const double tolerance = std::max(PixelSize() * kPickPixels, 1.0e-4);
        if (!PickSketchEntity(*sketch, thePoint, tolerance, picked)) {
            // Clicking nothing means "that's all I'm measuring", which is
            // how a single line becomes a length dimension.
            if (!myPicks.empty()) {
                myPlacing = Resolve(*sketch);
            }
            return;
        }

        if (std::find(myPicks.begin(), myPicks.end(), picked) == myPicks.end()) {
            myPicks.push_back(picked);
        }

        // One circle or arc is already a complete dimension; anything else
        // may want a second pick, but two is always enough.
        if (myPicks.size() >= 2 || IsRadial(*sketch, myPicks.front())) {
            myPlacing = Resolve(*sketch);
        }
    }

    void OnHover(const gp_Pnt2d& thePoint) override
    {
        SketchFeature* sketch = Sketch();
        if (sketch == nullptr) {
            return;
        }

        if (!myPlacing) {
            SketchPointRef hovered;
            const double tolerance = std::max(PixelSize() * kPickPixels, 1.0e-4);
            const SketchEntity* entity =
                PickSketchEntity(*sketch, thePoint, tolerance, hovered)
                    ? sketch->FindEntity(hovered.entity)
                    : nullptr;
            if (entity != nullptr && entity->IsCurve()) {
                ShowPreview({*entity});
            } else {
                ClearPreview();
            }
            return;
        }

        myPending.labelPosition = thePoint;
        ShowPreview(PreviewGeometry(*sketch));
    }

private:
    static bool IsRadial(const SketchFeature& theSketch, const SketchPointRef& theRef)
    {
        const SketchEntity* entity = theSketch.FindEntity(theRef.entity);
        if (entity == nullptr || theRef.role != SketchPointRole::Whole) {
            return false;
        }
        return entity->kind == SketchEntity::Kind::Circle
            || entity->kind == SketchEntity::Kind::Arc;
    }

    // Work out what the picks can measure. Returns false when they can't
    // measure anything, which leaves the tool collecting.
    bool Resolve(const SketchFeature& theSketch)
    {
        myPending = SketchConstraint();
        if (myPicks.empty()) {
            return false;
        }

        const SketchEntity* first = theSketch.FindEntity(myPicks.front().entity);
        if (first == nullptr) {
            myPicks.clear();
            return false;
        }

        if (myPicks.size() == 1) {
            if (myPicks.front().role == SketchPointRole::Whole) {
                if (first->kind == SketchEntity::Kind::Circle) {
                    myPending.type = SketchConstraintType::Diameter;
                    myPending.a = myPicks.front();
                    return Measure(theSketch);
                }
                if (first->kind == SketchEntity::Kind::Arc) {
                    myPending.type = SketchConstraintType::Radius;
                    myPending.a = myPicks.front();
                    return Measure(theSketch);
                }
                if (first->kind == SketchEntity::Kind::Line) {
                    // A line on its own means its length: the distance
                    // between the two ends it already has.
                    myPending.type = SketchConstraintType::Distance;
                    myPending.a = SketchPointRef{first->id, SketchPointRole::Start};
                    myPending.b = SketchPointRef{first->id, SketchPointRole::End};
                    return Measure(theSketch);
                }
            }
            myPicks.clear();
            return false;
        }

        const SketchEntity* second = theSketch.FindEntity(myPicks[1].entity);
        if (second == nullptr) {
            myPicks.clear();
            return false;
        }

        const bool bothLines = myPicks[0].role == SketchPointRole::Whole
                            && myPicks[1].role == SketchPointRole::Whole
                            && first->kind == SketchEntity::Kind::Line
                            && second->kind == SketchEntity::Kind::Line;
        if (bothLines) {
            myPending.type = SketchConstraintType::Angle;
            myPending.a = myPicks[0];
            myPending.b = myPicks[1];
            return Measure(theSketch);
        }

        myPending.type = SketchConstraintType::Distance;
        myPending.a = myPicks[0];
        myPending.b = myPicks[1];
        return Measure(theSketch);
    }

    bool Measure(const SketchFeature& theSketch)
    {
        double value = 0.0;
        if (!SketchSolver::MeasureDimension(theSketch.Entities(), myPending, value)) {
            myPicks.clear();
            return false;
        }
        myPending.value = value;
        return true;
    }

    // Preview through a throwaway copy of the sketch carrying only the
    // dimension being placed, so the annotation code that draws finished
    // dimensions draws this one too.
    std::vector<SketchEntity> PreviewGeometry(const SketchFeature& theSketch) const
    {
        std::unique_ptr<Feature> clone = theSketch.Clone();
        SketchFeature* scratch = dynamic_cast<SketchFeature*>(clone.get());
        if (scratch == nullptr) {
            return {};
        }
        scratch->ClearConstraints();
        scratch->AddConstraint(myPending);

        double scale = PixelSize() * 11.0;
        if (scale <= 0.0) {
            scale = 1.0;
        }
        return SketchAnnotations::DimensionGeometry(*scratch, scale);
    }

    void Place(SketchFeature& theSketch, const gp_Pnt2d& thePoint)
    {
        myPending.labelPosition = thePoint;

        // Fusion drops an edit box on the canvas here; a small modal does
        // the same job without a canvas widget layer. It opens on the
        // measured value, so accepting it changes nothing and cancelling
        // abandons the dimension altogether.
        double shown = myPending.IsAngular() ? myPending.value * 180.0 / kPi : myPending.value;
        const QString title =
            QString::fromLatin1(SketchConstraint::TypeName(myPending.type)) + " Dimension";
        if (!SketchDialogs::AskDimensionValue(myContext.parent, title, myPending.IsAngular(),
                                              shown)) {
            ClearPreview();
            Reset();
            return;
        }
        myPending.value = myPending.IsAngular() ? shown * kPi / 180.0 : shown;

        if (!BeginEdit()) {
            Reset();
            return;
        }
        const int added = theSketch.AddConstraint(myPending);
        SketchSelection::Instance().Clear();
        Reset();
        EndEdit();

        // A value the geometry can't reach leaves the sketch stuck, so the
        // dimension comes straight back out rather than staying broken.
        if (!theSketch.LastError().empty()) {
            theSketch.RemoveConstraint(added);
            EndEdit();
        }
    }

    std::vector<SketchPointRef> myPicks;
    SketchConstraint            myPending;
    bool                        myPlacing = false;
};

} // namespace

// ---- pattern parameter setters ----

void SketchRectangularPatternTool::SetCounts(int theAcross, int theDown)
{
    myAcross = std::min(std::max(theAcross, 1), 512);
    myDown = std::min(std::max(theDown, 1), 512);
}

void SketchCircularPatternTool::SetPattern(int theCount, double theTotalAngle)
{
    myCount = std::min(std::max(theCount, 2), 512);
    myTotalAngle = theTotalAngle;
}

// ---- accessors ----

SketchTool& SketchFilletTool()
{
    static FilletToolImpl theTool;
    return theTool;
}

SketchTool& SketchTrimTool()
{
    static TrimToolImpl theTool;
    return theTool;
}

SketchTool& SketchExtendTool()
{
    static ExtendToolImpl theTool;
    return theTool;
}

SketchTool& SketchOffsetTool()
{
    static OffsetToolImpl theTool;
    return theTool;
}

SketchTool& SketchMirrorTool()
{
    static MirrorToolImpl theTool;
    return theTool;
}

SketchRectangularPatternTool& SketchRectangularPattern()
{
    static RectangularPatternImpl theTool;
    return theTool;
}

SketchCircularPatternTool& SketchCircularPattern()
{
    static CircularPatternImpl theTool;
    return theTool;
}

SketchTool& SketchDimensionTool()
{
    static DimensionToolImpl theTool;
    return theTool;
}

} // namespace lcad
