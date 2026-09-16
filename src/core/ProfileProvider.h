#pragma once

#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>

#include <vector>

namespace lcad {

class Feature;

// The seam between the sketch subsystem and the solid-feature subsystem.
//
// A sketch implements this; Extrude/Revolve/Sweep consume it. Neither
// side needs to know the other's concrete types -- solid features hold a
// sketch's NAME, resolve it through ComputeContext::FindFeature, and ask
// for a profile through this interface.
class ProfileProvider
{
public:
    virtual ~ProfileProvider() = default;

    // Plane the profile lives on. Extrude uses its normal as the default
    // direction; revolve uses it to place the axis.
    virtual gp_Pln ProfilePlane() const = 0;

    // Closed wires making up the profile. Empty when the sketch has no
    // closed region (in which case a solid feature should report a clear
    // error rather than producing garbage).
    virtual std::vector<TopoDS_Wire> ProfileWires() const = 0;

    // Faces built from those wires, inner wires punched out as holes.
    // Default implementation builds them from ProfileWires(); override
    // only if the sketch can do it better.
    virtual std::vector<TopoDS_Face> ProfileFaces() const;
};

// Safe downcast: returns nullptr if the feature isn't a profile source.
ProfileProvider* AsProfileProvider(Feature* theFeature);

} // namespace lcad
