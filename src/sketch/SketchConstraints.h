#pragma once

#include "sketch/SketchEntity.h"

#include <gp_Pnt2d.hxx>

#include <string>
#include <vector>

namespace lcad {

// Which characteristic point of an entity a constraint grabs hold of.
// Whole means "the curve itself" -- what parallel/tangent/equal want.
enum class SketchPointRole
{
    Whole,
    Start,
    End,
    Centre
};

// One end of a constraint: an entity plus which of its points is meant.
struct SketchPointRef
{
    int             entity = 0;
    SketchPointRole role   = SketchPointRole::Whole;

    bool IsValid() const { return entity != 0; }
    bool operator==(const SketchPointRef& theOther) const
    {
        return entity == theOther.entity && role == theOther.role;
    }
};

// The relationships Fusion's Constraints panel offers, plus the driving
// dimensions, which are just constraints whose target value the user can
// type. Ordered the way the ribbon lists them.
enum class SketchConstraintType
{
    Coincident,
    Horizontal,
    Vertical,
    Parallel,
    Perpendicular,
    Equal,
    Tangent,
    Midpoint,
    Concentric,
    Collinear,
    Fix,
    Symmetric,

    // Driving dimensions. Everything below this line has a value the
    // properties panel can edit; the solver then moves the geometry.
    Distance,
    DistanceX,
    DistanceY,
    Radius,
    Diameter,
    Angle
};

struct SketchConstraint
{
    SketchConstraintType type = SketchConstraintType::Coincident;

    int id = 0;

    // Operands. How many are used depends on the type:
    //   Coincident/Distance     a, b = points
    //   Horizontal/Vertical     a = line, or a, b = points
    //   Parallel/Perpendicular  a, b = lines
    //   Equal/Tangent           a, b = curves
    //   Midpoint                a = point, b = line
    //   Concentric              a, b = circles/arcs
    //   Collinear               a, b = lines
    //   Fix                     a = entity
    //   Symmetric               a, b = points, c = mirror line
    //   Radius/Diameter         a = circle/arc
    //   Angle                   a, b = lines
    SketchPointRef a;
    SketchPointRef b;
    SketchPointRef c;

    // Target value for a dimension; ignored otherwise. Lengths are mm,
    // angles radians.
    double value = 0.0;

    // "d1", "d2"... assigned when the dimension is created, and the name
    // the properties panel shows.
    std::string label;

    // Where the dimension's text sits, in sketch coordinates.
    gp_Pnt2d labelPosition;

    bool IsDimension() const { return IsDimensionType(type); }
    static bool IsDimensionType(SketchConstraintType theType)
    {
        return theType >= SketchConstraintType::Distance;
    }

    // Angles are stored in radians but shown in degrees, the only unit
    // anyone actually types.
    bool IsAngular() const { return type == SketchConstraintType::Angle; }

    // Short human name, used for the ribbon, glyphs and error messages.
    static const char* TypeName(SketchConstraintType theType);
};

// Solves a constraint system by moving the entities as little as it can.
//
// Deliberately a self-contained damped Gauss-Newton over the entities'
// own defining values rather than a third-party solver: the systems a
// sketch produces are small (tens of unknowns), and pulling in a solver
// library for them would be a dependency out of all proportion.
//
// Under-constrained systems are the normal case in a CAD sketch, so the
// step is damped toward zero, which picks the smallest motion satisfying
// the constraints -- the "minimal move" behaviour users expect when they
// drag a dimension.
class SketchSolver
{
public:
    struct Result
    {
        bool        converged = false;
        double      residual  = 0.0;   // largest remaining violation
        int         iterations = 0;
        std::string error;             // empty unless something is unusable
    };

    // Moves theEntities in place until theConstraints are satisfied.
    // Returns false when the system could not be satisfied; the entities
    // are still left at the best fit found, which is what lets a user see
    // and fix an over-constrained sketch instead of losing their work.
    static Result Solve(std::vector<SketchEntity>&           theEntities,
                        const std::vector<SketchConstraint>& theConstraints);

    // Largest constraint violation without moving anything. Used to skip
    // solving a sketch that is already satisfied.
    static double Residual(const std::vector<SketchEntity>&     theEntities,
                           const std::vector<SketchConstraint>& theConstraints);

    // Value a freshly created dimension should take: measures the current
    // geometry so adding a dimension never moves anything by itself.
    static bool MeasureDimension(const std::vector<SketchEntity>& theEntities,
                                 const SketchConstraint&          theConstraint,
                                 double&                          theValue);
};

} // namespace lcad
