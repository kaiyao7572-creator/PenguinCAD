#include "sketch/SketchConstraints.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace lcad {

namespace {

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kTiny  = 1.0e-12;

// A satisfied constraint is satisfied to well under a micron; anything
// looser shows up as a visible gap in the profile.
constexpr double kConvergence = 1.0e-9;

constexpr int kMaxIterations = 80;

// Refuse absurd systems rather than freezing the UI: the dense solve is
// cubic in the unknown count, and a sketch that large is a bug upstream.
constexpr std::size_t kMaxVariables = 600;

struct Vec2
{
    double x = 0.0;
    double y = 0.0;

    Vec2 operator-(const Vec2& theOther) const { return Vec2{x - theOther.x, y - theOther.y}; }
    Vec2 operator+(const Vec2& theOther) const { return Vec2{x + theOther.x, y + theOther.y}; }
    Vec2 operator*(double theScale) const { return Vec2{x * theScale, y * theScale}; }

    double Dot(const Vec2& theOther) const { return x * theOther.x + y * theOther.y; }
    double Cross(const Vec2& theOther) const { return x * theOther.y - y * theOther.x; }
    double Length() const { return std::sqrt(x * x + y * y); }
};

std::size_t VariableCount(const SketchEntity& theEntity)
{
    switch (theEntity.kind) {
        case SketchEntity::Kind::Line:    return 4;   // both endpoints
        case SketchEntity::Kind::Circle:  return 3;   // centre + radius
        case SketchEntity::Kind::Arc:     return 5;   // centre, radius, both angles
        case SketchEntity::Kind::Ellipse: return 7;   // centre, radii, rotation, angles
        case SketchEntity::Kind::Spline:  return theEntity.points.size() * 2;
        case SketchEntity::Kind::Point:   return 2;
    }
    return 0;
}

void ReadEntity(const SketchEntity& theEntity, double* theVars)
{
    switch (theEntity.kind) {
        case SketchEntity::Kind::Line:
            theVars[0] = theEntity.first.X();
            theVars[1] = theEntity.first.Y();
            theVars[2] = theEntity.second.X();
            theVars[3] = theEntity.second.Y();
            return;

        case SketchEntity::Kind::Circle:
            theVars[0] = theEntity.first.X();
            theVars[1] = theEntity.first.Y();
            theVars[2] = theEntity.radius;
            return;

        case SketchEntity::Kind::Arc:
            theVars[0] = theEntity.first.X();
            theVars[1] = theEntity.first.Y();
            theVars[2] = theEntity.radius;
            theVars[3] = theEntity.startAngle;
            theVars[4] = theEntity.endAngle;
            return;

        case SketchEntity::Kind::Ellipse:
            theVars[0] = theEntity.first.X();
            theVars[1] = theEntity.first.Y();
            theVars[2] = theEntity.radius;
            theVars[3] = theEntity.minorRadius;
            theVars[4] = theEntity.rotation;
            theVars[5] = theEntity.startAngle;
            theVars[6] = theEntity.endAngle;
            return;

        case SketchEntity::Kind::Spline:
            for (std::size_t i = 0; i < theEntity.points.size(); ++i) {
                theVars[i * 2] = theEntity.points[i].X();
                theVars[i * 2 + 1] = theEntity.points[i].Y();
            }
            return;

        case SketchEntity::Kind::Point:
            theVars[0] = theEntity.first.X();
            theVars[1] = theEntity.first.Y();
            return;
    }
}

void WriteEntity(SketchEntity& theEntity, const double* theVars)
{
    switch (theEntity.kind) {
        case SketchEntity::Kind::Line:
            theEntity.first.SetCoord(theVars[0], theVars[1]);
            theEntity.second.SetCoord(theVars[2], theVars[3]);
            return;

        case SketchEntity::Kind::Circle:
            theEntity.first.SetCoord(theVars[0], theVars[1]);
            theEntity.radius = std::fabs(theVars[2]);
            return;

        case SketchEntity::Kind::Arc: {
            // Re-normalise through the factory so the sweep stays positive
            // and under a full turn however far the solver pushed it.
            const SketchEntity arc = SketchEntity::MakeArc(gp_Pnt2d(theVars[0], theVars[1]),
                                                           std::fabs(theVars[2]), theVars[3],
                                                           theVars[4]);
            theEntity.first = arc.first;
            theEntity.radius = arc.radius;
            theEntity.startAngle = arc.startAngle;
            theEntity.endAngle = arc.endAngle;
            return;
        }

        case SketchEntity::Kind::Ellipse: {
            const SketchEntity ellipse =
                SketchEntity::MakeEllipse(gp_Pnt2d(theVars[0], theVars[1]), std::fabs(theVars[2]),
                                          std::fabs(theVars[3]), theVars[4], theVars[5],
                                          theVars[6]);
            theEntity.first = ellipse.first;
            theEntity.radius = ellipse.radius;
            theEntity.minorRadius = ellipse.minorRadius;
            theEntity.rotation = ellipse.rotation;
            theEntity.startAngle = ellipse.startAngle;
            theEntity.endAngle = ellipse.endAngle;
            return;
        }

        case SketchEntity::Kind::Spline:
            for (std::size_t i = 0; i < theEntity.points.size(); ++i) {
                theEntity.points[i].SetCoord(theVars[i * 2], theVars[i * 2 + 1]);
            }
            return;

        case SketchEntity::Kind::Point:
            theEntity.first.SetCoord(theVars[0], theVars[1]);
            theEntity.second = theEntity.first;
            return;
    }
}

// The unknowns of one constraint system, plus everything needed to turn a
// candidate vector back into residuals.
class System
{
public:
    bool Build(const std::vector<SketchEntity>&     theEntities,
               const std::vector<SketchConstraint>& theConstraints);

    void Evaluate(const std::vector<double>& theVars, std::vector<double>& theResiduals) const;

    void Apply(const std::vector<double>& theVars, std::vector<SketchEntity>& theEntities) const;

    const std::vector<double>& Variables() const { return myVariables; }
    const std::vector<char>& Frozen() const { return myFrozen; }
    std::size_t ResidualCount() const { return myResidualCount; }
    bool IsEmpty() const { return myVariables.empty() || myResidualCount == 0; }
    const std::string& Error() const { return myError; }

private:
    struct Block
    {
        std::size_t entity = 0;   // index into the entity vector
        std::size_t offset = 0;   // into the variable vector
        std::size_t count  = 0;
    };

    // A constraint that survived Build, with its operands already resolved
    // to entity indices.
    struct Row
    {
        SketchConstraintType type = SketchConstraintType::Coincident;
        std::size_t          a = 0;
        std::size_t          b = 0;
        std::size_t          c = 0;
        SketchPointRole      roleA = SketchPointRole::Whole;
        SketchPointRole      roleB = SketchPointRole::Whole;
        bool                 hasB = false;
        bool                 hasC = false;
        double               value = 0.0;
        // Which branch of a two-sided relation to hold on to, fixed from
        // the starting geometry so tangency doesn't flip the circle to the
        // far side of the line mid-solve.
        int                  variant = 0;
        std::size_t          residuals = 0;
    };

    const double* VarsOf(const std::vector<double>& theVars, std::size_t theEntity) const
    {
        return theVars.data() + myBlocks.at(theEntity).offset;
    }

    Vec2 PointOf(const std::vector<double>& theVars,
                 std::size_t                theEntity,
                 SketchPointRole            theRole) const;
    Vec2 DirectionOf(const std::vector<double>& theVars, std::size_t theEntity) const;
    double RadiusOf(const std::vector<double>& theVars, std::size_t theEntity) const;
    bool IsCircular(std::size_t theEntity) const;
    bool IsLine(std::size_t theEntity) const;

    std::size_t ResidualsFor(const Row& theRow) const;
    void EvaluateRow(const Row& theRow, const std::vector<double>& theVars, double* theOut) const;

    std::vector<SketchEntity::Kind> myKinds;
    std::map<std::size_t, Block>    myBlocks;
    std::vector<double>             myVariables;
    std::vector<char>               myFrozen;
    std::vector<Row>                myRows;
    std::size_t                     myResidualCount = 0;
    std::string                     myError;
};

bool System::IsLine(std::size_t theEntity) const
{
    return myKinds[theEntity] == SketchEntity::Kind::Line;
}

bool System::IsCircular(std::size_t theEntity) const
{
    return myKinds[theEntity] == SketchEntity::Kind::Circle
        || myKinds[theEntity] == SketchEntity::Kind::Arc;
}

Vec2 System::PointOf(const std::vector<double>& theVars,
                     std::size_t                theEntity,
                     SketchPointRole            theRole) const
{
    const double* v = VarsOf(theVars, theEntity);

    switch (myKinds[theEntity]) {
        case SketchEntity::Kind::Line:
            switch (theRole) {
                case SketchPointRole::Start: return Vec2{v[0], v[1]};
                case SketchPointRole::End:   return Vec2{v[2], v[3]};
                case SketchPointRole::Centre:
                case SketchPointRole::Whole: return Vec2{(v[0] + v[2]) * 0.5, (v[1] + v[3]) * 0.5};
            }
            break;

        case SketchEntity::Kind::Circle:
        case SketchEntity::Kind::Point:
            return Vec2{v[0], v[1]};

        case SketchEntity::Kind::Arc:
            switch (theRole) {
                case SketchPointRole::Start:
                    return Vec2{v[0] + v[2] * std::cos(v[3]), v[1] + v[2] * std::sin(v[3])};
                case SketchPointRole::End:
                    return Vec2{v[0] + v[2] * std::cos(v[4]), v[1] + v[2] * std::sin(v[4])};
                case SketchPointRole::Centre:
                case SketchPointRole::Whole:
                    return Vec2{v[0], v[1]};
            }
            break;

        case SketchEntity::Kind::Ellipse: {
            if (theRole == SketchPointRole::Centre || theRole == SketchPointRole::Whole) {
                return Vec2{v[0], v[1]};
            }
            const double angle = theRole == SketchPointRole::Start ? v[5] : v[6];
            const double c = std::cos(v[4]);
            const double s = std::sin(v[4]);
            const double u = v[2] * std::cos(angle);
            const double w = v[3] * std::sin(angle);
            return Vec2{v[0] + u * c - w * s, v[1] + u * s + w * c};
        }

        case SketchEntity::Kind::Spline: {
            const std::size_t count = myBlocks.at(theEntity).count / 2;
            if (count == 0) {
                return Vec2{};
            }
            if (theRole == SketchPointRole::Start) {
                return Vec2{v[0], v[1]};
            }
            if (theRole == SketchPointRole::End) {
                return Vec2{v[(count - 1) * 2], v[(count - 1) * 2 + 1]};
            }
            Vec2 sum;
            for (std::size_t i = 0; i < count; ++i) {
                sum.x += v[i * 2];
                sum.y += v[i * 2 + 1];
            }
            return sum * (1.0 / static_cast<double>(count));
        }
    }
    return Vec2{v[0], v[1]};
}

Vec2 System::DirectionOf(const std::vector<double>& theVars, std::size_t theEntity) const
{
    if (IsLine(theEntity)) {
        const double* v = VarsOf(theVars, theEntity);
        return Vec2{v[2] - v[0], v[3] - v[1]};
    }
    // Only lines have a direction a constraint can act on; anything else
    // contributes a harmless unit vector rather than a NaN.
    return Vec2{1.0, 0.0};
}

double System::RadiusOf(const std::vector<double>& theVars, std::size_t theEntity) const
{
    const double* v = VarsOf(theVars, theEntity);
    switch (myKinds[theEntity]) {
        case SketchEntity::Kind::Circle:
        case SketchEntity::Kind::Arc:
            return v[2];
        case SketchEntity::Kind::Ellipse:
            return v[2];
        case SketchEntity::Kind::Line: {
            const Vec2 direction = DirectionOf(theVars, theEntity);
            return direction.Length();
        }
        case SketchEntity::Kind::Spline:
        case SketchEntity::Kind::Point:
            return 0.0;
    }
    return 0.0;
}

std::size_t System::ResidualsFor(const Row& theRow) const
{
    switch (theRow.type) {
        case SketchConstraintType::Coincident:
        case SketchConstraintType::Midpoint:
        case SketchConstraintType::Concentric:
        case SketchConstraintType::Collinear:
        case SketchConstraintType::Symmetric:
            return 2;

        case SketchConstraintType::Fix:
            return 0;  // frozen variables, not an equation

        case SketchConstraintType::Horizontal:
        case SketchConstraintType::Vertical:
        case SketchConstraintType::Parallel:
        case SketchConstraintType::Perpendicular:
        case SketchConstraintType::Equal:
        case SketchConstraintType::Tangent:
        case SketchConstraintType::Distance:
        case SketchConstraintType::DistanceX:
        case SketchConstraintType::DistanceY:
        case SketchConstraintType::Radius:
        case SketchConstraintType::Diameter:
        case SketchConstraintType::Angle:
            return 1;
    }
    return 0;
}

void System::EvaluateRow(const Row&                 theRow,
                         const std::vector<double>& theVars,
                         double*                    theOut) const
{
    auto unit = [](const Vec2& theVector) {
        const double length = theVector.Length();
        return length <= kTiny ? Vec2{1.0, 0.0} : theVector * (1.0 / length);
    };

    switch (theRow.type) {
        case SketchConstraintType::Coincident: {
            const Vec2 a = PointOf(theVars, theRow.a, theRow.roleA);
            const Vec2 b = PointOf(theVars, theRow.b, theRow.roleB);
            theOut[0] = a.x - b.x;
            theOut[1] = a.y - b.y;
            return;
        }

        case SketchConstraintType::Horizontal: {
            if (theRow.hasB) {
                theOut[0] = PointOf(theVars, theRow.a, theRow.roleA).y
                          - PointOf(theVars, theRow.b, theRow.roleB).y;
            } else {
                theOut[0] = DirectionOf(theVars, theRow.a).y;
            }
            return;
        }

        case SketchConstraintType::Vertical: {
            if (theRow.hasB) {
                theOut[0] = PointOf(theVars, theRow.a, theRow.roleA).x
                          - PointOf(theVars, theRow.b, theRow.roleB).x;
            } else {
                theOut[0] = DirectionOf(theVars, theRow.a).x;
            }
            return;
        }

        case SketchConstraintType::Parallel: {
            const Vec2 first = unit(DirectionOf(theVars, theRow.a));
            const Vec2 second = unit(DirectionOf(theVars, theRow.b));
            theOut[0] = first.Cross(second);
            return;
        }

        case SketchConstraintType::Perpendicular: {
            const Vec2 first = unit(DirectionOf(theVars, theRow.a));
            const Vec2 second = unit(DirectionOf(theVars, theRow.b));
            theOut[0] = first.Dot(second);
            return;
        }

        case SketchConstraintType::Equal:
            theOut[0] = RadiusOf(theVars, theRow.a) - RadiusOf(theVars, theRow.b);
            return;

        case SketchConstraintType::Tangent: {
            if (theRow.variant == 0) {
                // Line against circle: the centre stands off by exactly
                // one radius.
                const std::size_t line = IsLine(theRow.a) ? theRow.a : theRow.b;
                const std::size_t circle = line == theRow.a ? theRow.b : theRow.a;
                const Vec2 direction = unit(DirectionOf(theVars, line));
                const Vec2 origin = PointOf(theVars, line, SketchPointRole::Start);
                const Vec2 centre = PointOf(theVars, circle, SketchPointRole::Centre);
                theOut[0] = std::fabs(direction.Cross(centre - origin))
                          - RadiusOf(theVars, circle);
                return;
            }
            const Vec2 first = PointOf(theVars, theRow.a, SketchPointRole::Centre);
            const Vec2 second = PointOf(theVars, theRow.b, SketchPointRole::Centre);
            const double distance = (first - second).Length();
            const double radiusA = RadiusOf(theVars, theRow.a);
            const double radiusB = RadiusOf(theVars, theRow.b);
            theOut[0] = theRow.variant == 1 ? distance - (radiusA + radiusB)
                                            : distance - std::fabs(radiusA - radiusB);
            return;
        }

        case SketchConstraintType::Midpoint: {
            const Vec2 point = PointOf(theVars, theRow.a, theRow.roleA);
            const Vec2 middle = PointOf(theVars, theRow.b, SketchPointRole::Centre);
            theOut[0] = point.x - middle.x;
            theOut[1] = point.y - middle.y;
            return;
        }

        case SketchConstraintType::Concentric: {
            const Vec2 first = PointOf(theVars, theRow.a, SketchPointRole::Centre);
            const Vec2 second = PointOf(theVars, theRow.b, SketchPointRole::Centre);
            theOut[0] = first.x - second.x;
            theOut[1] = first.y - second.y;
            return;
        }

        case SketchConstraintType::Collinear: {
            const Vec2 first = unit(DirectionOf(theVars, theRow.a));
            const Vec2 second = unit(DirectionOf(theVars, theRow.b));
            const Vec2 origin = PointOf(theVars, theRow.a, SketchPointRole::Start);
            const Vec2 other = PointOf(theVars, theRow.b, SketchPointRole::Start);
            theOut[0] = first.Cross(second);
            theOut[1] = first.Cross(other - origin);
            return;
        }

        case SketchConstraintType::Symmetric: {
            const Vec2 a = PointOf(theVars, theRow.a, theRow.roleA);
            const Vec2 b = PointOf(theVars, theRow.b, theRow.roleB);
            const Vec2 axis = unit(DirectionOf(theVars, theRow.c));
            const Vec2 origin = PointOf(theVars, theRow.c, SketchPointRole::Start);
            const Vec2 middle = (a + b) * 0.5;
            // The pair straddles the mirror line: its midpoint is on the
            // line, and the segment joining them crosses it square.
            theOut[0] = axis.Cross(middle - origin);
            theOut[1] = axis.Dot(b - a);
            return;
        }

        case SketchConstraintType::Distance: {
            const Vec2 a = PointOf(theVars, theRow.a, theRow.roleA);
            const Vec2 b = PointOf(theVars, theRow.b, theRow.roleB);
            theOut[0] = (a - b).Length() - theRow.value;
            return;
        }

        case SketchConstraintType::DistanceX: {
            const Vec2 a = PointOf(theVars, theRow.a, theRow.roleA);
            const Vec2 b = PointOf(theVars, theRow.b, theRow.roleB);
            theOut[0] = (b.x - a.x) - theRow.value;
            return;
        }

        case SketchConstraintType::DistanceY: {
            const Vec2 a = PointOf(theVars, theRow.a, theRow.roleA);
            const Vec2 b = PointOf(theVars, theRow.b, theRow.roleB);
            theOut[0] = (b.y - a.y) - theRow.value;
            return;
        }

        case SketchConstraintType::Radius:
            theOut[0] = RadiusOf(theVars, theRow.a) - theRow.value;
            return;

        case SketchConstraintType::Diameter:
            theOut[0] = 2.0 * RadiusOf(theVars, theRow.a) - theRow.value;
            return;

        case SketchConstraintType::Angle: {
            const Vec2 first = unit(DirectionOf(theVars, theRow.a));
            const Vec2 second = unit(DirectionOf(theVars, theRow.b));
            const double angle = std::atan2(first.Cross(second), first.Dot(second));
            // Wrapped difference, so an angle sitting near the seam pulls
            // the short way round rather than a full turn the wrong way.
            theOut[0] = std::remainder(angle - theRow.value, kTwoPi);
            return;
        }

        case SketchConstraintType::Fix:
            return;
    }
}

bool System::Build(const std::vector<SketchEntity>&     theEntities,
                   const std::vector<SketchConstraint>& theConstraints)
{
    myKinds.clear();
    myBlocks.clear();
    myVariables.clear();
    myFrozen.clear();
    myRows.clear();
    myResidualCount = 0;
    myError.clear();

    myKinds.reserve(theEntities.size());
    for (const SketchEntity& entity : theEntities) {
        myKinds.push_back(entity.kind);
    }

    std::map<int, std::size_t> byId;
    for (std::size_t i = 0; i < theEntities.size(); ++i) {
        if (theEntities[i].id != 0) {
            byId[theEntities[i].id] = i;
        }
    }

    auto resolve = [&](const SketchPointRef& theRef, std::size_t& theIndex) {
        if (!theRef.IsValid()) {
            return false;
        }
        const auto found = byId.find(theRef.entity);
        if (found == byId.end()) {
            return false;
        }
        theIndex = found->second;
        return true;
    };

    // Only entities a constraint actually mentions become unknowns. That
    // keeps the dense solve proportional to the constrained part of the
    // sketch rather than the whole drawing.
    std::vector<std::size_t> involved;
    std::vector<Row> rows;
    std::vector<std::size_t> fixedEntities;

    for (const SketchConstraint& constraint : theConstraints) {
        Row row;
        row.type = constraint.type;
        row.value = constraint.value;
        row.roleA = constraint.a.role;
        row.roleB = constraint.b.role;

        if (!resolve(constraint.a, row.a)) {
            continue;  // the entity was deleted; the constraint is dead
        }
        involved.push_back(row.a);

        row.hasB = resolve(constraint.b, row.b);
        if (row.hasB) {
            involved.push_back(row.b);
        }
        row.hasC = resolve(constraint.c, row.c);
        if (row.hasC) {
            involved.push_back(row.c);
        }

        if (constraint.type == SketchConstraintType::Fix) {
            fixedEntities.push_back(row.a);
            continue;
        }

        // Everything except Horizontal, Vertical, Fix, Radius and Diameter
        // is a relation and needs a second operand.
        const bool needsSecond = constraint.type != SketchConstraintType::Horizontal
                              && constraint.type != SketchConstraintType::Vertical
                              && constraint.type != SketchConstraintType::Radius
                              && constraint.type != SketchConstraintType::Diameter;
        if (needsSecond && !row.hasB) {
            continue;
        }
        if (constraint.type == SketchConstraintType::Symmetric && !row.hasC) {
            continue;
        }

        row.residuals = ResidualsFor(row);
        if (row.residuals == 0) {
            continue;
        }
        rows.push_back(row);
    }

    if (rows.empty() && fixedEntities.empty()) {
        return false;
    }

    std::sort(involved.begin(), involved.end());
    involved.erase(std::unique(involved.begin(), involved.end()), involved.end());

    std::size_t offset = 0;
    for (const std::size_t index : involved) {
        Block block;
        block.entity = index;
        block.offset = offset;
        block.count = VariableCount(theEntities[index]);
        if (block.count == 0) {
            continue;
        }
        myBlocks[index] = block;
        offset += block.count;
    }

    if (offset == 0) {
        return false;
    }
    if (offset > kMaxVariables) {
        myError = "sketch has too many constrained entities to solve";
        return false;
    }

    myVariables.assign(offset, 0.0);
    myFrozen.assign(offset, 0);
    for (const auto& entry : myBlocks) {
        ReadEntity(theEntities[entry.first], myVariables.data() + entry.second.offset);
    }

    for (const std::size_t index : fixedEntities) {
        const auto found = myBlocks.find(index);
        if (found == myBlocks.end()) {
            continue;
        }
        for (std::size_t i = 0; i < found->second.count; ++i) {
            myFrozen[found->second.offset + i] = 1;
        }
    }

    // Tangency has two answers -- inside and outside -- and the right one
    // is whichever the geometry is already nearest, so the sketch doesn't
    // jump through itself the moment the constraint is applied.
    for (Row& row : rows) {
        if (row.type != SketchConstraintType::Tangent) {
            continue;
        }
        if (IsLine(row.a) || IsLine(row.b)) {
            row.variant = 0;
            continue;
        }
        const Vec2 first = PointOf(myVariables, row.a, SketchPointRole::Centre);
        const Vec2 second = PointOf(myVariables, row.b, SketchPointRole::Centre);
        const double distance = (first - second).Length();
        const double radiusA = RadiusOf(myVariables, row.a);
        const double radiusB = RadiusOf(myVariables, row.b);
        row.variant = std::fabs(distance - (radiusA + radiusB))
                            <= std::fabs(distance - std::fabs(radiusA - radiusB))
                        ? 1
                        : 2;
    }

    myRows = rows;
    myResidualCount = 0;
    for (const Row& row : myRows) {
        myResidualCount += row.residuals;
    }
    return true;
}

void System::Evaluate(const std::vector<double>& theVars, std::vector<double>& theResiduals) const
{
    theResiduals.assign(myResidualCount, 0.0);
    std::size_t at = 0;
    for (const Row& row : myRows) {
        EvaluateRow(row, theVars, theResiduals.data() + at);
        at += row.residuals;
    }
}

void System::Apply(const std::vector<double>&  theVars,
                   std::vector<SketchEntity>& theEntities) const
{
    for (const auto& entry : myBlocks) {
        WriteEntity(theEntities[entry.first], theVars.data() + entry.second.offset);
    }
}

// Gaussian elimination with partial pivoting. Small dense systems only --
// a sketch never produces enough unknowns to want anything cleverer.
bool SolveDense(std::vector<double>& theMatrix, std::vector<double>& theRight, std::size_t theSize)
{
    for (std::size_t column = 0; column < theSize; ++column) {
        std::size_t pivot = column;
        double best = std::fabs(theMatrix[column * theSize + column]);
        for (std::size_t row = column + 1; row < theSize; ++row) {
            const double candidate = std::fabs(theMatrix[row * theSize + column]);
            if (candidate > best) {
                best = candidate;
                pivot = row;
            }
        }
        if (best <= kTiny) {
            return false;
        }

        if (pivot != column) {
            for (std::size_t k = 0; k < theSize; ++k) {
                std::swap(theMatrix[column * theSize + k], theMatrix[pivot * theSize + k]);
            }
            std::swap(theRight[column], theRight[pivot]);
        }

        const double diagonal = theMatrix[column * theSize + column];
        for (std::size_t row = column + 1; row < theSize; ++row) {
            const double factor = theMatrix[row * theSize + column] / diagonal;
            if (factor == 0.0) {
                continue;
            }
            for (std::size_t k = column; k < theSize; ++k) {
                theMatrix[row * theSize + k] -= factor * theMatrix[column * theSize + k];
            }
            theRight[row] -= factor * theRight[column];
        }
    }

    for (std::size_t i = theSize; i-- > 0;) {
        double sum = theRight[i];
        for (std::size_t k = i + 1; k < theSize; ++k) {
            sum -= theMatrix[i * theSize + k] * theRight[k];
        }
        theRight[i] = sum / theMatrix[i * theSize + i];
    }
    return true;
}

double MaxAbsolute(const std::vector<double>& theValues)
{
    double worst = 0.0;
    for (const double value : theValues) {
        worst = std::max(worst, std::fabs(value));
    }
    return worst;
}

double SumSquares(const std::vector<double>& theValues)
{
    double total = 0.0;
    for (const double value : theValues) {
        total += value * value;
    }
    return total;
}

} // namespace

const char* SketchConstraint::TypeName(SketchConstraintType theType)
{
    switch (theType) {
        case SketchConstraintType::Coincident:    return "Coincident";
        case SketchConstraintType::Horizontal:    return "Horizontal";
        case SketchConstraintType::Vertical:      return "Vertical";
        case SketchConstraintType::Parallel:      return "Parallel";
        case SketchConstraintType::Perpendicular: return "Perpendicular";
        case SketchConstraintType::Equal:         return "Equal";
        case SketchConstraintType::Tangent:       return "Tangent";
        case SketchConstraintType::Midpoint:      return "Midpoint";
        case SketchConstraintType::Concentric:    return "Concentric";
        case SketchConstraintType::Collinear:     return "Collinear";
        case SketchConstraintType::Fix:           return "Fix";
        case SketchConstraintType::Symmetric:     return "Symmetry";
        case SketchConstraintType::Distance:      return "Distance";
        case SketchConstraintType::DistanceX:     return "Horizontal Distance";
        case SketchConstraintType::DistanceY:     return "Vertical Distance";
        case SketchConstraintType::Radius:        return "Radius";
        case SketchConstraintType::Diameter:      return "Diameter";
        case SketchConstraintType::Angle:         return "Angle";
    }
    return "Constraint";
}

double SketchSolver::Residual(const std::vector<SketchEntity>&     theEntities,
                              const std::vector<SketchConstraint>& theConstraints)
{
    System system;
    if (!system.Build(theEntities, theConstraints) || system.IsEmpty()) {
        return 0.0;
    }
    std::vector<double> residuals;
    system.Evaluate(system.Variables(), residuals);
    return MaxAbsolute(residuals);
}

SketchSolver::Result SketchSolver::Solve(std::vector<SketchEntity>&           theEntities,
                                         const std::vector<SketchConstraint>& theConstraints)
{
    Result result;

    System system;
    if (!system.Build(theEntities, theConstraints)) {
        result.error = system.Error();
        result.converged = result.error.empty();  // nothing to solve is success
        return result;
    }
    if (system.IsEmpty()) {
        result.converged = true;  // Fix-only systems have nothing to move
        return result;
    }

    std::vector<double> vars = system.Variables();
    const std::vector<char>& frozen = system.Frozen();

    // Map every movable variable to a column of the reduced system.
    std::vector<std::size_t> free;
    free.reserve(vars.size());
    for (std::size_t i = 0; i < vars.size(); ++i) {
        if (frozen[i] == 0) {
            free.push_back(i);
        }
    }
    if (free.empty()) {
        std::vector<double> residuals;
        system.Evaluate(vars, residuals);
        result.residual = MaxAbsolute(residuals);
        result.converged = result.residual <= kConvergence;
        return result;
    }

    const std::size_t columns = free.size();
    const std::size_t rows = system.ResidualCount();

    std::vector<double> residuals;
    std::vector<double> trialResiduals;
    std::vector<double> jacobian(rows * columns, 0.0);
    std::vector<double> normal(columns * columns, 0.0);
    std::vector<double> gradient(columns, 0.0);
    std::vector<double> trial(vars.size(), 0.0);

    system.Evaluate(vars, residuals);
    double error = SumSquares(residuals);

    // Levenberg-Marquardt: the damping term doubles as the "move as
    // little as possible" rule an under-constrained sketch needs.
    double damping = 1.0e-4;

    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        result.iterations = iteration + 1;
        result.residual = MaxAbsolute(residuals);
        if (result.residual <= kConvergence) {
            result.converged = true;
            break;
        }

        for (std::size_t column = 0; column < columns; ++column) {
            const std::size_t index = free[column];
            const double step = 1.0e-7 * std::max(1.0, std::fabs(vars[index]));
            const double original = vars[index];
            vars[index] = original + step;
            system.Evaluate(vars, trialResiduals);
            vars[index] = original;
            for (std::size_t row = 0; row < rows; ++row) {
                jacobian[row * columns + column] = (trialResiduals[row] - residuals[row]) / step;
            }
        }

        for (std::size_t i = 0; i < columns; ++i) {
            double sum = 0.0;
            for (std::size_t row = 0; row < rows; ++row) {
                sum += jacobian[row * columns + i] * residuals[row];
            }
            gradient[i] = -sum;

            for (std::size_t j = i; j < columns; ++j) {
                double entry = 0.0;
                for (std::size_t row = 0; row < rows; ++row) {
                    entry += jacobian[row * columns + i] * jacobian[row * columns + j];
                }
                normal[i * columns + j] = entry;
                normal[j * columns + i] = entry;
            }
        }

        bool stepped = false;
        for (int attempt = 0; attempt < 12 && !stepped; ++attempt) {
            std::vector<double> matrix = normal;
            std::vector<double> right = gradient;
            for (std::size_t i = 0; i < columns; ++i) {
                matrix[i * columns + i] += damping * (1.0 + matrix[i * columns + i]);
            }

            if (!SolveDense(matrix, right, columns)) {
                damping *= 8.0;
                continue;
            }

            trial = vars;
            bool finite = true;
            for (std::size_t i = 0; i < columns; ++i) {
                if (!std::isfinite(right[i])) {
                    finite = false;
                    break;
                }
                trial[free[i]] += right[i];
            }
            if (!finite) {
                damping *= 8.0;
                continue;
            }

            system.Evaluate(trial, trialResiduals);
            const double trialError = SumSquares(trialResiduals);
            if (trialError < error) {
                vars = trial;
                residuals = trialResiduals;
                error = trialError;
                damping = std::max(damping * 0.3, 1.0e-9);
                stepped = true;
            } else {
                damping *= 8.0;
            }
        }

        if (!stepped) {
            break;  // no downhill direction left; this is the best fit
        }
    }

    result.residual = MaxAbsolute(residuals);
    result.converged = result.residual <= kConvergence;
    if (!result.converged && result.error.empty()) {
        result.error = "constraints could not all be satisfied";
    }

    system.Apply(vars, theEntities);
    return result;
}

bool SketchSolver::MeasureDimension(const std::vector<SketchEntity>& theEntities,
                                    const SketchConstraint&          theConstraint,
                                    double&                          theValue)
{
    if (!theConstraint.IsDimension()) {
        return false;
    }

    auto find = [&theEntities](int theId) -> const SketchEntity* {
        for (const SketchEntity& entity : theEntities) {
            if (entity.id == theId) {
                return &entity;
            }
        }
        return nullptr;
    };

    auto pointOf = [](const SketchEntity& theEntity, SketchPointRole theRole) {
        switch (theRole) {
            case SketchPointRole::Start:  return theEntity.StartPoint();
            case SketchPointRole::End:    return theEntity.EndPoint();
            case SketchPointRole::Centre:
            case SketchPointRole::Whole:  return theEntity.CentrePoint();
        }
        return theEntity.CentrePoint();
    };

    const SketchEntity* first = find(theConstraint.a.entity);
    if (first == nullptr) {
        return false;
    }

    switch (theConstraint.type) {
        case SketchConstraintType::Radius:
            theValue = first->radius;
            return theValue > 0.0;

        case SketchConstraintType::Diameter:
            theValue = first->radius * 2.0;
            return theValue > 0.0;

        case SketchConstraintType::Angle: {
            const SketchEntity* second = find(theConstraint.b.entity);
            if (second == nullptr || first->kind != SketchEntity::Kind::Line
             || second->kind != SketchEntity::Kind::Line) {
                return false;
            }
            const gp_Pnt2d a0 = first->StartPoint();
            const gp_Pnt2d a1 = first->EndPoint();
            const gp_Pnt2d b0 = second->StartPoint();
            const gp_Pnt2d b1 = second->EndPoint();
            const double cross = (a1.X() - a0.X()) * (b1.Y() - b0.Y())
                               - (a1.Y() - a0.Y()) * (b1.X() - b0.X());
            const double dot = (a1.X() - a0.X()) * (b1.X() - b0.X())
                             + (a1.Y() - a0.Y()) * (b1.Y() - b0.Y());
            theValue = std::atan2(cross, dot);
            return true;
        }

        case SketchConstraintType::Distance:
        case SketchConstraintType::DistanceX:
        case SketchConstraintType::DistanceY: {
            const SketchEntity* second = find(theConstraint.b.entity);
            if (second == nullptr) {
                return false;
            }
            const gp_Pnt2d a = pointOf(*first, theConstraint.a.role);
            const gp_Pnt2d b = pointOf(*second, theConstraint.b.role);
            if (theConstraint.type == SketchConstraintType::Distance) {
                theValue = a.Distance(b);
                return theValue > 0.0;
            }
            theValue = theConstraint.type == SketchConstraintType::DistanceX ? b.X() - a.X()
                                                                            : b.Y() - a.Y();
            return std::fabs(theValue) > 0.0;
        }

        default:
            return false;
    }
}

} // namespace lcad
