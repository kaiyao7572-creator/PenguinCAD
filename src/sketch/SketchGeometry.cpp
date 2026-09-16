#include "sketch/SketchGeometry.h"

#include <Geom2dAPI_InterCurveCurve.hxx>
#include <Geom2dAPI_Interpolate.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_Circle.hxx>
#include <Geom2d_Ellipse.hxx>
#include <Geom2d_Line.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Geom2dInt_GInter.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Ellipse.hxx>
#include <Geom_Line.hxx>
#include <IntRes2d_IntersectionPoint.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt2d.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax22d.hxx>
#include <gp_Circ2d.hxx>
#include <gp_Dir.hxx>
#include <gp_Dir2d.hxx>
#include <gp_Elips2d.hxx>
#include <gp_Lin2d.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lcad {
namespace SketchGeometry {

namespace {

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kTiny  = 1.0e-12;

// Samples used when a curve has no closed-form nearest-point solution.
// Enough to land in the right lobe of an ellipse or a wiggly spline; the
// bisection that follows does the rest.
constexpr int kSampleCount = 192;
constexpr int kRefineSteps = 40;

double AngleOf(const gp_Pnt2d& theCentre, const gp_Pnt2d& thePoint)
{
    return std::atan2(thePoint.Y() - theCentre.Y(), thePoint.X() - theCentre.X());
}

gp_Vec2d Perpendicular(const gp_Vec2d& theVector)
{
    return gp_Vec2d(-theVector.Y(), theVector.X());
}

bool LineFrame(const SketchEntity& theEntity, gp_Pnt2d& theOrigin, gp_Vec2d& theDirection,
               double& theLength)
{
    theOrigin = theEntity.first;
    const gp_Vec2d delta(theEntity.second.X() - theEntity.first.X(),
                         theEntity.second.Y() - theEntity.first.Y());
    theLength = delta.Magnitude();
    if (theLength <= kTolerance) {
        return false;
    }
    theDirection = delta / theLength;
    return true;
}

// A closed spline is stored with its first point repeated at the end,
// which is how the tools express "click back on the start". OCCT's
// interpolator wants the duplicate gone and the periodic flag set
// instead.
bool SplineIsClosed(const SketchEntity& theEntity)
{
    return theEntity.points.size() >= 3
        && theEntity.points.front().SquareDistance(theEntity.points.back())
               <= kTolerance * kTolerance;
}

Handle(Geom2d_Curve) BuildSpline(const SketchEntity& theEntity)
{
    const bool closed = SplineIsClosed(theEntity);
    const std::size_t count = closed ? theEntity.points.size() - 1 : theEntity.points.size();
    if (count < 2) {
        return Handle(Geom2d_Curve)();
    }

    try {
        Handle(TColgp_HArray1OfPnt2d) array =
            new TColgp_HArray1OfPnt2d(1, static_cast<Standard_Integer>(count));
        for (std::size_t i = 0; i < count; ++i) {
            array->SetValue(static_cast<Standard_Integer>(i + 1), theEntity.points[i]);
        }

        Geom2dAPI_Interpolate interpolate(array, closed ? Standard_True : Standard_False,
                                          kTolerance);
        interpolate.Perform();
        if (!interpolate.IsDone()) {
            return Handle(Geom2d_Curve)();
        }
        return interpolate.Curve();
    } catch (const Standard_Failure&) {
        return Handle(Geom2d_Curve)();
    }
}

gp_Pnt2d EllipsePoint(const SketchEntity& theEntity, double theAngle)
{
    const double c = std::cos(theEntity.rotation);
    const double s = std::sin(theEntity.rotation);
    const double u = theEntity.radius * std::cos(theAngle);
    const double v = theEntity.minorRadius * std::sin(theAngle);
    return gp_Pnt2d(theEntity.first.X() + u * c - v * s,
                    theEntity.first.Y() + u * s + v * c);
}

gp_Vec2d EllipseTangent(const SketchEntity& theEntity, double theAngle)
{
    const double c = std::cos(theEntity.rotation);
    const double s = std::sin(theEntity.rotation);
    const double du = -theEntity.radius * std::sin(theAngle);
    const double dv = theEntity.minorRadius * std::cos(theAngle);
    return gp_Vec2d(du * c - dv * s, du * s + dv * c);
}

// Period of the entity's parameterisation, or 0 for an open curve.
double PeriodOf(const SketchEntity& theEntity)
{
    switch (theEntity.kind) {
        case SketchEntity::Kind::Circle:
        case SketchEntity::Kind::Arc:
        case SketchEntity::Kind::Ellipse:
            return kTwoPi;
        case SketchEntity::Kind::Spline:
            return 0.0;  // handled through the curve itself, which knows
        case SketchEntity::Kind::Line:
        case SketchEntity::Kind::Point:
            return 0.0;
    }
    return 0.0;
}

// Shift a periodic parameter into the entity's window, then say whether
// it actually landed inside. Without this a trimmed arc spanning the
// 0/2*pi seam would reject every intersection on it.
bool InWindow(const SketchEntity& theEntity, double& theParam, double theTolerance = kTolerance)
{
    double first = 0.0, last = 0.0;
    if (!ParamRange(theEntity, first, last)) {
        return false;
    }

    const double period = PeriodOf(theEntity);
    if (period > 0.0) {
        while (theParam < first - theTolerance) {
            theParam += period;
        }
        while (theParam > first + period + theTolerance) {
            theParam -= period;
        }
    }
    return theParam >= first - theTolerance && theParam <= last + theTolerance;
}

gp_Pnt To3dPoint(const gp_Pnt2d& thePoint, const gp_Ax3& thePosition)
{
    const gp_Vec offset = gp_Vec(thePosition.XDirection()) * thePoint.X()
                        + gp_Vec(thePosition.YDirection()) * thePoint.Y();
    return thePosition.Location().Translated(offset);
}

gp_Vec To3dVector(const gp_Vec2d& theVector, const gp_Ax3& thePosition)
{
    return gp_Vec(thePosition.XDirection()) * theVector.X()
         + gp_Vec(thePosition.YDirection()) * theVector.Y();
}

// Nearest parameter by dense sampling followed by bisection on the
// derivative of the squared distance. Used for the curves with no closed
// form; 40 halvings take the bracket below float noise.
bool SampleNearest(const Handle(Geom2d_Curve)& theCurve,
                   double                      theFirst,
                   double                      theLast,
                   const gp_Pnt2d&             thePoint,
                   double&                     theParam)
{
    if (theCurve.IsNull() || theLast - theFirst <= kTiny) {
        return false;
    }

    const double step = (theLast - theFirst) / kSampleCount;
    double bestParam = theFirst;
    double bestSquared = -1.0;
    for (int i = 0; i <= kSampleCount; ++i) {
        const double param = theFirst + step * i;
        const double squared = theCurve->Value(param).SquareDistance(thePoint);
        if (bestSquared < 0.0 || squared < bestSquared) {
            bestSquared = squared;
            bestParam = param;
        }
    }

    double low = std::max(theFirst, bestParam - step);
    double high = std::min(theLast, bestParam + step);
    for (int i = 0; i < kRefineSteps && high - low > kTiny; ++i) {
        const double third = (high - low) / 3.0;
        const double a = low + third;
        const double b = high - third;
        if (theCurve->Value(a).SquareDistance(thePoint)
            <= theCurve->Value(b).SquareDistance(thePoint)) {
            high = b;
        } else {
            low = a;
        }
    }

    theParam = (low + high) * 0.5;
    return true;
}

SketchEntity CopyIdentity(const SketchEntity& theSource, SketchEntity theResult)
{
    theResult.id = theSource.id;
    theResult.isConstruction = theSource.isConstruction;
    return theResult;
}

// Re-fit a curve that offsetting turns into something its own kind can no
// longer express. Sampling and interpolating is the only way to keep an
// offset ellipse or spline a single sketch entity.
bool RefitOffsetAsSpline(const SketchEntity& theEntity, double theDistance, SketchEntity& theResult)
{
    double first = 0.0, last = 0.0;
    if (!ParamRange(theEntity, first, last)) {
        return false;
    }

    constexpr int kOffsetSamples = 48;
    std::vector<gp_Pnt2d> points;
    points.reserve(kOffsetSamples + 1);

    const double step = (last - first) / kOffsetSamples;
    for (int i = 0; i <= kOffsetSamples; ++i) {
        const double param = first + step * i;
        const gp_Pnt2d point = PointAt(theEntity, param);
        gp_Vec2d tangent = TangentAt(theEntity, param);
        if (tangent.SquareMagnitude() <= kTiny) {
            continue;
        }
        tangent.Normalize();
        const gp_Vec2d normal = Perpendicular(tangent);
        points.push_back(gp_Pnt2d(point.X() + normal.X() * theDistance,
                                  point.Y() + normal.Y() * theDistance));
    }

    if (points.size() < 3) {
        return false;
    }

    // Interpolation refuses coincident points, which an offset larger than
    // the local radius of curvature readily produces.
    std::vector<gp_Pnt2d> distinct;
    distinct.push_back(points.front());
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (points[i].SquareDistance(distinct.back()) > kTolerance * kTolerance) {
            distinct.push_back(points[i]);
        }
    }
    if (distinct.size() < 3) {
        return false;
    }

    theResult = CopyIdentity(theEntity, SketchEntity::MakeSpline(distinct));
    theResult.id = 0;  // an offset is new geometry, not the original
    return true;
}

} // namespace

// ---- curves and parameters ----

Handle(Geom2d_Curve) Curve2dOf(const SketchEntity& theEntity)
{
    try {
        switch (theEntity.kind) {
            case SketchEntity::Kind::Line: {
                gp_Pnt2d origin;
                gp_Vec2d direction;
                double length = 0.0;
                if (!LineFrame(theEntity, origin, direction, length)) {
                    return Handle(Geom2d_Curve)();
                }
                return new Geom2d_Line(origin, gp_Dir2d(direction));
            }

            case SketchEntity::Kind::Circle:
            case SketchEntity::Kind::Arc: {
                if (theEntity.radius <= kTolerance) {
                    return Handle(Geom2d_Curve)();
                }
                const gp_Ax22d axis(theEntity.first, gp_Dir2d(1.0, 0.0), gp_Dir2d(0.0, 1.0));
                return new Geom2d_Circle(gp_Circ2d(axis, theEntity.radius));
            }

            case SketchEntity::Kind::Ellipse: {
                if (theEntity.radius <= kTolerance || theEntity.minorRadius <= kTolerance
                 || theEntity.minorRadius > theEntity.radius) {
                    return Handle(Geom2d_Curve)();
                }
                const gp_Dir2d major(std::cos(theEntity.rotation), std::sin(theEntity.rotation));
                const gp_Ax22d axis(theEntity.first, major, Standard_True);
                return new Geom2d_Ellipse(
                    gp_Elips2d(axis, theEntity.radius, theEntity.minorRadius));
            }

            case SketchEntity::Kind::Spline:
                return BuildSpline(theEntity);

            case SketchEntity::Kind::Point:
                return Handle(Geom2d_Curve)();
        }
    } catch (const Standard_Failure&) {
        return Handle(Geom2d_Curve)();
    }
    return Handle(Geom2d_Curve)();
}

bool ParamRange(const SketchEntity& theEntity, double& theFirst, double& theLast)
{
    switch (theEntity.kind) {
        case SketchEntity::Kind::Line: {
            gp_Pnt2d origin;
            gp_Vec2d direction;
            double length = 0.0;
            if (!LineFrame(theEntity, origin, direction, length)) {
                return false;
            }
            theFirst = 0.0;
            theLast = length;
            return true;
        }

        case SketchEntity::Kind::Circle:
        case SketchEntity::Kind::Arc:
        case SketchEntity::Kind::Ellipse:
            theFirst = theEntity.startAngle;
            theLast = theEntity.endAngle;
            return theLast - theFirst > kTiny;

        case SketchEntity::Kind::Spline: {
            const Handle(Geom2d_Curve) curve = Curve2dOf(theEntity);
            if (curve.IsNull()) {
                return false;
            }
            const double low = curve->FirstParameter();
            const double span = curve->LastParameter() - low;
            theFirst = low + span * theEntity.trimFirst;
            theLast = low + span * theEntity.trimLast;
            return theLast - theFirst > kTiny;
        }

        case SketchEntity::Kind::Point:
            return false;
    }
    return false;
}

Handle(Geom_Curve) To3dCurve(const Handle(Geom2d_Curve)& theCurve, const gp_Ax3& thePosition)
{
    if (theCurve.IsNull()) {
        return Handle(Geom_Curve)();
    }

    try {
        if (Handle(Geom2d_TrimmedCurve) trimmed =
                Handle(Geom2d_TrimmedCurve)::DownCast(theCurve)) {
            return To3dCurve(trimmed->BasisCurve(), thePosition);
        }

        if (Handle(Geom2d_Line) line = Handle(Geom2d_Line)::DownCast(theCurve)) {
            const gp_Lin2d lin = line->Lin2d();
            const gp_Vec direction = To3dVector(gp_Vec2d(lin.Direction()), thePosition);
            if (direction.SquareMagnitude() <= kTiny) {
                return Handle(Geom_Curve)();
            }
            return new Geom_Line(To3dPoint(lin.Location(), thePosition), gp_Dir(direction));
        }

        if (Handle(Geom2d_Circle) circle = Handle(Geom2d_Circle)::DownCast(theCurve)) {
            const gp_Circ2d circ = circle->Circ2d();
            const gp_Vec xDir = To3dVector(gp_Vec2d(circ.XAxis().Direction()), thePosition);
            const gp_Vec yDir = To3dVector(gp_Vec2d(circ.YAxis().Direction()), thePosition);
            // Normal from the 2D frame's own handedness, so an indirect
            // circle keeps sweeping the way its parameters say it does.
            const gp_Vec normal = xDir.Crossed(yDir);
            if (normal.SquareMagnitude() <= kTiny) {
                return Handle(Geom_Curve)();
            }
            const gp_Ax2 axis(To3dPoint(circ.Location(), thePosition), gp_Dir(normal),
                              gp_Dir(xDir));
            return new Geom_Circle(axis, circ.Radius());
        }

        if (Handle(Geom2d_Ellipse) ellipse = Handle(Geom2d_Ellipse)::DownCast(theCurve)) {
            const gp_Elips2d elips = ellipse->Elips2d();
            const gp_Vec xDir = To3dVector(gp_Vec2d(elips.XAxis().Direction()), thePosition);
            const gp_Vec yDir = To3dVector(gp_Vec2d(elips.YAxis().Direction()), thePosition);
            const gp_Vec normal = xDir.Crossed(yDir);
            if (normal.SquareMagnitude() <= kTiny) {
                return Handle(Geom_Curve)();
            }
            const gp_Ax2 axis(To3dPoint(elips.Location(), thePosition), gp_Dir(normal),
                              gp_Dir(xDir));
            return new Geom_Ellipse(axis, elips.MajorRadius(), elips.MinorRadius());
        }

        if (Handle(Geom2d_BSplineCurve) spline =
                Handle(Geom2d_BSplineCurve)::DownCast(theCurve)) {
            const Standard_Integer poleCount = spline->NbPoles();
            const Standard_Integer knotCount = spline->NbKnots();

            TColgp_Array1OfPnt poles(1, poleCount);
            TColStd_Array1OfReal weights(1, poleCount);
            for (Standard_Integer i = 1; i <= poleCount; ++i) {
                poles.SetValue(i, To3dPoint(spline->Pole(i), thePosition));
                weights.SetValue(i, spline->Weight(i));
            }

            TColStd_Array1OfReal knots(1, knotCount);
            TColStd_Array1OfInteger mults(1, knotCount);
            for (Standard_Integer i = 1; i <= knotCount; ++i) {
                knots.SetValue(i, spline->Knot(i));
                mults.SetValue(i, spline->Multiplicity(i));
            }

            // Poles map through the plane frame one for one, so the 3D
            // curve carries the 2D parameterisation over untouched -- that
            // is what lets a parameter measured in 2D trim the 3D edge.
            return new Geom_BSplineCurve(poles, weights, knots, mults, spline->Degree(),
                                         spline->IsPeriodic());
        }
    } catch (const Standard_Failure&) {
        return Handle(Geom_Curve)();
    }

    return Handle(Geom_Curve)();
}

gp_Pnt2d PointAt(const SketchEntity& theEntity, double theParam)
{
    switch (theEntity.kind) {
        case SketchEntity::Kind::Line: {
            gp_Pnt2d origin;
            gp_Vec2d direction;
            double length = 0.0;
            if (!LineFrame(theEntity, origin, direction, length)) {
                return theEntity.first;
            }
            return gp_Pnt2d(origin.X() + direction.X() * theParam,
                            origin.Y() + direction.Y() * theParam);
        }

        case SketchEntity::Kind::Circle:
        case SketchEntity::Kind::Arc:
            return gp_Pnt2d(theEntity.first.X() + theEntity.radius * std::cos(theParam),
                            theEntity.first.Y() + theEntity.radius * std::sin(theParam));

        case SketchEntity::Kind::Ellipse:
            return EllipsePoint(theEntity, theParam);

        case SketchEntity::Kind::Spline: {
            const Handle(Geom2d_Curve) curve = Curve2dOf(theEntity);
            if (curve.IsNull()) {
                return theEntity.points.empty() ? theEntity.first : theEntity.points.front();
            }
            const double clamped = std::min(std::max(theParam, curve->FirstParameter()),
                                            curve->LastParameter());
            return curve->Value(clamped);
        }

        case SketchEntity::Kind::Point:
            return theEntity.first;
    }
    return theEntity.first;
}

gp_Vec2d TangentAt(const SketchEntity& theEntity, double theParam)
{
    switch (theEntity.kind) {
        case SketchEntity::Kind::Line: {
            gp_Pnt2d origin;
            gp_Vec2d direction;
            double length = 0.0;
            if (!LineFrame(theEntity, origin, direction, length)) {
                return gp_Vec2d(1.0, 0.0);
            }
            return direction;
        }

        case SketchEntity::Kind::Circle:
        case SketchEntity::Kind::Arc:
            return gp_Vec2d(-theEntity.radius * std::sin(theParam),
                            theEntity.radius * std::cos(theParam));

        case SketchEntity::Kind::Ellipse:
            return EllipseTangent(theEntity, theParam);

        case SketchEntity::Kind::Spline: {
            const Handle(Geom2d_Curve) curve = Curve2dOf(theEntity);
            if (curve.IsNull()) {
                return gp_Vec2d(1.0, 0.0);
            }
            const double clamped = std::min(std::max(theParam, curve->FirstParameter()),
                                            curve->LastParameter());
            gp_Pnt2d point;
            gp_Vec2d tangent;
            curve->D1(clamped, point, tangent);
            return tangent;
        }

        case SketchEntity::Kind::Point:
            return gp_Vec2d(1.0, 0.0);
    }
    return gp_Vec2d(1.0, 0.0);
}

bool NearestParam(const SketchEntity& theEntity,
                  const gp_Pnt2d&     thePoint,
                  double&             theParam,
                  bool                theClamp)
{
    double first = 0.0, last = 0.0;
    if (!ParamRange(theEntity, first, last)) {
        if (theEntity.kind == SketchEntity::Kind::Point) {
            theParam = 0.0;
            return true;
        }
        return false;
    }

    switch (theEntity.kind) {
        case SketchEntity::Kind::Line: {
            gp_Pnt2d origin;
            gp_Vec2d direction;
            double length = 0.0;
            if (!LineFrame(theEntity, origin, direction, length)) {
                return false;
            }
            const gp_Vec2d delta(thePoint.X() - origin.X(), thePoint.Y() - origin.Y());
            theParam = delta.Dot(direction);
            if (theClamp) {
                theParam = std::min(std::max(theParam, first), last);
            }
            return true;
        }

        case SketchEntity::Kind::Circle:
        case SketchEntity::Kind::Arc: {
            double angle = AngleOf(theEntity.first, thePoint);
            if (!theClamp) {
                theParam = angle;
                return true;
            }
            double candidate = angle;
            if (InWindow(theEntity, candidate)) {
                theParam = candidate;
                return true;
            }
            // Outside the sweep: the nearest point of the arc is whichever
            // end it fell past.
            const double toFirst = std::fabs(std::remainder(angle - first, kTwoPi));
            const double toLast = std::fabs(std::remainder(angle - last, kTwoPi));
            theParam = toFirst <= toLast ? first : last;
            return true;
        }

        case SketchEntity::Kind::Ellipse:
        case SketchEntity::Kind::Spline: {
            const Handle(Geom2d_Curve) curve = Curve2dOf(theEntity);
            if (curve.IsNull()) {
                return false;
            }
            return SampleNearest(curve, first, last, thePoint, theParam);
        }

        case SketchEntity::Kind::Point:
            theParam = 0.0;
            return true;
    }
    return false;
}

double DistanceTo(const SketchEntity& theEntity, const gp_Pnt2d& thePoint, double& theParam)
{
    theParam = 0.0;
    if (theEntity.kind == SketchEntity::Kind::Point) {
        return theEntity.first.Distance(thePoint);
    }
    if (!NearestParam(theEntity, thePoint, theParam, true)) {
        return std::numeric_limits<double>::max();
    }
    return PointAt(theEntity, theParam).Distance(thePoint);
}

std::vector<Intersection> Intersect(const SketchEntity& theFirst,
                                    const SketchEntity& theSecond,
                                    bool                theWholeCurves)
{
    std::vector<Intersection> result;

    const Handle(Geom2d_Curve) curveA = Curve2dOf(theFirst);
    const Handle(Geom2d_Curve) curveB = Curve2dOf(theSecond);
    if (curveA.IsNull() || curveB.IsNull()) {
        return result;
    }

    try {
        Geom2dAPI_InterCurveCurve intersector(curveA, curveB, kTolerance);
        const Standard_Integer count = intersector.NbPoints();
        result.reserve(static_cast<std::size_t>(count));

        for (Standard_Integer i = 1; i <= count; ++i) {
            const IntRes2d_IntersectionPoint& point = intersector.Intersector().Point(i);
            Intersection hit;
            hit.paramA = point.ParamOnFirst();
            hit.paramB = point.ParamOnSecond();

            if (!theWholeCurves) {
                if (!InWindow(theFirst, hit.paramA) || !InWindow(theSecond, hit.paramB)) {
                    continue;
                }
            }

            hit.point = PointAt(theFirst, hit.paramA);
            result.push_back(hit);
        }
    } catch (const Standard_Failure&) {
        result.clear();
    }

    return result;
}

// ---- trimming ----

bool PieceBetweenCrossings(const SketchEntity&              theEntity,
                           const std::vector<SketchEntity>& theOthers,
                           double                           theParam,
                           double&                          theFrom,
                           double&                          theTo)
{
    double first = 0.0, last = 0.0;
    if (!ParamRange(theEntity, first, last)) {
        return false;
    }

    std::vector<double> cuts;
    for (const SketchEntity& other : theOthers) {
        if (other.id == theEntity.id || !other.IsCurve() || other.IsDegenerate()) {
            continue;
        }
        for (const Intersection& hit : Intersect(theEntity, other, false)) {
            cuts.push_back(hit.paramA);
        }
    }
    std::sort(cuts.begin(), cuts.end());

    const double period = PeriodOf(theEntity);
    if (theEntity.IsSelfClosed() && period > 0.0) {
        // A closed curve has no ends to fall back on: without at least two
        // crossings the whole thing goes.
        if (cuts.size() < 2) {
            theFrom = first;
            theTo = last;
            return true;
        }
        for (std::size_t i = 0; i < cuts.size(); ++i) {
            const double low = cuts[i];
            const double high = (i + 1 < cuts.size()) ? cuts[i + 1] : cuts.front() + period;
            double param = theParam;
            while (param < low - kTolerance) {
                param += period;
            }
            if (param <= high + kTolerance) {
                theFrom = low;
                theTo = high;
                return true;
            }
        }
        theFrom = cuts.back();
        theTo = cuts.front() + period;
        return true;
    }

    double low = first;
    double high = last;
    for (const double cut : cuts) {
        if (cut <= theParam + kTolerance && cut > low) {
            low = cut;
        }
        if (cut >= theParam - kTolerance && cut < high) {
            high = cut;
        }
    }
    if (high - low <= kTolerance) {
        return false;
    }
    theFrom = low;
    theTo = high;
    return true;
}

bool SetParamWindow(SketchEntity& theEntity, double theFirst, double theLast)
{
    if (theLast - theFirst <= kTolerance) {
        return false;
    }

    switch (theEntity.kind) {
        case SketchEntity::Kind::Line: {
            const gp_Pnt2d start = PointAt(theEntity, theFirst);
            const gp_Pnt2d end = PointAt(theEntity, theLast);
            if (start.SquareDistance(end) <= kTolerance * kTolerance) {
                return false;
            }
            theEntity.first = start;
            theEntity.second = end;
            return true;
        }

        case SketchEntity::Kind::Circle:
        case SketchEntity::Kind::Arc: {
            // A trimmed circle is an arc; that is the whole point of Trim.
            const SketchEntity arc =
                SketchEntity::MakeArc(theEntity.first, theEntity.radius, theFirst, theLast);
            if (arc.IsDegenerate()) {
                return false;
            }
            theEntity.kind = SketchEntity::Kind::Arc;
            theEntity.startAngle = arc.startAngle;
            theEntity.endAngle = arc.endAngle;
            return true;
        }

        case SketchEntity::Kind::Ellipse: {
            const SketchEntity ellipse =
                SketchEntity::MakeEllipse(theEntity.first, theEntity.radius,
                                          theEntity.minorRadius, theEntity.rotation,
                                          theFirst, theLast);
            if (ellipse.IsDegenerate()) {
                return false;
            }
            theEntity.startAngle = ellipse.startAngle;
            theEntity.endAngle = ellipse.endAngle;
            return true;
        }

        case SketchEntity::Kind::Spline: {
            const Handle(Geom2d_Curve) curve = Curve2dOf(theEntity);
            if (curve.IsNull()) {
                return false;
            }
            const double low = curve->FirstParameter();
            const double span = curve->LastParameter() - low;
            if (span <= kTiny) {
                return false;
            }
            const double newFirst = std::min(std::max((theFirst - low) / span, 0.0), 1.0);
            const double newLast = std::min(std::max((theLast - low) / span, 0.0), 1.0);
            if (newLast - newFirst <= 1.0e-6) {
                return false;
            }
            theEntity.trimFirst = newFirst;
            theEntity.trimLast = newLast;
            return true;
        }

        case SketchEntity::Kind::Point:
            return false;
    }
    return false;
}

std::vector<SketchEntity> RemoveRange(const SketchEntity& theEntity,
                                      double              theFrom,
                                      double              theTo)
{
    std::vector<SketchEntity> pieces;

    double first = 0.0, last = 0.0;
    if (!ParamRange(theEntity, first, last)) {
        return pieces;
    }

    double from = std::min(theFrom, theTo);
    double to = std::max(theFrom, theTo);

    // On a closed curve there is no "outside", so cutting a piece out
    // always leaves exactly the complementary arc.
    if (theEntity.IsSelfClosed() && PeriodOf(theEntity) > 0.0) {
        SketchEntity remainder = theEntity;
        if (SetParamWindow(remainder, to, from + PeriodOf(theEntity))) {
            pieces.push_back(remainder);
        }
        return pieces;
    }

    from = std::min(std::max(from, first), last);
    to = std::min(std::max(to, first), last);

    if (from - first > kTolerance) {
        SketchEntity head = theEntity;
        if (SetParamWindow(head, first, from)) {
            pieces.push_back(head);
        }
    }
    if (last - to > kTolerance) {
        SketchEntity tail = theEntity;
        tail.id = 0;  // the second half is new geometry and needs its own id
        if (SetParamWindow(tail, to, last)) {
            pieces.push_back(tail);
        }
    }

    return pieces;
}

// ---- transforms ----

SketchEntity Translated(const SketchEntity& theEntity, const gp_Vec2d& theOffset)
{
    SketchEntity result = theEntity;
    result.first.Translate(theOffset);
    result.second.Translate(theOffset);
    for (gp_Pnt2d& point : result.points) {
        point.Translate(theOffset);
    }
    return result;
}

SketchEntity Rotated(const SketchEntity& theEntity, const gp_Pnt2d& theCentre, double theAngle)
{
    SketchEntity result = theEntity;
    result.first.Rotate(theCentre, theAngle);
    result.second.Rotate(theCentre, theAngle);
    for (gp_Pnt2d& point : result.points) {
        point.Rotate(theCentre, theAngle);
    }

    switch (theEntity.kind) {
        case SketchEntity::Kind::Circle:
            break;  // a circle looks the same from every angle
        case SketchEntity::Kind::Arc:
            result.startAngle += theAngle;
            result.endAngle += theAngle;
            break;
        case SketchEntity::Kind::Ellipse:
            result.rotation += theAngle;
            break;
        case SketchEntity::Kind::Line:
        case SketchEntity::Kind::Spline:
        case SketchEntity::Kind::Point:
            break;
    }
    return result;
}

SketchEntity Mirrored(const SketchEntity& theEntity,
                      const gp_Pnt2d&     theFirst,
                      const gp_Pnt2d&     theSecond)
{
    gp_Vec2d axis(theSecond.X() - theFirst.X(), theSecond.Y() - theFirst.Y());
    if (axis.SquareMagnitude() <= kTiny) {
        return theEntity;
    }
    axis.Normalize();
    const double axisAngle = std::atan2(axis.Y(), axis.X());

    auto reflect = [&](const gp_Pnt2d& thePoint) {
        const gp_Vec2d delta(thePoint.X() - theFirst.X(), thePoint.Y() - theFirst.Y());
        const double along = delta.Dot(axis);
        const gp_Vec2d reflected = axis * (2.0 * along) - delta;
        return gp_Pnt2d(theFirst.X() + reflected.X(), theFirst.Y() + reflected.Y());
    };

    SketchEntity result = theEntity;
    result.id = 0;

    switch (theEntity.kind) {
        case SketchEntity::Kind::Line:
        case SketchEntity::Kind::Point:
            result.first = reflect(theEntity.first);
            result.second = reflect(theEntity.second);
            break;

        case SketchEntity::Kind::Circle:
            result.first = reflect(theEntity.first);
            break;

        case SketchEntity::Kind::Arc: {
            // A reflection reverses the sweep, so the mirrored arc runs
            // from the image of the old end to the image of the old start.
            const gp_Pnt2d centre = reflect(theEntity.first);
            const gp_Pnt2d start = reflect(theEntity.EndPoint());
            const gp_Pnt2d end = reflect(theEntity.StartPoint());
            result = CopyIdentity(theEntity,
                                  SketchEntity::MakeArc(centre, theEntity.radius,
                                                        AngleOf(centre, start),
                                                        AngleOf(centre, end)));
            result.id = 0;
            break;
        }

        case SketchEntity::Kind::Ellipse: {
            const gp_Pnt2d centre = reflect(theEntity.first);
            const double rotation = 2.0 * axisAngle - theEntity.rotation;
            // Eccentric angles negate in the reflected frame, which flips
            // the sweep the same way the arc case does.
            result = CopyIdentity(theEntity,
                                  SketchEntity::MakeEllipse(centre, theEntity.radius,
                                                            theEntity.minorRadius, rotation,
                                                            -theEntity.endAngle,
                                                            -theEntity.startAngle));
            result.id = 0;
            break;
        }

        case SketchEntity::Kind::Spline:
            for (gp_Pnt2d& point : result.points) {
                point = reflect(point);
            }
            break;
    }

    return result;
}

bool Offset(const SketchEntity& theEntity, double theDistance, SketchEntity& theResult)
{
    if (std::fabs(theDistance) <= kTolerance) {
        theResult = theEntity;
        theResult.id = 0;
        return true;
    }

    switch (theEntity.kind) {
        case SketchEntity::Kind::Line: {
            gp_Pnt2d origin;
            gp_Vec2d direction;
            double length = 0.0;
            if (!LineFrame(theEntity, origin, direction, length)) {
                return false;
            }
            const gp_Vec2d normal = Perpendicular(direction) * theDistance;
            theResult = SketchEntity::MakeLine(theEntity.first.Translated(normal),
                                               theEntity.second.Translated(normal));
            theResult.isConstruction = theEntity.isConstruction;
            return true;
        }

        case SketchEntity::Kind::Circle:
        case SketchEntity::Kind::Arc: {
            // The left normal of a counter-clockwise circle points at the
            // centre, so offsetting left shrinks it.
            const double radius = theEntity.radius - theDistance;
            if (radius <= kTolerance) {
                return false;
            }
            if (theEntity.kind == SketchEntity::Kind::Circle) {
                theResult = SketchEntity::MakeCircle(theEntity.first, radius);
            } else {
                theResult = SketchEntity::MakeArc(theEntity.first, radius, theEntity.startAngle,
                                                  theEntity.endAngle);
            }
            theResult.isConstruction = theEntity.isConstruction;
            return true;
        }

        case SketchEntity::Kind::Ellipse:
        case SketchEntity::Kind::Spline:
            return RefitOffsetAsSpline(theEntity, theDistance, theResult);

        case SketchEntity::Kind::Point:
            return false;
    }
    return false;
}

// ---- constructions ----

std::vector<SketchEntity> RectangleTwoPoint(const gp_Pnt2d& theCorner,
                                            const gp_Pnt2d& theOpposite)
{
    const gp_Pnt2d a = theCorner;
    const gp_Pnt2d b(theOpposite.X(), theCorner.Y());
    const gp_Pnt2d c = theOpposite;
    const gp_Pnt2d d(theCorner.X(), theOpposite.Y());
    return {SketchEntity::MakeLine(a, b),
            SketchEntity::MakeLine(b, c),
            SketchEntity::MakeLine(c, d),
            SketchEntity::MakeLine(d, a)};
}

std::vector<SketchEntity> RectangleCentre(const gp_Pnt2d& theCentre, const gp_Pnt2d& theCorner)
{
    const double halfWidth = std::fabs(theCorner.X() - theCentre.X());
    const double halfHeight = std::fabs(theCorner.Y() - theCentre.Y());
    return RectangleTwoPoint(
        gp_Pnt2d(theCentre.X() - halfWidth, theCentre.Y() - halfHeight),
        gp_Pnt2d(theCentre.X() + halfWidth, theCentre.Y() + halfHeight));
}

std::vector<SketchEntity> RectangleThreePoint(const gp_Pnt2d& theFirst,
                                              const gp_Pnt2d& theSecond,
                                              const gp_Pnt2d& theThird)
{
    gp_Vec2d along(theSecond.X() - theFirst.X(), theSecond.Y() - theFirst.Y());
    if (along.SquareMagnitude() <= kTolerance * kTolerance) {
        return {};
    }
    along.Normalize();

    const gp_Vec2d across = Perpendicular(along);
    const gp_Vec2d toThird(theThird.X() - theSecond.X(), theThird.Y() - theSecond.Y());
    const double width = toThird.Dot(across);
    if (std::fabs(width) <= kTolerance) {
        return {};
    }

    const gp_Vec2d offset = across * width;
    const gp_Pnt2d a = theFirst;
    const gp_Pnt2d b = theSecond;
    const gp_Pnt2d c = theSecond.Translated(offset);
    const gp_Pnt2d d = theFirst.Translated(offset);
    return {SketchEntity::MakeLine(a, b),
            SketchEntity::MakeLine(b, c),
            SketchEntity::MakeLine(c, d),
            SketchEntity::MakeLine(d, a)};
}

namespace {

std::vector<SketchEntity> PolygonFromVertices(const std::vector<gp_Pnt2d>& theVertices)
{
    std::vector<SketchEntity> sides;
    if (theVertices.size() < 3) {
        return sides;
    }
    sides.reserve(theVertices.size());
    for (std::size_t i = 0; i < theVertices.size(); ++i) {
        const gp_Pnt2d& a = theVertices[i];
        const gp_Pnt2d& b = theVertices[(i + 1) % theVertices.size()];
        if (a.SquareDistance(b) <= kTolerance * kTolerance) {
            return {};
        }
        sides.push_back(SketchEntity::MakeLine(a, b));
    }
    return sides;
}

} // namespace

std::vector<SketchEntity> PolygonInscribed(const gp_Pnt2d& theCentre,
                                           const gp_Pnt2d& theReference,
                                           int             theSides)
{
    if (theSides < 3) {
        return {};
    }
    const double radius = theCentre.Distance(theReference);
    if (radius <= kTolerance) {
        return {};
    }

    const double start = AngleOf(theCentre, theReference);
    const double step = kTwoPi / theSides;

    std::vector<gp_Pnt2d> vertices;
    vertices.reserve(static_cast<std::size_t>(theSides));
    for (int i = 0; i < theSides; ++i) {
        const double angle = start + step * i;
        vertices.push_back(gp_Pnt2d(theCentre.X() + radius * std::cos(angle),
                                    theCentre.Y() + radius * std::sin(angle)));
    }
    return PolygonFromVertices(vertices);
}

std::vector<SketchEntity> PolygonCircumscribed(const gp_Pnt2d& theCentre,
                                               const gp_Pnt2d& theReference,
                                               int             theSides)
{
    if (theSides < 3) {
        return {};
    }
    const double apothem = theCentre.Distance(theReference);
    if (apothem <= kTolerance) {
        return {};
    }

    // The reference is an edge midpoint, so the vertices sit half a step
    // either side of it on the larger circumscribing circle.
    const double step = kTwoPi / theSides;
    const double radius = apothem / std::cos(step * 0.5);
    const double start = AngleOf(theCentre, theReference) - step * 0.5;

    std::vector<gp_Pnt2d> vertices;
    vertices.reserve(static_cast<std::size_t>(theSides));
    for (int i = 0; i < theSides; ++i) {
        const double angle = start + step * i;
        vertices.push_back(gp_Pnt2d(theCentre.X() + radius * std::cos(angle),
                                    theCentre.Y() + radius * std::sin(angle)));
    }
    return PolygonFromVertices(vertices);
}

std::vector<SketchEntity> PolygonOnEdge(const gp_Pnt2d& theFirst,
                                        const gp_Pnt2d& theSecond,
                                        int             theSides)
{
    if (theSides < 3) {
        return {};
    }
    const double side = theFirst.Distance(theSecond);
    if (side <= kTolerance) {
        return {};
    }

    // Walk the edge round, turning by the exterior angle at every corner.
    const double exterior = kTwoPi / theSides;
    double heading = AngleOf(theFirst, theSecond);

    std::vector<gp_Pnt2d> vertices;
    vertices.reserve(static_cast<std::size_t>(theSides));
    gp_Pnt2d current = theFirst;
    for (int i = 0; i < theSides; ++i) {
        vertices.push_back(current);
        current = gp_Pnt2d(current.X() + side * std::cos(heading),
                           current.Y() + side * std::sin(heading));
        heading += exterior;
    }
    return PolygonFromVertices(vertices);
}

std::vector<SketchEntity> Slot(const gp_Pnt2d& theFirstCentre,
                               const gp_Pnt2d& theSecondCentre,
                               double          theWidth)
{
    const double radius = std::fabs(theWidth) * 0.5;
    if (radius <= kTolerance) {
        return {};
    }

    gp_Vec2d along(theSecondCentre.X() - theFirstCentre.X(),
                   theSecondCentre.Y() - theFirstCentre.Y());
    if (along.SquareMagnitude() <= kTolerance * kTolerance) {
        return {};
    }
    along.Normalize();

    const gp_Vec2d across = Perpendicular(along) * radius;
    const double axisAngle = std::atan2(along.Y(), along.X());

    const gp_Pnt2d leftStart = theFirstCentre.Translated(across);
    const gp_Pnt2d leftEnd = theSecondCentre.Translated(across);
    const gp_Pnt2d rightStart = theFirstCentre.Translated(-across);
    const gp_Pnt2d rightEnd = theSecondCentre.Translated(-across);

    // Flanks run in opposite directions so the four pieces chain into one
    // closed loop, which is what makes a slot extrudable.
    return {SketchEntity::MakeLine(leftStart, leftEnd),
            SketchEntity::MakeArc(theSecondCentre, radius, axisAngle + kPi * 0.5,
                                  axisAngle - kPi * 0.5),
            SketchEntity::MakeLine(rightEnd, rightStart),
            SketchEntity::MakeArc(theFirstCentre, radius, axisAngle - kPi * 0.5,
                                  axisAngle + kPi * 0.5)};
}

bool CircleTwoPoint(const gp_Pnt2d& theFirst, const gp_Pnt2d& theSecond, SketchEntity& theResult)
{
    const double diameter = theFirst.Distance(theSecond);
    if (diameter <= kTolerance) {
        return false;
    }
    const gp_Pnt2d centre((theFirst.X() + theSecond.X()) * 0.5,
                          (theFirst.Y() + theSecond.Y()) * 0.5);
    theResult = SketchEntity::MakeCircle(centre, diameter * 0.5);
    return true;
}

namespace {

// Circumcentre of three points, or false when they are (near-)collinear.
bool Circumcentre(const gp_Pnt2d& theA,
                  const gp_Pnt2d& theB,
                  const gp_Pnt2d& theC,
                  gp_Pnt2d&       theCentre,
                  double&         theRadius)
{
    const double ax = theA.X(), ay = theA.Y();
    const double bx = theB.X(), by = theB.Y();
    const double cx = theC.X(), cy = theC.Y();

    const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (std::fabs(d) <= kTolerance) {
        return false;
    }

    const double a2 = ax * ax + ay * ay;
    const double b2 = bx * bx + by * by;
    const double c2 = cx * cx + cy * cy;

    theCentre = gp_Pnt2d((a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d,
                         (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d);
    theRadius = theCentre.Distance(theA);
    return theRadius > kTolerance;
}

// True when sweeping counter-clockwise from theFrom reaches theMiddle
// before theTo -- which tells an arc through three points which way round
// it has to go.
bool SweepPasses(double theFrom, double theMiddle, double theTo)
{
    auto normalise = [](double theAngle) {
        double value = theAngle;
        while (value < 0.0) {
            value += kTwoPi;
        }
        while (value >= kTwoPi) {
            value -= kTwoPi;
        }
        return value;
    };
    return normalise(theMiddle - theFrom) < normalise(theTo - theFrom);
}

} // namespace

bool CircleThreePoint(const gp_Pnt2d& theFirst,
                      const gp_Pnt2d& theSecond,
                      const gp_Pnt2d& theThird,
                      SketchEntity&   theResult)
{
    gp_Pnt2d centre;
    double radius = 0.0;
    if (!Circumcentre(theFirst, theSecond, theThird, centre, radius)) {
        return false;
    }
    theResult = SketchEntity::MakeCircle(centre, radius);
    return true;
}

bool ArcThreePoint(const gp_Pnt2d& theFirst,
                   const gp_Pnt2d& theSecond,
                   const gp_Pnt2d& theThird,
                   SketchEntity&   theResult)
{
    gp_Pnt2d centre;
    double radius = 0.0;
    if (!Circumcentre(theFirst, theSecond, theThird, centre, radius)) {
        return false;
    }

    const double from = AngleOf(centre, theFirst);
    const double middle = AngleOf(centre, theSecond);
    const double to = AngleOf(centre, theThird);

    // Arcs always sweep counter-clockwise, so when the middle point is on
    // the other side the arc is stored running the other way instead.
    theResult = SweepPasses(from, middle, to)
                    ? SketchEntity::MakeArc(centre, radius, from, to)
                    : SketchEntity::MakeArc(centre, radius, to, from);
    return !theResult.IsDegenerate();
}

bool ArcTangent(const gp_Pnt2d& theStart,
                const gp_Vec2d& theDirection,
                const gp_Pnt2d& theEnd,
                SketchEntity&   theResult)
{
    gp_Vec2d direction = theDirection;
    if (direction.SquareMagnitude() <= kTiny) {
        return false;
    }
    direction.Normalize();

    // The centre is on the normal at the start and equidistant from both
    // points; that pins it down to a single solution.
    const gp_Vec2d normal = Perpendicular(direction);
    const gp_Vec2d toStart(theStart.X() - theEnd.X(), theStart.Y() - theEnd.Y());
    const double denominator = 2.0 * toStart.Dot(normal);
    if (std::fabs(denominator) <= kTolerance) {
        return false;  // the end point lies straight ahead: no finite arc
    }

    const double along = -toStart.SquareMagnitude() / denominator;
    const gp_Pnt2d centre = theStart.Translated(normal * along);
    const double radius = std::fabs(along);
    if (radius <= kTolerance) {
        return false;
    }

    const double startAngle = AngleOf(centre, theStart);
    const double endAngle = AngleOf(centre, theEnd);

    // Pick the sweep whose tangent at the start actually points the way
    // the user came from.
    const gp_Vec2d radial(theStart.X() - centre.X(), theStart.Y() - centre.Y());
    const bool counterClockwise = Perpendicular(radial).Dot(direction) > 0.0;

    theResult = counterClockwise
                    ? SketchEntity::MakeArc(centre, radius, startAngle, endAngle)
                    : SketchEntity::MakeArc(centre, radius, endAngle, startAngle);
    return !theResult.IsDegenerate();
}

// ---- fillet ----

namespace {

// Where two lines' infinite extensions cross, plus the unit direction
// from that corner toward the half of each line the user wants kept.
bool CornerOf(const SketchEntity& theFirst,
              const SketchEntity& theSecond,
              const gp_Pnt2d&     theNearFirst,
              const gp_Pnt2d&     theNearSecond,
              gp_Pnt2d&           theCorner,
              gp_Vec2d&           theFirstDirection,
              gp_Vec2d&           theSecondDirection)
{
    if (theFirst.kind != SketchEntity::Kind::Line
     || theSecond.kind != SketchEntity::Kind::Line) {
        return false;
    }

    const std::vector<Intersection> hits = Intersect(theFirst, theSecond, true);
    if (hits.empty()) {
        return false;  // parallel
    }
    theCorner = hits.front().point;

    // Which half of each line to keep. When the reference point IS the
    // corner -- which is exactly what two lines already meeting at one
    // look like -- there is only one half to keep, so take it.
    auto keptDirection = [&theCorner](const SketchEntity& theLine, const gp_Pnt2d& theNear,
                                      gp_Vec2d& theResult) {
        gp_Vec2d toNear(theNear.X() - theCorner.X(), theNear.Y() - theCorner.Y());
        if (toNear.SquareMagnitude() <= kTolerance * kTolerance) {
            const gp_Pnt2d far = theCorner.SquareDistance(theLine.first)
                                         >= theCorner.SquareDistance(theLine.second)
                                     ? theLine.first
                                     : theLine.second;
            toNear = gp_Vec2d(far.X() - theCorner.X(), far.Y() - theCorner.Y());
        }
        if (toNear.SquareMagnitude() <= kTolerance * kTolerance) {
            return false;
        }
        toNear.Normalize();
        theResult = toNear;
        return true;
    };

    return keptDirection(theFirst, theNearFirst, theFirstDirection)
        && keptDirection(theSecond, theNearSecond, theSecondDirection);
}

// Length of theLine measured from theCorner toward theDirection -- how
// much leg the fillet has to eat into.
double LegLength(const SketchEntity& theLine, const gp_Pnt2d& theCorner,
                 const gp_Vec2d& theDirection)
{
    const gp_Vec2d toStart(theLine.first.X() - theCorner.X(), theLine.first.Y() - theCorner.Y());
    const gp_Vec2d toEnd(theLine.second.X() - theCorner.X(), theLine.second.Y() - theCorner.Y());
    return std::max(std::max(toStart.Dot(theDirection), toEnd.Dot(theDirection)), 0.0);
}

} // namespace

double MaximumFilletRadius(const SketchEntity& theFirst,
                           const SketchEntity& theSecond,
                           const gp_Pnt2d&     theNearFirst,
                           const gp_Pnt2d&     theNearSecond)
{
    gp_Pnt2d corner;
    gp_Vec2d firstDirection, secondDirection;
    if (!CornerOf(theFirst, theSecond, theNearFirst, theNearSecond, corner, firstDirection,
                  secondDirection)) {
        return 0.0;
    }

    const double cosine = std::min(std::max(firstDirection.Dot(secondDirection), -1.0), 1.0);
    const double half = std::acos(cosine) * 0.5;
    if (half <= kTolerance || half >= kPi * 0.5 - kTolerance) {
        return 0.0;
    }

    const double leg = std::min(LegLength(theFirst, corner, firstDirection),
                                LegLength(theSecond, corner, secondDirection));
    return leg * std::tan(half);
}

bool FilletLines(const SketchEntity& theFirst,
                 const SketchEntity& theSecond,
                 const gp_Pnt2d&     theNearFirst,
                 const gp_Pnt2d&     theNearSecond,
                 double              theRadius,
                 FilletResult&       theResult)
{
    if (theRadius <= kTolerance) {
        return false;
    }

    gp_Pnt2d corner;
    gp_Vec2d firstDirection, secondDirection;
    if (!CornerOf(theFirst, theSecond, theNearFirst, theNearSecond, corner, firstDirection,
                  secondDirection)) {
        return false;
    }

    const double cosine = std::min(std::max(firstDirection.Dot(secondDirection), -1.0), 1.0);
    const double half = std::acos(cosine) * 0.5;
    if (half <= kTolerance || half >= kPi * 0.5 - kTolerance) {
        return false;  // the lines are parallel or on top of each other
    }

    // Standard corner-rounding: the tangent points sit r/tan(half) back
    // along each leg, the centre r/sin(half) along the bisector.
    const double setback = theRadius / std::tan(half);
    if (setback > LegLength(theFirst, corner, firstDirection) + kTolerance
     || setback > LegLength(theSecond, corner, secondDirection) + kTolerance) {
        return false;  // radius too big for the lines it has to fit between
    }

    const gp_Pnt2d tangentFirst = corner.Translated(firstDirection * setback);
    const gp_Pnt2d tangentSecond = corner.Translated(secondDirection * setback);

    gp_Vec2d bisector = firstDirection + secondDirection;
    if (bisector.SquareMagnitude() <= kTiny) {
        return false;
    }
    bisector.Normalize();
    const gp_Pnt2d centre = corner.Translated(bisector * (theRadius / std::sin(half)));

    const double angleFirst = AngleOf(centre, tangentFirst);
    const double angleSecond = AngleOf(centre, tangentSecond);

    // Take whichever sweep is the minor one; the major arc would loop all
    // the way round the outside of the corner.
    SketchEntity arc = SketchEntity::MakeArc(centre, theRadius, angleFirst, angleSecond);
    if (arc.endAngle - arc.startAngle > kPi) {
        arc = SketchEntity::MakeArc(centre, theRadius, angleSecond, angleFirst);
    }
    theResult.arc = arc;

    // Each line keeps the half the user clicked on and now stops at its
    // tangent point instead of running into the corner.
    theResult.firstTrimmed = theFirst;
    const bool firstStartKept =
        theFirst.first.SquareDistance(theNearFirst) <= theFirst.second.SquareDistance(theNearFirst);
    if (firstStartKept) {
        theResult.firstTrimmed.second = tangentFirst;
    } else {
        theResult.firstTrimmed.first = tangentFirst;
    }

    theResult.secondTrimmed = theSecond;
    const bool secondStartKept = theSecond.first.SquareDistance(theNearSecond)
                              <= theSecond.second.SquareDistance(theNearSecond);
    if (secondStartKept) {
        theResult.secondTrimmed.second = tangentSecond;
    } else {
        theResult.secondTrimmed.first = tangentSecond;
    }

    return !theResult.arc.IsDegenerate() && !theResult.firstTrimmed.IsDegenerate()
        && !theResult.secondTrimmed.IsDegenerate();
}

} // namespace SketchGeometry
} // namespace lcad
