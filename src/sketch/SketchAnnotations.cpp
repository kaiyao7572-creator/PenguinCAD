#include "sketch/SketchAnnotations.h"

#include "sketch/SketchGeometry.h"

#include <gp_Vec2d.hxx>

#include <cmath>
#include <cstdio>
#include <map>

namespace lcad {
namespace SketchAnnotations {

namespace {

constexpr double kPi = 3.14159265358979323846;

using Entities = std::vector<SketchEntity>;

void AddLine(Entities& theOut, const gp_Pnt2d& theFrom, const gp_Pnt2d& theTo)
{
    if (theFrom.SquareDistance(theTo) <= SketchGeometry::kTolerance) {
        return;
    }
    theOut.push_back(SketchEntity::MakeLine(theFrom, theTo));
}

gp_Pnt2d Offset(const gp_Pnt2d& thePoint, double theX, double theY)
{
    return gp_Pnt2d(thePoint.X() + theX, thePoint.Y() + theY);
}

gp_Vec2d Perpendicular(const gp_Vec2d& theVector)
{
    return gp_Vec2d(-theVector.Y(), theVector.X());
}

bool UnitBetween(const gp_Pnt2d& theFrom, const gp_Pnt2d& theTo, gp_Vec2d& theResult)
{
    gp_Vec2d delta(theTo.X() - theFrom.X(), theTo.Y() - theFrom.Y());
    if (delta.SquareMagnitude() <= SketchGeometry::kTolerance) {
        return false;
    }
    delta.Normalize();
    theResult = delta;
    return true;
}

// A pair of barbs pointing back down the dimension line.
void AddArrow(Entities& theOut, const gp_Pnt2d& theTip, const gp_Vec2d& theDirection, double theSize)
{
    const gp_Vec2d back = theDirection * -theSize;
    const gp_Vec2d side = Perpendicular(theDirection) * (theSize * 0.32);
    AddLine(theOut, theTip, theTip.Translated(back + side));
    AddLine(theOut, theTip, theTip.Translated(back - side));
}

// ---- glyphs ----

void GlyphHorizontal(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    AddLine(theOut, Offset(theAt, -theSize * 0.5, 0.0), Offset(theAt, theSize * 0.5, 0.0));
}

void GlyphVertical(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    AddLine(theOut, Offset(theAt, 0.0, -theSize * 0.5), Offset(theAt, 0.0, theSize * 0.5));
}

void GlyphParallel(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    const double half = theSize * 0.5;
    for (int i = 0; i < 2; ++i) {
        const double shift = (i == 0 ? -1.0 : 1.0) * theSize * 0.2;
        AddLine(theOut, Offset(theAt, shift - half * 0.4, -half),
                Offset(theAt, shift + half * 0.4, half));
    }
}

void GlyphPerpendicular(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    const double half = theSize * 0.5;
    AddLine(theOut, Offset(theAt, -half, half), Offset(theAt, -half, -half));
    AddLine(theOut, Offset(theAt, -half, -half), Offset(theAt, half, -half));
}

void GlyphEqual(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    const double half = theSize * 0.5;
    AddLine(theOut, Offset(theAt, -half, theSize * 0.18), Offset(theAt, half, theSize * 0.18));
    AddLine(theOut, Offset(theAt, -half, -theSize * 0.18), Offset(theAt, half, -theSize * 0.18));
}

void GlyphCoincident(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    theOut.push_back(SketchEntity::MakeCircle(theAt, theSize * 0.32));
}

void GlyphConcentric(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    theOut.push_back(SketchEntity::MakeCircle(theAt, theSize * 0.38));
    theOut.push_back(SketchEntity::MakeCircle(theAt, theSize * 0.18));
}

void GlyphTangent(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    const double radius = theSize * 0.28;
    theOut.push_back(SketchEntity::MakeCircle(Offset(theAt, 0.0, radius), radius));
    AddLine(theOut, Offset(theAt, -theSize * 0.5, 0.0), Offset(theAt, theSize * 0.5, 0.0));
}

void GlyphMidpoint(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    const double half = theSize * 0.5;
    AddLine(theOut, Offset(theAt, -half, 0.0), Offset(theAt, half, 0.0));
    AddLine(theOut, Offset(theAt, 0.0, -theSize * 0.3), Offset(theAt, 0.0, theSize * 0.3));
}

void GlyphCollinear(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    const double half = theSize * 0.5;
    AddLine(theOut, Offset(theAt, -half, 0.0), Offset(theAt, -theSize * 0.12, 0.0));
    AddLine(theOut, Offset(theAt, theSize * 0.12, 0.0), Offset(theAt, half, 0.0));
}

void GlyphFix(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    const double half = theSize * 0.4;
    AddLine(theOut, Offset(theAt, -half, -half), Offset(theAt, half, half));
    AddLine(theOut, Offset(theAt, -half, half), Offset(theAt, half, -half));
}

void GlyphSymmetric(Entities& theOut, const gp_Pnt2d& theAt, double theSize)
{
    const double half = theSize * 0.5;
    AddLine(theOut, Offset(theAt, 0.0, -half), Offset(theAt, 0.0, half));
    for (int side = 0; side < 2; ++side) {
        const double sign = side == 0 ? -1.0 : 1.0;
        AddLine(theOut, Offset(theAt, sign * half, half * 0.6),
                Offset(theAt, sign * theSize * 0.18, 0.0));
        AddLine(theOut, Offset(theAt, sign * half, -half * 0.6),
                Offset(theAt, sign * theSize * 0.18, 0.0));
    }
}

void DrawGlyph(Entities& theOut, SketchConstraintType theType, const gp_Pnt2d& theAt, double theSize)
{
    switch (theType) {
        case SketchConstraintType::Coincident:    GlyphCoincident(theOut, theAt, theSize); return;
        case SketchConstraintType::Horizontal:    GlyphHorizontal(theOut, theAt, theSize); return;
        case SketchConstraintType::Vertical:      GlyphVertical(theOut, theAt, theSize); return;
        case SketchConstraintType::Parallel:      GlyphParallel(theOut, theAt, theSize); return;
        case SketchConstraintType::Perpendicular: GlyphPerpendicular(theOut, theAt, theSize); return;
        case SketchConstraintType::Equal:         GlyphEqual(theOut, theAt, theSize); return;
        case SketchConstraintType::Tangent:       GlyphTangent(theOut, theAt, theSize); return;
        case SketchConstraintType::Midpoint:      GlyphMidpoint(theOut, theAt, theSize); return;
        case SketchConstraintType::Concentric:    GlyphConcentric(theOut, theAt, theSize); return;
        case SketchConstraintType::Collinear:     GlyphCollinear(theOut, theAt, theSize); return;
        case SketchConstraintType::Fix:           GlyphFix(theOut, theAt, theSize); return;
        case SketchConstraintType::Symmetric:     GlyphSymmetric(theOut, theAt, theSize); return;

        // Dimensions carry their own witness lines and a number; a glyph
        // on top of that would only be clutter.
        case SketchConstraintType::Distance:
        case SketchConstraintType::DistanceX:
        case SketchConstraintType::DistanceY:
        case SketchConstraintType::Radius:
        case SketchConstraintType::Diameter:
        case SketchConstraintType::Angle:
            return;
    }
}

// Where a constraint's mark belongs: on the point it pins, or beside the
// curve it governs.
bool AnchorOf(const SketchFeature&    theSketch,
              const SketchConstraint& theConstraint,
              gp_Pnt2d&               theResult)
{
    const SketchEntity* entity = theSketch.FindEntity(theConstraint.a.entity);
    if (entity == nullptr) {
        return false;
    }

    switch (theConstraint.a.role) {
        case SketchPointRole::Start:  theResult = entity->StartPoint(); return true;
        case SketchPointRole::End:    theResult = entity->EndPoint(); return true;
        case SketchPointRole::Centre: theResult = entity->CentrePoint(); return true;
        case SketchPointRole::Whole:  break;
    }

    if (entity->kind == SketchEntity::Kind::Circle
     || entity->kind == SketchEntity::Kind::Arc
     || entity->kind == SketchEntity::Kind::Ellipse) {
        // On the rim rather than the centre, where it doesn't collide with
        // a concentric neighbour's mark.
        double first = 0.0, last = 0.0;
        if (SketchGeometry::ParamRange(*entity, first, last)) {
            theResult = SketchGeometry::PointAt(*entity, (first + last) * 0.5);
            return true;
        }
    }

    theResult = entity->CentrePoint();
    return true;
}

// ---- dimensions ----

void LinearDimension(Entities&       theOut,
                     const gp_Pnt2d& theFrom,
                     const gp_Pnt2d& theTo,
                     const gp_Pnt2d& theLabel,
                     double          theScale)
{
    gp_Vec2d along;
    if (!UnitBetween(theFrom, theTo, along)) {
        return;
    }
    const gp_Vec2d normal = Perpendicular(along);

    // The dimension line runs parallel to the measured span, through
    // wherever the user dropped the number.
    const double offset =
        normal.X() * (theLabel.X() - theFrom.X()) + normal.Y() * (theLabel.Y() - theFrom.Y());
    const gp_Pnt2d from = theFrom.Translated(normal * offset);
    const gp_Pnt2d to = theTo.Translated(normal * offset);

    // Witness lines overshoot the dimension line slightly, as drafting
    // convention has them.
    const gp_Vec2d overshoot = normal * (offset >= 0.0 ? theScale * 0.4 : -theScale * 0.4);
    AddLine(theOut, theFrom, from.Translated(overshoot));
    AddLine(theOut, theTo, to.Translated(overshoot));
    AddLine(theOut, from, to);

    AddArrow(theOut, from, along, theScale * 0.8);
    AddArrow(theOut, to, along * -1.0, theScale * 0.8);
}

void RadialDimension(Entities&           theOut,
                     const SketchEntity& theEntity,
                     const gp_Pnt2d&     theLabel,
                     double              theScale,
                     bool                theIsDiameter)
{
    const gp_Pnt2d centre = theEntity.first;
    gp_Vec2d toLabel;
    if (!UnitBetween(centre, theLabel, toLabel)) {
        toLabel = gp_Vec2d(1.0, 0.0);
    }

    const gp_Pnt2d rim = centre.Translated(toLabel * theEntity.radius);
    const gp_Pnt2d tail =
        theIsDiameter ? centre.Translated(toLabel * -theEntity.radius) : centre;

    AddLine(theOut, tail, theLabel);
    AddArrow(theOut, rim, toLabel * -1.0, theScale * 0.8);
    if (theIsDiameter) {
        AddArrow(theOut, tail, toLabel, theScale * 0.8);
    }
}

void AngularDimension(Entities&           theOut,
                      const SketchEntity& theFirst,
                      const SketchEntity& theSecond,
                      const gp_Pnt2d&     theLabel,
                      double              theScale)
{
    const std::vector<SketchGeometry::Intersection> hits =
        SketchGeometry::Intersect(theFirst, theSecond, true);
    if (hits.empty()) {
        return;  // parallel lines have no vertex to sweep an arc about
    }

    const gp_Pnt2d corner = hits.front().point;
    const double radius = corner.Distance(theLabel);
    if (radius <= theScale * 0.5) {
        return;
    }

    auto angleTo = [&corner](const SketchEntity& theEntity) {
        // Measure toward whichever end of the line is further from the
        // corner, so the arc spans the angle actually being called out.
        const gp_Pnt2d far = corner.SquareDistance(theEntity.first)
                                     >= corner.SquareDistance(theEntity.second)
                                 ? theEntity.first
                                 : theEntity.second;
        return std::atan2(far.Y() - corner.Y(), far.X() - corner.X());
    };

    const double from = angleTo(theFirst);
    const double to = angleTo(theSecond);
    const SketchEntity arc = SketchEntity::MakeArc(corner, radius, from, to);
    if (!arc.IsDegenerate()) {
        theOut.push_back(arc);
    }

    AddLine(theOut, corner, SketchGeometry::PointAt(arc, arc.startAngle));
    AddLine(theOut, corner, SketchGeometry::PointAt(arc, arc.endAngle));
}

bool DimensionPoints(const SketchFeature&    theSketch,
                     const SketchConstraint& theConstraint,
                     gp_Pnt2d&               theFrom,
                     gp_Pnt2d&               theTo)
{
    const SketchEntity* first = theSketch.FindEntity(theConstraint.a.entity);
    const SketchEntity* second = theSketch.FindEntity(theConstraint.b.entity);
    if (first == nullptr || second == nullptr) {
        return false;
    }

    auto pointOf = [](const SketchEntity& theEntity, SketchPointRole theRole) {
        switch (theRole) {
            case SketchPointRole::Start:  return theEntity.StartPoint();
            case SketchPointRole::End:    return theEntity.EndPoint();
            case SketchPointRole::Centre:
            case SketchPointRole::Whole:  return theEntity.CentrePoint();
        }
        return theEntity.CentrePoint();
    };

    theFrom = pointOf(*first, theConstraint.a.role);
    theTo = pointOf(*second, theConstraint.b.role);
    return true;
}

} // namespace

std::string FormatDimension(const SketchConstraint& theConstraint)
{
    char buffer[64] = {0};
    if (theConstraint.IsAngular()) {
        std::snprintf(buffer, sizeof(buffer), "%.1f deg",
                      theConstraint.value * 180.0 / kPi);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f", theConstraint.value);
    }

    if (theConstraint.label.empty()) {
        return std::string(buffer);
    }
    return theConstraint.label + " = " + buffer;
}

std::vector<SketchEntity> ConstraintGlyphs(const SketchFeature& theSketch, double theScale)
{
    Entities glyphs;
    if (theScale <= 0.0) {
        return glyphs;
    }

    // Several constraints often land on one point; they stack upward so
    // the marks stay legible instead of drawing on top of each other.
    std::map<int, int> stacked;

    for (const SketchConstraint& constraint : theSketch.Constraints()) {
        if (constraint.IsDimension()) {
            continue;
        }

        gp_Pnt2d anchor;
        if (!AnchorOf(theSketch, constraint, anchor)) {
            continue;
        }

        const int level = stacked[constraint.a.entity]++;
        const gp_Pnt2d at = Offset(anchor, theScale * 1.4, theScale * (1.4 + 1.6 * level));
        DrawGlyph(glyphs, constraint.type, at, theScale);

        // A hairline back to the geometry, so a stacked mark still reads
        // as belonging to its curve.
        AddLine(glyphs, anchor, Offset(at, -theScale * 0.5, 0.0));
    }

    return glyphs;
}

std::vector<SketchEntity> DimensionGeometry(const SketchFeature& theSketch, double theScale)
{
    Entities geometry;
    if (theScale <= 0.0) {
        return geometry;
    }

    for (const SketchConstraint& constraint : theSketch.Constraints()) {
        if (!constraint.IsDimension()) {
            continue;
        }

        const SketchEntity* first = theSketch.FindEntity(constraint.a.entity);
        if (first == nullptr) {
            continue;
        }

        switch (constraint.type) {
            case SketchConstraintType::Radius:
            case SketchConstraintType::Diameter:
                RadialDimension(geometry, *first, constraint.labelPosition, theScale,
                                constraint.type == SketchConstraintType::Diameter);
                break;

            case SketchConstraintType::Angle: {
                const SketchEntity* second = theSketch.FindEntity(constraint.b.entity);
                if (second != nullptr) {
                    AngularDimension(geometry, *first, *second, constraint.labelPosition,
                                     theScale);
                }
                break;
            }

            case SketchConstraintType::Distance: {
                gp_Pnt2d from, to;
                if (DimensionPoints(theSketch, constraint, from, to)) {
                    LinearDimension(geometry, from, to, constraint.labelPosition, theScale);
                }
                break;
            }

            case SketchConstraintType::DistanceX:
            case SketchConstraintType::DistanceY: {
                gp_Pnt2d from, to;
                if (!DimensionPoints(theSketch, constraint, from, to)) {
                    break;
                }
                // The measured span is only the one axis, so the far point
                // is squared off onto it before the dimension is drawn.
                const gp_Pnt2d squared =
                    constraint.type == SketchConstraintType::DistanceX
                        ? gp_Pnt2d(to.X(), from.Y())
                        : gp_Pnt2d(from.X(), to.Y());
                AddLine(geometry, to, squared);
                LinearDimension(geometry, from, squared, constraint.labelPosition, theScale);
                break;
            }

            default:
                break;
        }
    }

    return geometry;
}

std::vector<Label> DimensionLabels(const SketchFeature& theSketch)
{
    std::vector<Label> labels;
    for (const SketchConstraint& constraint : theSketch.Constraints()) {
        if (!constraint.IsDimension()) {
            continue;
        }
        if (theSketch.FindEntity(constraint.a.entity) == nullptr) {
            continue;
        }
        labels.push_back(Label{constraint.labelPosition, FormatDimension(constraint)});
    }
    return labels;
}

} // namespace SketchAnnotations
} // namespace lcad
