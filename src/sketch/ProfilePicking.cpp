#include "sketch/ProfilePicking.h"

#include "core/Units.h"

#include <BRep_Builder.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <cstdio>
#include <limits>

namespace lcad {

namespace {

// A ray this close to lying in the plane meets it so far away, and so
// sensitively to the last pixel, that no answer it gives means anything.
constexpr double kParallel = 1.0e-9;

// Surfaces are met to this tolerance. Well under anything the depth rule
// is sized to, so it never decides a coplanar case on its own.
constexpr double kIntersectTolerance = 1.0e-7;

// Trailing zeros trimmed and the length unit squared, exactly as
// DescribeSelection words a face -- the two lines sit in the same place
// and must read as the same kind of answer.
std::string FormatArea(double theSquareMillimetres)
{
    const LengthUnit unit  = DefaultLengthUnit();
    const double     per   = MillimetersPer(unit);
    const double     value = per > 0.0 ? theSquareMillimetres / (per * per) : theSquareMillimetres;

    char text[64];
    std::snprintf(text, sizeof(text), "%.2f", value);
    std::string out(text);
    if (out.find('.') != std::string::npos) {
        out.erase(out.find_last_not_of('0') + 1);
        if (!out.empty() && out.back() == '.') {
            out.pop_back();
        }
    }
    return out + " " + SymbolOf(unit) + "²";
}

} // namespace

bool RayMeetsPlane(const gp_Lin& theRay,
                   const gp_Ax3& thePlane,
                   double&       theDepth,
                   gp_Pnt2d&     thePoint)
{
    const gp_Vec normal(thePlane.Direction());
    const gp_Vec along(theRay.Direction());

    const double denominator = along.Dot(normal);
    if (std::fabs(denominator) < kParallel) {
        return false;
    }

    const double depth = gp_Vec(theRay.Location(), thePlane.Location()).Dot(normal) / denominator;
    if (depth < 0.0) {
        return false;
    }

    // The same mapping SketchFeature::To2d uses, so the point lands in the
    // coordinates the regions were built in.
    const gp_Pnt hit = theRay.Location().Translated(along * depth);
    const gp_Vec delta(thePlane.Location(), hit);
    theDepth = depth;
    thePoint = gp_Pnt2d(delta.Dot(gp_Vec(thePlane.XDirection())),
                        delta.Dot(gp_Vec(thePlane.YDirection())));
    return true;
}

ProfileHit ProfileUnderRay(const gp_Lin&                         theRay,
                           const std::vector<ProfilePickTarget>& theTargets,
                           double                                theBodyDepth,
                           double                                theTolerance)
{
    ProfileHit best;
    double     bestArea = 0.0;

    for (std::size_t i = 0; i < theTargets.size(); ++i) {
        const ProfilePickTarget& target = theTargets[i];
        if (target.regions == nullptr || target.regions->empty()) {
            continue;
        }

        double   depth = 0.0;
        gp_Pnt2d point;
        if (!RayMeetsPlane(theRay, target.position, depth, point)) {
            continue;
        }

        // A body nearer than the plane by more than the tolerance hides
        // the sketch here. Asked before classifying the point, which is
        // the expensive half.
        if (theBodyDepth < depth - theTolerance) {
            continue;
        }

        // A farther plane never beats a nearer one, and a coplanar one
        // only with a smaller region.
        if (best.IsHit() && depth > best.depth + theTolerance) {
            continue;
        }

        const int region = ProfileRegionAt(*target.regions, point);
        if (region < 0) {
            continue;
        }
        const double area = (*target.regions)[static_cast<std::size_t>(region)].area;

        if (best.IsHit()) {
            const bool coplanar = std::fabs(depth - best.depth) <= theTolerance;
            const bool nearer = depth < best.depth - theTolerance;
            if (!nearer && !(coplanar && area < bestArea)) {
                continue;
            }
        }

        best.target = static_cast<int>(i);
        best.region = region;
        best.depth  = depth;
        best.point  = point;
        bestArea    = area;
    }

    return best;
}

BodyRayCaster::BodyRayCaster() = default;
BodyRayCaster::~BodyRayCaster() = default;

void BodyRayCaster::SetBodies(const std::vector<TopoDS_Shape>& theShapes)
{
    bool same = theShapes.size() == myShapes.size();
    for (std::size_t i = 0; same && i < theShapes.size(); ++i) {
        same = theShapes[i].IsSame(myShapes[i]);
    }
    if (same && (myIntersector || theShapes.empty())) {
        return;
    }

    myShapes = theShapes;
    myIntersector.reset();
    if (myShapes.empty()) {
        return;
    }

    try {
        TopoDS_Compound compound;
        BRep_Builder    builder;
        builder.MakeCompound(compound);
        for (const TopoDS_Shape& shape : myShapes) {
            if (!shape.IsNull()) {
                builder.Add(compound, shape);
            }
        }
        auto intersector = std::make_unique<IntCurvesFace_ShapeIntersector>();
        intersector->Load(compound, kIntersectTolerance);
        myIntersector = std::move(intersector);
    } catch (const Standard_Failure&) {
        // Bodies that will not load occlude nothing. A region lit through
        // a solid is a far smaller harm than a picker that throws out of a
        // mouse handler.
        myIntersector.reset();
    }
}

double BodyRayCaster::NearestDepth(const gp_Lin& theRay) const
{
    const double none = std::numeric_limits<double>::infinity();
    if (!myIntersector) {
        return none;
    }
    try {
        myIntersector->PerformNearest(theRay, 0.0, Precision::Infinite());
        if (!myIntersector->IsDone() || myIntersector->NbPnt() < 1) {
            return none;
        }
        return myIntersector->WParameter(1);
    } catch (const Standard_Failure&) {
        return none;
    }
}

std::string DescribeProfiles(const std::vector<double>& theAreas)
{
    if (theAreas.empty()) {
        return std::string();
    }
    double total = 0.0;
    for (const double area : theAreas) {
        total += area;
    }
    if (theAreas.size() == 1) {
        return "Profile   Area " + FormatArea(total);
    }
    return std::to_string(theAreas.size()) + " profiles   Total area " + FormatArea(total);
}

} // namespace lcad
