#include "sketch/SketchEntity.h"

#include "sketch/SketchGeometry.h"

#include <cmath>

namespace lcad {

namespace {

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kTiny  = 1.0e-9;

// Point on an ellipse at an eccentric angle, in the ellipse's own rotated
// frame. Matches Geom2d_Ellipse's parameterisation exactly so a parameter
// measured here can be handed straight to the curve.
gp_Pnt2d EllipsePoint(const SketchEntity& theEntity, double theAngle)
{
    const double c = std::cos(theEntity.rotation);
    const double s = std::sin(theEntity.rotation);
    const double u = theEntity.radius * std::cos(theAngle);
    const double v = theEntity.minorRadius * std::sin(theAngle);
    return gp_Pnt2d(theEntity.first.X() + u * c - v * s,
                    theEntity.first.Y() + u * s + v * c);
}

bool IsSplineUntrimmed(const SketchEntity& theEntity)
{
    return theEntity.trimFirst <= kTiny && theEntity.trimLast >= 1.0 - kTiny;
}

} // namespace

SketchEntity SketchEntity::MakeLine(const gp_Pnt2d& theStart, const gp_Pnt2d& theEnd)
{
    SketchEntity entity;
    entity.kind = Kind::Line;
    entity.first = theStart;
    entity.second = theEnd;
    return entity;
}

SketchEntity SketchEntity::MakeCircle(const gp_Pnt2d& theCentre, double theRadius)
{
    SketchEntity entity;
    entity.kind = Kind::Circle;
    entity.first = theCentre;
    entity.radius = std::fabs(theRadius);
    entity.startAngle = 0.0;
    entity.endAngle = kTwoPi;
    return entity;
}

SketchEntity SketchEntity::MakeArc(const gp_Pnt2d& theCentre,
                                   double          theRadius,
                                   double          theStartAngle,
                                   double          theEndAngle)
{
    SketchEntity entity;
    entity.kind = Kind::Arc;
    entity.first = theCentre;
    entity.radius = std::fabs(theRadius);
    entity.startAngle = theStartAngle;

    // Normalise to a positive CCW sweep: OCCT builds a circular edge from
    // the smaller parameter to the larger one, so a "backwards" pair of
    // angles would silently produce the complementary arc.
    double end = theEndAngle;
    while (end <= theStartAngle) {
        end += kTwoPi;
    }
    while (end - theStartAngle > kTwoPi) {
        end -= kTwoPi;
    }
    entity.endAngle = end;
    return entity;
}

SketchEntity SketchEntity::MakeEllipse(const gp_Pnt2d& theCentre,
                                       double          theMajorRadius,
                                       double          theMinorRadius,
                                       double          theRotation,
                                       double          theStartAngle,
                                       double          theEndAngle)
{
    SketchEntity entity;
    entity.kind = Kind::Ellipse;
    entity.first = theCentre;
    entity.radius = std::fabs(theMajorRadius);
    entity.minorRadius = std::fabs(theMinorRadius);
    entity.rotation = theRotation;

    // gp_Ax22d insists the major radius is the larger one, so a "tall"
    // ellipse is stored as a wide one turned a quarter turn rather than
    // being rejected.
    if (entity.minorRadius > entity.radius) {
        std::swap(entity.radius, entity.minorRadius);
        entity.rotation += kPi * 0.5;
    }

    if (std::fabs(theEndAngle - theStartAngle) <= kTiny) {
        entity.startAngle = 0.0;
        entity.endAngle = kTwoPi;
        return entity;
    }

    entity.startAngle = theStartAngle;
    double end = theEndAngle;
    while (end <= theStartAngle) {
        end += kTwoPi;
    }
    while (end - theStartAngle > kTwoPi) {
        end -= kTwoPi;
    }
    entity.endAngle = end;
    return entity;
}

SketchEntity SketchEntity::MakeSpline(const std::vector<gp_Pnt2d>& thePoints)
{
    SketchEntity entity;
    entity.kind = Kind::Spline;
    entity.points = thePoints;
    entity.trimFirst = 0.0;
    entity.trimLast = 1.0;
    return entity;
}

SketchEntity SketchEntity::MakePoint(const gp_Pnt2d& thePosition)
{
    SketchEntity entity;
    entity.kind = Kind::Point;
    entity.first = thePosition;
    entity.second = thePosition;
    return entity;
}

gp_Pnt2d SketchEntity::StartPoint() const
{
    switch (kind) {
        case Kind::Line:
        case Kind::Point:
            return first;

        case Kind::Circle:
        case Kind::Arc:
            return gp_Pnt2d(first.X() + radius * std::cos(startAngle),
                            first.Y() + radius * std::sin(startAngle));

        case Kind::Ellipse:
            return EllipsePoint(*this, startAngle);

        case Kind::Spline: {
            if (points.empty()) {
                return first;
            }
            if (IsSplineUntrimmed(*this)) {
                return points.front();
            }
            double firstParam = 0.0, lastParam = 0.0;
            if (!SketchGeometry::ParamRange(*this, firstParam, lastParam)) {
                return points.front();
            }
            return SketchGeometry::PointAt(*this, firstParam);
        }
    }
    return first;
}

gp_Pnt2d SketchEntity::EndPoint() const
{
    switch (kind) {
        case Kind::Line:
            return second;
        case Kind::Point:
            return first;

        case Kind::Circle:
        case Kind::Arc:
            return gp_Pnt2d(first.X() + radius * std::cos(endAngle),
                            first.Y() + radius * std::sin(endAngle));

        case Kind::Ellipse:
            return EllipsePoint(*this, endAngle);

        case Kind::Spline: {
            if (points.empty()) {
                return second;
            }
            if (IsSplineUntrimmed(*this)) {
                return points.back();
            }
            double firstParam = 0.0, lastParam = 0.0;
            if (!SketchGeometry::ParamRange(*this, firstParam, lastParam)) {
                return points.back();
            }
            return SketchGeometry::PointAt(*this, lastParam);
        }
    }
    return second;
}

gp_Pnt2d SketchEntity::CentrePoint() const
{
    switch (kind) {
        case Kind::Line:
            return gp_Pnt2d((first.X() + second.X()) * 0.5, (first.Y() + second.Y()) * 0.5);

        case Kind::Circle:
        case Kind::Arc:
        case Kind::Ellipse:
        case Kind::Point:
            return first;

        case Kind::Spline: {
            if (points.empty()) {
                return first;
            }
            double x = 0.0, y = 0.0;
            for (const gp_Pnt2d& point : points) {
                x += point.X();
                y += point.Y();
            }
            const double count = static_cast<double>(points.size());
            return gp_Pnt2d(x / count, y / count);
        }
    }
    return first;
}

bool SketchEntity::IsSelfClosed() const
{
    switch (kind) {
        case Kind::Circle:
            return true;
        case Kind::Arc:
        case Kind::Ellipse:
            return (endAngle - startAngle) >= kTwoPi - kTiny;
        case Kind::Spline:
            return points.size() >= 3 && IsSplineUntrimmed(*this)
                && points.front().SquareDistance(points.back())
                       <= SketchGeometry::kTolerance * SketchGeometry::kTolerance;
        case Kind::Line:
        case Kind::Point:
            return false;
    }
    return false;
}

bool SketchEntity::IsDegenerate() const
{
    switch (kind) {
        case Kind::Line:
            return first.SquareDistance(second)
                <= SketchGeometry::kTolerance * SketchGeometry::kTolerance;
        case Kind::Circle:
            return radius <= SketchGeometry::kTolerance;
        case Kind::Arc:
            return radius <= SketchGeometry::kTolerance || (endAngle - startAngle) <= kTiny;
        case Kind::Ellipse:
            return radius <= SketchGeometry::kTolerance
                || minorRadius <= SketchGeometry::kTolerance
                || (endAngle - startAngle) <= kTiny;
        case Kind::Spline:
            return points.size() < 2 || trimLast - trimFirst <= kTiny;
        case Kind::Point:
            return false;  // a point is never "too small" to keep
    }
    return true;
}

} // namespace lcad
