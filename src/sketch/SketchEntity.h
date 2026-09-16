#pragma once

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
    enum class Kind { Line, Circle, Arc, Ellipse, Spline, Point };

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
};

} // namespace lcad
