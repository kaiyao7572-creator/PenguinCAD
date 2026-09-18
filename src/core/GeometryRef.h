#pragma once

#include "core/Entity.h"

#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <string>
#include <vector>

namespace lcad {

class Body;

// A durable reference to one face, edge or vertex of a body.
//
// This is the topological naming problem, and it has no clean solution:
// the timeline is re-evaluated from scratch on every edit, so the face
// the user picked is a different C++ object -- and often a differently
// ordered one -- by the time a downstream feature asks for it. An index
// into Body::Faces() is worthless the moment an upstream feature adds a
// fillet.
//
// So a reference carries what the sub-shape IS rather than where it sat:
// the owning body's name, the kind of thing, its size, and where its
// centre is. Two independent signals, the same bargain ProfileRef makes,
// and the same refusal to guess: if nothing matches, or if two things
// match equally well, resolving FAILS rather than picking one. A feature
// that quietly moves to a different face is worse than a feature that
// goes red and says so.
struct GeometryRef
{
    EntityType  type = EntityType::BRepFace;
    std::string body;            // owning body's name
    gp_Pnt      point;           // centre of mass of the sub-shape
    double      measure = 0.0;   // face area, edge length, 0 for a vertex

    bool IsNull() const { return body.empty(); }

    // "BRepFace|Body1|12.5,0,7.25|300.5" -- rides a plain string
    // Parameter, so a feature can expose its picked face in the
    // properties panel with no new widget.
    std::string Encode() const;
    static bool Decode(const std::string& theText, GeometryRef& theResult);
};

std::string EncodeGeometryRefs(const std::vector<GeometryRef>& theRefs);
std::vector<GeometryRef> DecodeGeometryRefs(const std::string& theText);

// Describe one sub-shape of a body. theType must be BRepFace, BRepEdge or
// BRepVertex; anything else returns a null ref.
GeometryRef MakeGeometryRef(const Body& theBody, const TopoDS_Shape& theSubShape);

// Every face, edge or vertex of a body as a reference, in the body's own
// enumeration order.
std::vector<GeometryRef> CollectGeometryRefs(const Body& theBody, EntityType theType);

// Find what a reference names in the body as it is NOW. False when the
// sub-shape is gone, or when the match is ambiguous.
bool ResolveGeometryRef(const Body&        theBody,
                        const GeometryRef& theRef,
                        TopoDS_Shape&      theResult);

} // namespace lcad
