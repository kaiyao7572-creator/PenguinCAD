#include "core/Body.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lcad {

namespace {

// Two bodies are "the same body across a rebuild" when they are the same
// kind, their size agrees to within this fraction, and their centres are
// within kSameCentre. Loose enough to survive an edit that nudges a
// dimension, tight enough that two different bodies never swap names.
constexpr double kSameSizeRatio = 0.02;
constexpr double kSameCentre    = 1.0;  // mm

Body::Kind KindOf(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return Body::Kind::Wire;
    }
    if (TopExp_Explorer(theShape, TopAbs_SOLID).More()) {
        return Body::Kind::Solid;
    }
    if (TopExp_Explorer(theShape, TopAbs_FACE).More()) {
        return Body::Kind::Surface;
    }
    return Body::Kind::Wire;
}

// Sub-shapes with duplicates removed.
//
// TopExp_Explorer visits a shape once per USE, not once per shape: a box
// explored for vertices yields 48 (six faces, four edges each, two ends
// each) and for edges 24. Fusion reports 8 and 12, because a vertex
// shared by three edges is one vertex. An indexed map is what collapses
// them, and it keeps a stable first-seen order while doing it.
template <typename Collector>
std::size_t CountOf(const TopoDS_Shape& theShape, TopAbs_ShapeEnum theType, Collector theCollect)
{
    if (theShape.IsNull()) {
        return 0;
    }
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(theShape, theType, map);
    for (Standard_Integer i = 1; i <= map.Extent(); ++i) {
        theCollect(map(i));
    }
    return static_cast<std::size_t>(map.Extent());
}

// Size in whatever unit the body has: volume for a solid, area for a
// surface, length for a wire. Comparing a solid's volume with a surface's
// area would be meaningless, which is why kind is matched first.
double MeasureOf(const TopoDS_Shape& theShape, Body::Kind theKind)
{
    if (theShape.IsNull()) {
        return 0.0;
    }
    try {
        GProp_GProps props;
        switch (theKind) {
            case Body::Kind::Solid:   BRepGProp::VolumeProperties(theShape, props);  break;
            case Body::Kind::Surface: BRepGProp::SurfaceProperties(theShape, props); break;
            case Body::Kind::Wire:    BRepGProp::LinearProperties(theShape, props);  break;
        }
        return props.Mass();
    } catch (const Standard_Failure&) {
        return 0.0;
    }
}

gp_Pnt CentroidOf(const TopoDS_Shape& theShape, Body::Kind theKind)
{
    if (theShape.IsNull()) {
        return gp_Pnt(0.0, 0.0, 0.0);
    }
    try {
        GProp_GProps props;
        switch (theKind) {
            case Body::Kind::Solid:   BRepGProp::VolumeProperties(theShape, props);  break;
            case Body::Kind::Surface: BRepGProp::SurfaceProperties(theShape, props); break;
            case Body::Kind::Wire:    BRepGProp::LinearProperties(theShape, props);  break;
        }
        return props.CentreOfMass();
    } catch (const Standard_Failure&) {
        return gp_Pnt(0.0, 0.0, 0.0);
    }
}

void FlattenInto(const TopoDS_Shape& theShape, std::vector<TopoDS_Shape>& theResult)
{
    if (theShape.IsNull()) {
        return;
    }
    if (theShape.ShapeType() != TopAbs_COMPOUND) {
        theResult.push_back(theShape);
        return;
    }
    for (TopoDS_Iterator it(theShape); it.More(); it.Next()) {
        FlattenInto(it.Value(), theResult);
    }
}

} // namespace

// ---- Body ----

Body::Body(TopoDS_Shape theShape, std::string theName)
    : myShape(std::move(theShape))
    , myName(std::move(theName))
{
    myKind = KindOf(myShape);
}

std::vector<TopoDS_Face> Body::Faces() const
{
    std::vector<TopoDS_Face> faces;
    CountOf(myShape, TopAbs_FACE, [&faces](const TopoDS_Shape& theShape) {
        faces.push_back(TopoDS::Face(theShape));
    });
    return faces;
}

std::vector<TopoDS_Edge> Body::Edges() const
{
    std::vector<TopoDS_Edge> edges;
    CountOf(myShape, TopAbs_EDGE, [&edges](const TopoDS_Shape& theShape) {
        edges.push_back(TopoDS::Edge(theShape));
    });
    return edges;
}

std::vector<TopoDS_Vertex> Body::Vertices() const
{
    std::vector<TopoDS_Vertex> vertices;
    CountOf(myShape, TopAbs_VERTEX, [&vertices](const TopoDS_Shape& theShape) {
        vertices.push_back(TopoDS::Vertex(theShape));
    });
    return vertices;
}

std::vector<TopoDS_Solid> Body::Lumps() const
{
    std::vector<TopoDS_Solid> lumps;
    CountOf(myShape, TopAbs_SOLID, [&lumps](const TopoDS_Shape& theShape) {
        lumps.push_back(TopoDS::Solid(theShape));
    });
    return lumps;
}

std::vector<TopoDS_Shell> Body::Shells() const
{
    std::vector<TopoDS_Shell> shells;
    CountOf(myShape, TopAbs_SHELL, [&shells](const TopoDS_Shape& theShape) {
        shells.push_back(TopoDS::Shell(theShape));
    });
    return shells;
}

std::vector<TopoDS_Wire> Body::Wires() const
{
    std::vector<TopoDS_Wire> wires;
    CountOf(myShape, TopAbs_WIRE, [&wires](const TopoDS_Shape& theShape) {
        wires.push_back(TopoDS::Wire(theShape));
    });
    return wires;
}

std::size_t Body::FaceCount() const
{
    return CountOf(myShape, TopAbs_FACE, [](const TopoDS_Shape&) {});
}

std::size_t Body::EdgeCount() const
{
    return CountOf(myShape, TopAbs_EDGE, [](const TopoDS_Shape&) {});
}

std::size_t Body::VertexCount() const
{
    return CountOf(myShape, TopAbs_VERTEX, [](const TopoDS_Shape&) {});
}

double Body::Volume() const
{
    return myKind == Kind::Solid ? MeasureOf(myShape, Kind::Solid) : 0.0;
}

double Body::Area() const
{
    return MeasureOf(myShape, myKind == Kind::Wire ? Kind::Wire : Kind::Surface);
}

gp_Pnt Body::Centroid() const
{
    return CentroidOf(myShape, myKind);
}

// ---- free functions ----

std::vector<TopoDS_Shape> SplitIntoBodies(const TopoDS_Shape& theShape)
{
    std::vector<TopoDS_Shape> bodies;
    FlattenInto(theShape, bodies);
    return bodies;
}

// ---- BodyTable ----

void BodyTable::Update(const TopoDS_Shape& theShape)
{
    const std::vector<TopoDS_Shape> shapes = SplitIntoBodies(theShape);

    std::vector<BodyPtr> previous;
    previous.swap(myBodies);
    std::vector<bool> taken(previous.size(), false);

    myBodies.reserve(shapes.size());

    // Two passes so an exact survivor always wins its name back before an
    // approximate match can take it. Without that, editing one body's
    // dimension can hand its name to the untouched body beside it.
    std::vector<int> matched(shapes.size(), -1);
    for (int pass = 0; pass < 2; ++pass) {
        const bool exact = pass == 0;
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            if (matched[i] >= 0) {
                continue;
            }
            const Body::Kind kind = KindOf(shapes[i]);
            const double measure = MeasureOf(shapes[i], kind);
            const gp_Pnt centre = CentroidOf(shapes[i], kind);

            int    best = -1;
            double bestDistance = std::numeric_limits<double>::max();
            for (std::size_t j = 0; j < previous.size(); ++j) {
                if (taken[j] || !previous[j] || previous[j]->BodyKind() != kind) {
                    continue;
                }
                const double theirs = MeasureOf(previous[j]->Shape(), kind);
                const double scale = std::max({std::fabs(measure), std::fabs(theirs), 1.0e-9});
                const double sizeError = std::fabs(measure - theirs) / scale;
                const double distance = centre.Distance(previous[j]->Centroid());

                if (exact) {
                    if (sizeError > 1.0e-9 || distance > 1.0e-7) {
                        continue;
                    }
                } else if (sizeError > kSameSizeRatio || distance > kSameCentre) {
                    continue;
                }

                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = static_cast<int>(j);
                }
            }

            if (best >= 0) {
                matched[i] = best;
                taken[static_cast<std::size_t>(best)] = true;
            }
        }
    }

    // Third: a body that MOVED -- the same size somewhere else, which is
    // what a move or a rotate makes. Fusion keeps its name; matching on
    // position alone called it Body3. Only an unambiguous pairing counts:
    // the new body has exactly one leftover old body of its kind and size,
    // and that old body has no other taker. Two same-size bodies that both
    // moved cannot be told apart this way, and get new names rather than
    // possibly each other's.
    std::vector<std::vector<std::size_t>> sameSize(shapes.size());
    std::vector<int> takers(previous.size(), 0);
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        if (matched[i] >= 0) {
            continue;
        }
        const Body::Kind kind = KindOf(shapes[i]);
        const double measure = MeasureOf(shapes[i], kind);
        for (std::size_t j = 0; j < previous.size(); ++j) {
            if (taken[j] || !previous[j] || previous[j]->BodyKind() != kind) {
                continue;
            }
            const double theirs = MeasureOf(previous[j]->Shape(), kind);
            const double scale = std::max({std::fabs(measure), std::fabs(theirs), 1.0e-9});
            if (std::fabs(measure - theirs) / scale <= kSameSizeRatio) {
                sameSize[i].push_back(j);
                ++takers[j];
            }
        }
    }
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        if (matched[i] < 0 && sameSize[i].size() == 1 && takers[sameSize[i].front()] == 1) {
            matched[i] = static_cast<int>(sameSize[i].front());
            taken[sameSize[i].front()] = true;
        }
    }

    for (std::size_t i = 0; i < shapes.size(); ++i) {
        auto body = std::make_shared<Body>(shapes[i], std::string());
        if (matched[i] >= 0) {
            const BodyPtr& old = previous[static_cast<std::size_t>(matched[i])];
            body->SetName(old->Name());
            body->SetVisible(old->IsVisible());
        } else {
            body->SetName("Body" + std::to_string(myNextIndex++));
        }
        myBodies.push_back(body);
    }
}

Body* BodyTable::Find(const std::string& theName) const
{
    for (const BodyPtr& body : myBodies) {
        if (body && body->Name() == theName) {
            return body.get();
        }
    }
    return nullptr;
}

void BodyTable::Clear()
{
    myBodies.clear();
    myNextIndex = 1;
}

} // namespace lcad
