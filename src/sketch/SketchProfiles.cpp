#include "sketch/SketchProfiles.h"

#include "sketch/SketchFeature.h"

#include <BOPAlgo_Builder.hxx>
#include <BOPAlgo_BuilderFace.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_State.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace lcad {

namespace {

// Smaller than any region a user could see, let alone click. Its real job
// is to throw away the slivers a near-tangent intersection leaves behind.
constexpr double kMinRegionArea = 1.0e-6;

// The unbounded cell BOPAlgo_BuilderFace returns alongside the real
// regions comes back with a NEGATIVE signed area, so the sign is the
// discriminator. This upper bound catches the separate case of a sketch
// with no closed region at all, where the algorithm hands back a
// degenerate face whose area is astronomically large rather than zero.
constexpr double kMaxRegionArea = 1.0e12;

constexpr double kClassifyTolerance = 1.0e-7;

// Seed-point search. The centroid is tried first and is almost always
// right; the grid is the fallback for the concave regions where the
// centroid falls outside the region entirely -- a ring, a crescent, an
// L-shape. Coarse on purpose: the seed only has to be comfortably inside,
// and every extra sample costs a point-in-face classification.
constexpr int kSeedGrid   = 12;
constexpr int kSeedRays   = 8;
constexpr int kSeedRefine = 10;

bool ContributesToProfile(const SketchEntity& theEntity)
{
    return theEntity.IsCurve() && !theEntity.isConstruction && !theEntity.IsDegenerate();
}

bool FaceContains(const BRepTopAdaptor_FClass2d& theClassifier, const gp_Pnt2d& thePoint)
{
    return const_cast<BRepTopAdaptor_FClass2d&>(theClassifier).Perform(thePoint) == TopAbs_IN;
}

// How far thePoint can travel in any direction before leaving the face.
// Used to pick the deepest of several interior candidates, so the seed
// lands where a small edit is least likely to move the boundary past it.
double Clearance(BRepTopAdaptor_FClass2d& theClassifier,
                 const gp_Pnt2d&          thePoint,
                 double                   theSpan)
{
    double clearance = theSpan;
    for (int ray = 0; ray < kSeedRays; ++ray) {
        const double angle = 2.0 * M_PI * ray / kSeedRays;
        const double dx = std::cos(angle);
        const double dy = std::sin(angle);

        double inside = 0.0;
        double outside = theSpan;
        for (int step = 0; step < kSeedRefine; ++step) {
            const double mid = 0.5 * (inside + outside);
            const gp_Pnt2d probe(thePoint.X() + mid * dx, thePoint.Y() + mid * dy);
            if (theClassifier.Perform(probe) == TopAbs_IN) {
                inside = mid;
            } else {
                outside = mid;
            }
        }
        clearance = std::min(clearance, inside);
    }
    return clearance;
}

// A point strictly inside theFace, in sketch coordinates.
//
// A region's face carries the sketch plane's own parameterisation, so the
// face's (u, v) already ARE sketch coordinates and no conversion is
// needed anywhere in here.
bool InteriorPoint(const TopoDS_Face& theFace, const gp_Pnt2d& theCentroid, gp_Pnt2d& theResult)
{
    try {
        BRepTopAdaptor_FClass2d classifier(theFace, kClassifyTolerance);
        if (FaceContains(classifier, theCentroid)) {
            theResult = theCentroid;
            return true;
        }

        double u0 = 0.0, u1 = 0.0, v0 = 0.0, v1 = 0.0;
        BRepTools::UVBounds(theFace, u0, u1, v0, v1);
        const double span = std::max(u1 - u0, v1 - v0);
        if (!(span > 0.0)) {
            return false;
        }

        double best = -1.0;
        for (int i = 1; i < kSeedGrid; ++i) {
            for (int j = 1; j < kSeedGrid; ++j) {
                const gp_Pnt2d sample(u0 + (u1 - u0) * i / kSeedGrid,
                                      v0 + (v1 - v0) * j / kSeedGrid);
                if (classifier.Perform(sample) != TopAbs_IN) {
                    continue;
                }
                const double clearance = Clearance(classifier, sample, span);
                if (clearance > best) {
                    best = clearance;
                    theResult = sample;
                }
            }
        }
        return best > 0.0;
    } catch (const Standard_Failure&) {
        return false;
    }
}

} // namespace

std::vector<ProfileRegion> ComputeProfileRegions(const SketchFeature& theSketch)
{
    std::vector<ProfileRegion> regions;

    try {
        // ---- one edge per profile curve, remembering where each came from ----
        std::vector<TopoDS_Edge> edges;
        std::vector<int>         owners;
        for (const SketchEntity& entity : theSketch.Entities()) {
            if (!ContributesToProfile(entity)) {
                continue;
            }
            const TopoDS_Edge edge = theSketch.BuildEdge(entity);
            if (edge.IsNull()) {
                continue;
            }
            edges.push_back(edge);
            owners.push_back(entity.id);
        }
        if (edges.empty()) {
            return regions;
        }

        // ---- split every curve where it crosses another ----
        //
        // Without this two overlapping circles share no vertex and the
        // lens between them is not a region at all. The general fuse also
        // hands back the history that maps each fragment to the curve it
        // was cut from, which is how a region learns which sketch
        // entities bound it.
        std::map<const TopoDS_TShape*, int> ownerOf;
        TopTools_ListOfShape                fragments;

        if (edges.size() < 2) {
            // The fuse refuses fewer than two arguments, and a lone circle
            // needs no splitting anyway.
            fragments.Append(edges.front());
            ownerOf[edges.front().TShape().get()] = owners.front();
        } else {
            BOPAlgo_Builder fuse;
            for (const TopoDS_Edge& edge : edges) {
                fuse.AddArgument(edge);
            }
            fuse.SetRunParallel(Standard_False);  // deterministic region order
            fuse.Perform();
            if (fuse.HasErrors()) {
                return regions;
            }

            for (std::size_t i = 0; i < edges.size(); ++i) {
                const TopTools_ListOfShape& pieces = fuse.Modified(edges[i]);
                if (pieces.IsEmpty()) {
                    ownerOf[edges[i].TShape().get()] = owners[i];  // survived uncut
                    continue;
                }
                for (TopTools_ListIteratorOfListOfShape it(pieces); it.More(); it.Next()) {
                    ownerOf[it.Value().TShape().get()] = owners[i];
                }
            }

            for (TopExp_Explorer it(fuse.Shape(), TopAbs_EDGE); it.More(); it.Next()) {
                fragments.Append(it.Current());
            }
        }

        // ---- walk the arrangement into minimal regions ----
        //
        // Each fragment is offered in BOTH orientations. That is not a
        // detail: BOPAlgo_BuilderFace walks an edge only in the direction
        // it was given, so with one orientation apiece it finds the loops
        // on one side of each curve and silently drops the cells on the
        // other -- a rectangle cut by a line comes back as one region
        // instead of two. This is how OCCT's own boolean code feeds it.
        TopTools_ListOfShape oriented;
        for (TopTools_ListIteratorOfListOfShape it(fragments); it.More(); it.Next()) {
            oriented.Append(it.Value().Oriented(TopAbs_FORWARD));
            oriented.Append(it.Value().Oriented(TopAbs_REVERSED));
        }

        BOPAlgo_BuilderFace builder;
        builder.SetFace(BRepBuilderAPI_MakeFace(theSketch.Plane()).Face());
        builder.SetShapes(oriented);
        builder.SetAvoidInternalShapes(Standard_True);
        builder.Perform();
        if (builder.HasErrors()) {
            return regions;
        }

        for (TopTools_ListIteratorOfListOfShape it(builder.Areas()); it.More(); it.Next()) {
            const TopoDS_Face face = TopoDS::Face(it.Value());
            if (face.IsNull()) {
                continue;
            }

            GProp_GProps props;
            BRepGProp::SurfaceProperties(face, props);
            const double area = props.Mass();
            if (!std::isfinite(area) || area < kMinRegionArea || area > kMaxRegionArea) {
                continue;  // the unbounded cell, a sliver, or an open chain
            }

            ProfileRegion region;
            region.face = face;
            region.area = area;
            if (!InteriorPoint(face, theSketch.To2d(props.CentreOfMass()), region.ref.seed)) {
                continue;  // no point inside means nothing could ever click it
            }

            std::set<int> bounding;
            for (TopExp_Explorer edge(face, TopAbs_EDGE); edge.More(); edge.Next()) {
                const auto found = ownerOf.find(edge.Current().TShape().get());
                if (found != ownerOf.end()) {
                    bounding.insert(found->second);
                }
            }
            region.ref.boundary.assign(bounding.begin(), bounding.end());
            if (region.ref.boundary.empty()) {
                continue;  // unattributable: refuse rather than offer an unnameable region
            }

            regions.push_back(region);
        }
    } catch (const Standard_Failure&) {
        regions.clear();
    }

    // Largest first, so the whole-sketch face list starts with the outer
    // region the way callers of ProfileWires() have always expected. The
    // seed breaks ties so the order never depends on kernel iteration.
    std::stable_sort(regions.begin(), regions.end(),
                     [](const ProfileRegion& theLeft, const ProfileRegion& theRight) {
                         if (theLeft.area != theRight.area) {
                             return theLeft.area > theRight.area;
                         }
                         if (theLeft.ref.seed.X() != theRight.ref.seed.X()) {
                             return theLeft.ref.seed.X() < theRight.ref.seed.X();
                         }
                         return theLeft.ref.seed.Y() < theRight.ref.seed.Y();
                     });
    return regions;
}

} // namespace lcad
