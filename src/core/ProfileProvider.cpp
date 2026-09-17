#include "core/ProfileProvider.h"

#include "core/Feature.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRepTools.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_State.hxx>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace lcad {

namespace {

// Tight enough that a seed sitting on a boundary reads as outside rather
// than in: a seed is always placed well inside, so "nearly on the edge"
// means the region moved, not that the test is too strict.
constexpr double kClassifyTolerance = 1.0e-7;

// A region's face carries the sketch plane's own parameterisation, so the
// face's (u, v) ARE the sketch's 2D coordinates -- which is what lets a
// seed point stored in sketch space be classified against a face with no
// conversion at all.
bool FaceContains(const TopoDS_Face& theFace, const gp_Pnt2d& thePoint)
{
    if (theFace.IsNull()) {
        return false;
    }
    try {
        BRepTopAdaptor_FClass2d classifier(theFace, kClassifyTolerance);
        return classifier.Perform(thePoint) == TopAbs_IN;
    } catch (const Standard_Failure&) {
        return false;
    }
}

} // namespace

// ---- ProfileRef ----

std::string ProfileRef::Encode() const
{
    std::ostringstream out;
    for (std::size_t i = 0; i < boundary.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << boundary[i];
    }
    // Enough digits that the seed survives the round trip through the
    // properties panel unchanged; a re-parsed seed that drifted could
    // land outside a thin region.
    char point[64];
    std::snprintf(point, sizeof(point), "@%.10g,%.10g", seed.X(), seed.Y());
    out << point;
    return out.str();
}

bool ProfileRef::Decode(const std::string& theText, ProfileRef& theResult)
{
    const std::size_t at = theText.find('@');
    if (at == std::string::npos) {
        return false;
    }

    ProfileRef parsed;

    std::istringstream ids(theText.substr(0, at));
    std::string token;
    while (std::getline(ids, token, ',')) {
        if (token.empty()) {
            continue;
        }
        try {
            parsed.boundary.push_back(std::stoi(token));
        } catch (const std::exception&) {
            return false;
        }
    }
    if (parsed.boundary.empty()) {
        return false;
    }
    std::sort(parsed.boundary.begin(), parsed.boundary.end());

    const std::string rest = theText.substr(at + 1);
    const std::size_t comma = rest.find(',');
    if (comma == std::string::npos) {
        return false;
    }
    try {
        parsed.seed = gp_Pnt2d(std::stod(rest.substr(0, comma)),
                               std::stod(rest.substr(comma + 1)));
    } catch (const std::exception&) {
        return false;
    }

    theResult = parsed;
    return true;
}

bool ProfileRef::SameBoundary(const ProfileRef& theOther) const
{
    return boundary == theOther.boundary;
}

bool ProfileRef::SameRegion(const ProfileRef& theOther) const
{
    constexpr double kSeedTolerance = 1.0e-6;
    return SameBoundary(theOther)
        && seed.SquareDistance(theOther.seed) <= kSeedTolerance * kSeedTolerance;
}

std::string EncodeProfileRefs(const std::vector<ProfileRef>& theRefs)
{
    std::string text;
    for (const ProfileRef& ref : theRefs) {
        if (ref.IsNull()) {
            continue;
        }
        if (!text.empty()) {
            text += ';';
        }
        text += ref.Encode();
    }
    return text;
}

std::vector<ProfileRef> DecodeProfileRefs(const std::string& theText)
{
    std::vector<ProfileRef> refs;
    std::istringstream parts(theText);
    std::string part;
    while (std::getline(parts, part, ';')) {
        ProfileRef ref;
        if (ProfileRef::Decode(part, ref)) {
            refs.push_back(ref);
        }
    }
    return refs;
}

// ---- ProfileProvider ----

std::vector<TopoDS_Face> ProfileProvider::ProfileFaces() const
{
    std::vector<TopoDS_Face> faces;
    const std::vector<TopoDS_Wire> wires = ProfileWires();
    if (wires.empty()) {
        return faces;
    }

    try {
        const gp_Pln plane = ProfilePlane();

        // First wire is treated as the outer boundary; any others are
        // punched out as holes. That covers the common "rectangle with a
        // circle in it" case without needing full containment analysis.
        BRepBuilderAPI_MakeFace faceMaker(plane, wires.front());
        if (!faceMaker.IsDone()) {
            return faces;
        }

        for (std::size_t i = 1; i < wires.size(); ++i) {
            TopoDS_Wire hole = wires[i];
            hole.Reverse();
            faceMaker.Add(hole);
        }

        if (faceMaker.IsDone()) {
            faces.push_back(faceMaker.Face());
        }
    } catch (const Standard_Failure&) {
        faces.clear();
    }

    return faces;
}

bool ProfileProvider::FindProfile(const ProfileRef& theRef, TopoDS_Face& theFace) const
{
    if (theRef.IsNull()) {
        return false;
    }

    const std::vector<ProfileRegion> regions = ProfileRegions();
    if (regions.empty()) {
        return false;
    }

    // Step one: the curves bounding the region. This is the signal that
    // survives the region changing shape -- dragging a line moves the
    // region but not which curves enclose it.
    std::vector<const ProfileRegion*> candidates;
    for (const ProfileRegion& region : regions) {
        if (region.ref.SameBoundary(theRef)) {
            candidates.push_back(&region);
        }
    }

    if (candidates.size() == 1) {
        theFace = candidates.front()->face;
        return !theFace.IsNull();
    }

    // Step two: the seed point, which is here to break ties rather than to
    // rescue a lost region. Two overlapping circles give all three of
    // their regions the same pair of bounding ids, so the ids alone can
    // never name one of them and only a point inside can.
    // No region is bounded by those curves any more, so the region the
    // user picked is gone. Refuse.
    //
    // An earlier version fell back to "whichever region now contains the
    // seed, as long as it still shares a bounding curve", to survive a
    // trim renumbering an edge. That is too generous, and generous is the
    // wrong direction here: pick the RING of a rectangle-around-a-circle,
    // delete the circle, and the plain rectangle shares four of the
    // ring's five curves and swallows its seed -- so the extrude quietly
    // became the whole rectangle instead of the ring. Any rule loose
    // enough to survive a renumber is loose enough to do that, because
    // the two are the same shape of change: a bounding curve stopped
    // existing. Breaking loudly costs the user one re-pick; guessing
    // wrong costs them a wrong model they may not notice.
    if (candidates.empty()) {
        return false;
    }

    const ProfileRegion* hit = nullptr;
    for (const ProfileRegion* region : candidates) {
        if (!FaceContains(region->face, theRef.seed)) {
            continue;
        }
        if (hit != nullptr) {
            return false;  // ambiguous: refuse rather than guess
        }
        hit = region;
    }

    if (hit == nullptr) {
        return false;
    }
    theFace = hit->face;
    return !theFace.IsNull();
}

int ProfileRegionAt(const std::vector<ProfileRegion>& theRegions, const gp_Pnt2d& thePoint)
{
    int    hit  = -1;
    double best = 0.0;

    for (std::size_t i = 0; i < theRegions.size(); ++i) {
        const ProfileRegion& region = theRegions[i];
        if (region.face.IsNull()) {
            continue;
        }
        if (hit >= 0 && region.area >= best) {
            continue;  // already holding a smaller candidate
        }
        try {
            BRepTopAdaptor_FClass2d classifier(region.face, kClassifyTolerance);
            if (classifier.Perform(thePoint) != TopAbs_IN) {
                continue;
            }
        } catch (const Standard_Failure&) {
            continue;
        }
        hit  = static_cast<int>(i);
        best = region.area;
    }

    return hit;
}

ProfileProvider* AsProfileProvider(Feature* theFeature)
{
    return dynamic_cast<ProfileProvider*>(theFeature);
}

} // namespace lcad
