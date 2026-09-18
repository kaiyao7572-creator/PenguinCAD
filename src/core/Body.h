#pragma once

#include "core/Entity.h"

#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pnt.hxx>

#include <memory>
#include <string>
#include <vector>

namespace lcad {

// Fusion's BRepBody: one nameable, hideable, selectable piece of geometry.
//
// The document evaluates to a single TopoDS_Shape, which is the right
// model for a timeline but the wrong one for a user -- "the model" is not
// a thing you can hide, rename, or act on one of. Fusion is body-centric
// throughout, and everything downstream of that (per-body appearance,
// Combine, assemblies, a Bodies folder that means anything) needs bodies
// to exist as objects first.
class Body
{
public:
    // Fusion exposes this as BRepBody.isSolid; an open shell is a surface
    // body, and a loose wire is neither but still shows in the browser.
    enum class Kind
    {
        Solid,
        Surface,
        Wire
    };

    Body() = default;
    Body(TopoDS_Shape theShape, std::string theName);

    EntityType Type() const { return EntityType::BRepBody; }

    const std::string& Name() const { return myName; }
    void SetName(std::string theName) { myName = std::move(theName); }

    const TopoDS_Shape& Shape() const { return myShape; }

    Kind BodyKind() const { return myKind; }
    bool IsSolid() const { return myKind == Kind::Solid; }

    // Fusion's light-bulb. Hiding a body takes it out of the viewport
    // without touching the timeline that made it.
    bool IsVisible() const { return myIsVisible; }
    void SetVisible(bool theValue) { myIsVisible = theValue; }

    // ---- sub-entities, in Fusion's vocabulary ----
    //
    // Enumerated on demand rather than cached: the shape is replaced
    // wholesale on every rebuild, so a cache would be stale more often
    // than it was warm.
    std::vector<TopoDS_Face>   Faces() const;
    std::vector<TopoDS_Edge>   Edges() const;
    std::vector<TopoDS_Vertex> Vertices() const;

    // A lump is one connected chunk of a body, a shell one closed or open
    // boundary of a lump. A hollow cube is one lump with two shells.
    std::vector<TopoDS_Solid> Lumps() const;
    std::vector<TopoDS_Shell> Shells() const;
    std::vector<TopoDS_Wire>  Wires() const;

    std::size_t FaceCount() const;
    std::size_t EdgeCount() const;
    std::size_t VertexCount() const;

    // ---- measurements ----

    // Zero for anything that isn't a solid.
    double Volume() const;

    // Total surface area; for a wire body, total length.
    double Area() const;

    // Centre of mass of whatever the body is -- volume for a solid, area
    // for a surface, length for a wire. Used to tell bodies apart across a
    // rebuild, so it has to mean something for all three.
    gp_Pnt Centroid() const;

private:
    TopoDS_Shape myShape;
    std::string  myName;
    Kind         myKind      = Kind::Solid;
    bool         myIsVisible = true;
};

using BodyPtr = std::shared_ptr<Body>;

// Split a document shape into the bodies a user would say it contains.
//
// The direct children of a compound are separate bodies, which is exactly
// what the timeline produces: a New Body operation compounds its result
// with what was already there, while Join fuses into one shape and so
// into one body. Nested compounds are flattened; anything that is not a
// compound is a single body.
std::vector<TopoDS_Shape> SplitIntoBodies(const TopoDS_Shape& theShape);

// The bodies of a document, with names that survive a rebuild.
//
// The whole timeline is re-evaluated on every edit, so the shapes are new
// objects every time and nothing in them remembers being called "Body2".
// Names are carried across by matching each new body to a previous one of
// the same kind with a close enough size and position -- the same
// two-signal idea profile references use, and for the same reason: one
// signal alone mismatches, and mismatching silently renames the user's
// bodies underneath them.
class BodyTable
{
public:
    // Re-derive from theShape, carrying names and visibility over.
    void Update(const TopoDS_Shape& theShape);

    const std::vector<BodyPtr>& Bodies() const { return myBodies; }
    std::size_t Count() const { return myBodies.size(); }

    Body* Find(const std::string& theName) const;

    void Clear();

private:
    std::vector<BodyPtr> myBodies;

    // Never reused, even after a body is deleted -- Fusion keeps counting
    // too, and a recycled name would attach old references to new geometry.
    int myNextIndex = 1;
};

} // namespace lcad
