#pragma once

#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt2d.hxx>

#include <string>
#include <vector>

namespace lcad {

class Feature;

// A durable reference to ONE closed region of a sketch.
//
// Regions are recomputed from the sketch curves on every rebuild, so an
// index into that list would name a different region the moment the user
// draws one more line. Identity is carried by two independent signals
// instead: the ids of the sketch entities that bound the region, and a
// point known to lie inside it. The id set survives anything that doesn't
// change which curves enclose the region; the seed point survives a trim
// that renumbers them. Requiring the two to agree is what stops an edit
// from silently re-pointing an extrude at a different region -- quietly
// extruding the wrong area is worse than failing loudly.
struct ProfileRef
{
    // Bounding sketch entity ids, ascending, so two refs compare directly.
    std::vector<int> boundary;

    // A point strictly inside the region, in sketch coordinates. It breaks
    // ties between regions that share a boundary -- all three regions of
    // two overlapping circles are bounded by the same two curves -- and it
    // is what a click is tested against. It is deliberately NOT allowed to
    // rescue a reference whose bounding curves are gone: see FindProfile.
    gp_Pnt2d seed;

    bool IsNull() const { return boundary.empty(); }

    // "3,4,5,6@12.5,7.25". Compact on purpose: it rides the plain string
    // Parameter the properties panel already knows how to show and edit,
    // so a profile needs no new widget to be inspectable.
    std::string Encode() const;
    static bool Decode(const std::string& theText, ProfileRef& theResult);

    // Same bounding curves -- a candidate for being the same region.
    bool SameBoundary(const ProfileRef& theOther) const;

    // The same region, boundary and seed both. Loose on the seed because a
    // rebuild nudges geometry by float noise, but far tighter than the gap
    // between any two regions a user could tell apart.
    //
    // One definition on purpose: the selection uses it to decide what
    // Ctrl+click is toggling and a feature uses it to drop a region listed
    // twice, and the two must not drift -- they are answering the same
    // question about the same reference.
    bool SameRegion(const ProfileRef& theOther) const;
};

// Several refs in one string, for a feature built on more than one region.
std::string EncodeProfileRefs(const std::vector<ProfileRef>& theRefs);
std::vector<ProfileRef> DecodeProfileRefs(const std::string& theText);

// One minimal closed region of a sketch -- what Fusion calls a profile.
struct ProfileRegion
{
    TopoDS_Face face;
    ProfileRef  ref;
    double      area = 0.0;
};

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

    // Every minimal closed region, the way Fusion splits a sketch into
    // pickable profiles: a rectangle with a circle inside it is TWO
    // regions, not one face with a hole. Default is empty so a provider
    // that has no notion of regions still works through ProfileFaces().
    virtual std::vector<ProfileRegion> ProfileRegions() const { return {}; }

    // Face for a stored reference, resolved against the regions as they
    // are NOW. False when the region it names no longer exists.
    bool FindProfile(const ProfileRef& theRef, TopoDS_Face& theFace) const;
};

// Index of the region under a point in sketch-plane coordinates, or -1.
//
// The SMALLEST containing region wins: a region nested inside another
// would otherwise be unreachable, and the smaller one is always the more
// specific thing to mean by a click.
int ProfileRegionAt(const std::vector<ProfileRegion>& theRegions, const gp_Pnt2d& thePoint);

// Safe downcast: returns nullptr if the feature isn't a profile source.
ProfileProvider* AsProfileProvider(Feature* theFeature);

} // namespace lcad
