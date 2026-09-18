#pragma once

#include "core/Entity.h"

#include <gp_Pnt2d.hxx>

#include <vector>

namespace lcad {

// One curve drawn on the sketch plane.
//
// Everything is stored in 2D plane coordinates rather than as loose 3D
// curves: that is what makes this a sketch. Move or offset the plane and
// every entity follows it, which is the whole point of sketching on a
// plane instead of drawing wires in space.
//
// Compound shapes -- rectangles, polygons, slots -- are deliberately NOT
// their own kind. They decompose into lines and arcs the moment they are
// drawn, exactly as Fusion stores them, so the wire assembler, the
// solver and every modify tool need no special case for them.
struct SketchEntity
{
    // Fusion's full set of sketch curves, minus the ones it only ever
    // creates for you: SketchFixedSpline comes from a projection and
    // cannot be drawn, and SketchText needs font outlines. An ellipse with
    // a partial sweep IS Fusion's SketchEllipticalArc -- same geometry,
    // different name -- so it needs no kind of its own; SketchEntityType
    // tells them apart.
    enum class Kind
    {
        Line,
        Circle,
        Arc,
        Ellipse,
        Spline,              // interpolates its points: Fusion's fit point spline
        ControlPointSpline,  // its points are control poles, not points on the curve
        Conic,               // start, apex and end plus a rho: ellipse, parabola or hyperbola
        Point
    };

    Kind kind = Kind::Line;

    // Stable identity, handed out by SketchFeature::AddEntity. Constraints
    // and dimensions reference entities by this rather than by index, so
    // deleting or trimming a neighbour doesn't silently re-target them.
    int id = 0;

    gp_Pnt2d first;   // Line: start point. Circle/Arc/Ellipse: centre.
                      // Point: the point itself.
    gp_Pnt2d second;  // Line: end point. Unused otherwise.

    double radius      = 0.0;  // Circle/Arc: radius. Ellipse: major radius.
    double minorRadius = 0.0;  // Ellipse only.
    double rotation    = 0.0;  // Ellipse: major-axis angle, CCW from plane X.

    double startAngle = 0.0;   // radians, CCW from the plane's X axis
    double endAngle   = 0.0;

    // Spline fit points, in order. The curve interpolates all of them.
    std::vector<gp_Pnt2d> points;

    // Conic only: how far the curve is pulled towards its apex. Below 0.5
    // it is an ellipse, exactly 0.5 a parabola, above it a hyperbola --
    // which is the whole reason one curve type covers all three.
    double rho = 0.5;

    // Spline parameter window as fractions of the full interpolated range.
    // Fractions rather than raw parameters because trimming must survive
    // re-interpolation after the solver moves a fit point.
    double trimFirst = 0.0;
    double trimLast  = 1.0;

    // Construction geometry guides other curves but never contributes to a
    // profile -- Fusion's dashed-orange lines.
    bool isConstruction = false;

    static SketchEntity MakeLine(const gp_Pnt2d& theStart, const gp_Pnt2d& theEnd);
    static SketchEntity MakeCircle(const gp_Pnt2d& theCentre, double theRadius);

    // Sweeps counter-clockwise from theStart to theEnd; the angles are
    // normalised so the arc always has a positive sweep.
    static SketchEntity MakeArc(const gp_Pnt2d& theCentre,
                                double          theRadius,
                                double          theStartAngle,
                                double          theEndAngle);

    // theRotation orients the major axis; the angles are eccentric angles
    // measured in that rotated frame and default to a full ellipse.
    static SketchEntity MakeEllipse(const gp_Pnt2d& theCentre,
                                    double          theMajorRadius,
                                    double          theMinorRadius,
                                    double          theRotation,
                                    double          theStartAngle = 0.0,
                                    double          theEndAngle   = 0.0);

    static SketchEntity MakeSpline(const std::vector<gp_Pnt2d>& thePoints);

    // The points are control poles: the curve is pulled towards them but
    // passes through only the first and last. This is the other half of
    // Fusion's spline story -- drawing through points and shaping by
    // poles are different tools, and mapping both onto one curve type
    // would make one of them behave wrongly under a drag.
    static SketchEntity MakeControlPointSpline(const std::vector<gp_Pnt2d>& thePoints);

    // theApex is where the tangents at the two ends meet. theRho runs
    // strictly between 0 and 1 and is clamped into that range.
    static SketchEntity MakeConic(const gp_Pnt2d& theStart,
                                  const gp_Pnt2d& theApex,
                                  const gp_Pnt2d& theEnd,
                                  double          theRho = 0.5);
    static SketchEntity MakePoint(const gp_Pnt2d& thePosition);

    gp_Pnt2d StartPoint() const;
    gp_Pnt2d EndPoint() const;

    // Centre for the curves that have one, otherwise a representative
    // interior point. Used by concentric/tangent constraints and glyphs.
    gp_Pnt2d CentrePoint() const;

    // True for curves that already form a loop by themselves and so never
    // take part in endpoint chaining.
    bool IsSelfClosed() const;

    // True for curves with no extent -- a zero-length line, a zero-radius
    // circle, a one-point spline. Those never produce an edge.
    bool IsDegenerate() const;

    // A bare point contributes no edge at all; it exists to be constrained
    // to (Fusion's sketch point).
    bool IsCurve() const { return kind != Kind::Point; }

    // True for the kinds whose shape comes from `points` plus a trim
    // window rather than from a centre and angles. They share every code
    // path that asks "where does this curve start" or "what parameter
    // range does it occupy".
    bool IsPointBased() const
    {
        return kind == Kind::Spline || kind == Kind::ControlPointSpline || kind == Kind::Conic;
    }
};

// The Fusion type this entity would report. A partial ellipse is a
// SketchEllipticalArc and a whole one is a SketchEllipse, which is a
// distinction the browser and any future API need to make but the
// geometry does not.
EntityType SketchEntityType(const SketchEntity& theEntity);

} // namespace lcad
