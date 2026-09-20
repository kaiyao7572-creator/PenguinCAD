#include "sketch/SketchAnnotations.h"

#include "core/Units.h"
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

// The dimension LINE is drawn through labelPosition, so text anchored
// there lands on top of it. Drafting convention floats the number just
// clear of the line, which is what this lift does; the display centres
// the text on the point, so lifting is all that is needed.
gp_Pnt2d LiftedLabel(const SketchFeature&    theSketch,
                     const SketchConstraint& theConstraint,
                     double                  theScale)
{
    const gp_Pnt2d label = theConstraint.labelPosition;
    if (theScale <= 0.0) {
        return label;
    }
    // A whole glyph, not half of one: the text runs horizontally while
    // the dimension line usually does not, so half a glyph still leaves
    // the far end of the number sitting on the line.
    const double lift = theScale * 1.2;

    switch (theConstraint.type) {
        case SketchConstraintType::Distance:
        case SketchConstraintType::DistanceX:
        case SketchConstraintType::DistanceY: {
            gp_Pnt2d from, to;
            if (!DimensionPoints(theSketch, theConstraint, from, to)) {
                break;
            }
            if (theConstraint.type == SketchConstraintType::DistanceX) {
                to = gp_Pnt2d(to.X(), from.Y());
            } else if (theConstraint.type == SketchConstraintType::DistanceY) {
                to = gp_Pnt2d(from.X(), to.Y());
            }

            gp_Vec2d along;
            if (!UnitBetween(from, to, along)) {
                break;
            }
            // Lifted AWAY from what is being measured, never back across
            // it: the far side is where the user dropped the number.
            const gp_Vec2d normal = Perpendicular(along);
            const double side =
                normal.X() * (label.X() - from.X()) + normal.Y() * (label.Y() - from.Y());
            return label.Translated(normal * (side >= 0.0 ? lift : -lift));
        }

        case SketchConstraintType::Radius:
        case SketchConstraintType::Diameter: {
            const SketchEntity* entity = theSketch.FindEntity(theConstraint.a.entity);
            gp_Vec2d leader;
            if (entity == nullptr || !UnitBetween(entity->first, label, leader)) {
                break;
            }
            return label.Translated(Perpendicular(leader) * lift);
        }

        default:
            break;
    }

    // Nothing better known: straight up, which is where a number goes
    // when it has no line to clear.
    return Offset(label, 0.0, lift);
}

} // namespace

std::string FormatDimension(const SketchConstraint& theConstraint)
{
    // Through FormatValue, so a dimension reads in the document's unit and
    // trims its own trailing zeros: 25.4 mm, not 25.400000, and 1 in when
    // the document is in inches.
    std::string text;
    if (theConstraint.IsAngular()) {
        text = FormatValue(theConstraint.value * 180.0 / kPi, UnitKind::Angle,
                           LengthUnit::Millimeter, AngleUnit::Degree, 2);
    } else {
        text = FormatValue(theConstraint.value, UnitKind::Length, DefaultLengthUnit(),
                           AngleUnit::Degree, 3);
    }

    // Drafting's own shorthand, and the only thing that tells a radius
    // apart from a diameter on the same circle. ASCII on purpose: the
    // diameter sign Fusion uses is not in the viewport's font.
    if (theConstraint.type == SketchConstraintType::Radius) {
        text = "R" + text;
    } else if (theConstraint.type == SketchConstraintType::Diameter) {
        text = "D" + text;
    }

    // Just the value on the canvas. Fusion shows the NUMBER on a sketch
    // and keeps the parameter name ("d1") for the properties panel and
    // the parameters dialog -- putting the name on the drawing doubles
    // the width of every dimension and buries the one part of it anybody
    // reads at a glance.
    return text;
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

namespace {

// Length of a curve. Exact for the kinds with a closed form, sampled for
// the ones without -- a number the user reads off the screen should not
// be approximate when it does not have to be.
double CurveLength(const SketchEntity& theEntity)
{
    if (theEntity.kind == SketchEntity::Kind::Line) {
        return theEntity.first.Distance(theEntity.second);
    }
    if (theEntity.kind == SketchEntity::Kind::Arc) {
        double sweep = theEntity.endAngle - theEntity.startAngle;
        while (sweep < 0.0) {
            sweep += 2.0 * kPi;
        }
        return theEntity.radius * sweep;
    }
    if (theEntity.kind == SketchEntity::Kind::Circle) {
        return 2.0 * kPi * theEntity.radius;
    }

    double first = 0.0, last = 0.0;
    if (!SketchGeometry::ParamRange(theEntity, first, last)) {
        return 0.0;
    }
    constexpr int kSamples = 96;
    double   total = 0.0;
    gp_Pnt2d previous = SketchGeometry::PointAt(theEntity, first);
    for (int i = 1; i <= kSamples; ++i) {
        const gp_Pnt2d current =
            SketchGeometry::PointAt(theEntity, first + (last - first) * i / kSamples);
        total += previous.Distance(current);
        previous = current;
    }
    return total;
}

// What to call a curve's size. A line has a length; a circle and an arc
// are named by diameter and radius, because that is what anyone building
// the part measures and what a dimension on them would say.
std::string MeasureText(const SketchEntity& theEntity)
{
    const LengthUnit unit = DefaultLengthUnit();
    switch (theEntity.kind) {
        case SketchEntity::Kind::Circle:
            return "D" + FormatValue(2.0 * theEntity.radius, UnitKind::Length, unit,
                                     AngleUnit::Degree, 2);
        case SketchEntity::Kind::Arc:
            return "R" + FormatValue(theEntity.radius, UnitKind::Length, unit,
                                     AngleUnit::Degree, 2);
        case SketchEntity::Kind::Ellipse:
            return "R" + FormatValue(theEntity.radius, UnitKind::Length, unit,
                                     AngleUnit::Degree, 2);
        case SketchEntity::Kind::Point:
            return std::string();
        default:
            break;
    }
    // Two decimals, not three: this is a number to glance at, and a
    // third digit only makes every label wider.
    return FormatValue(CurveLength(theEntity), UnitKind::Length, unit, AngleUnit::Degree, 2);
}

} // namespace

std::vector<Label> CurveMeasureLabels(const SketchFeature& theSketch, double theScale)
{
    std::vector<Label> labels;
    const double lift = theScale > 0.0 ? theScale : 1.0;

    for (const SketchEntity& entity : theSketch.Entities()) {
        if (!entity.IsCurve() || entity.IsDegenerate()) {
            continue;
        }
        const std::string text = MeasureText(entity);
        if (text.empty()) {
            continue;
        }

        double first = 0.0, last = 0.0;
        if (!SketchGeometry::ParamRange(entity, first, last)) {
            continue;
        }
        gp_Pnt2d at;
        if (entity.IsSelfClosed()) {
            // A closed curve has no meaningful "middle" to hang a number
            // off -- the parameter midpoint is just wherever it happens to
            // start -- and a diameter belongs at the centre anyway.
            at = entity.CentrePoint();
        } else {
            const double   middle  = 0.5 * (first + last);
            const gp_Vec2d tangent = SketchGeometry::TangentAt(entity, middle);
            at = SketchGeometry::PointAt(entity, middle);

            // Lifted off the curve along its own normal, so the number
            // never sits on the line it is measuring.
            if (tangent.Magnitude() > 1.0e-12) {
                const gp_Vec2d normal(-tangent.Y() / tangent.Magnitude(),
                                      tangent.X() / tangent.Magnitude());
                at = gp_Pnt2d(at.X() + normal.X() * lift, at.Y() + normal.Y() * lift);
            }
        }

        // Horizontal on purpose: these are read, not drafted, and text
        // rotated with the curve is slower to scan.
        labels.push_back(Label{at, text, 0.0});
    }
    return labels;
}

std::vector<Label> DimensionLabels(const SketchFeature& theSketch, double theScale)
{
    std::vector<Label> labels;
    for (const SketchConstraint& constraint : theSketch.Constraints()) {
        if (!constraint.IsDimension()) {
            continue;
        }
        if (theSketch.FindEntity(constraint.a.entity) == nullptr) {
            continue;
        }
        labels.push_back(
            Label{LiftedLabel(theSketch, constraint, theScale), FormatDimension(constraint)});
    }
    return labels;
}

} // namespace SketchAnnotations
} // namespace lcad
