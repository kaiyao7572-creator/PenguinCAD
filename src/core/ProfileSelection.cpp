#include "core/ProfileSelection.h"


#include <algorithm>

namespace lcad {

namespace {

// Two references name the same region when they are bounded by the same
// curves and their seeds agree. The seed comparison is loose because a
// re-solve can nudge geometry by float noise; it is far tighter than the
// gap between any two regions a user could tell apart.
constexpr double kSameSeedTolerance = 1.0e-6;

bool SameRegion(const ProfileRef& theLeft, const ProfileRef& theRight)
{
    return theLeft.SameBoundary(theRight)
        && theLeft.seed.SquareDistance(theRight.seed)
               <= kSameSeedTolerance * kSameSeedTolerance;
}

} // namespace

ProfileSelection& ProfileSelection::Instance()
{
    static ProfileSelection theInstance;
    return theInstance;
}

void ProfileSelection::SetSketchName(const std::string& theName)
{
    if (mySketchName == theName) {
        return;
    }
    mySketchName = theName;
    myItems.clear();
}

void ProfileSelection::Toggle(const ProfileRef& theRef, bool theAdditive)
{
    if (theRef.IsNull()) {
        // A click on empty space clears, unless the user was adding.
        if (!theAdditive) {
            myItems.clear();
        }
        return;
    }

    if (!theAdditive) {
        myItems.clear();
        myItems.push_back(theRef);
        return;
    }

    const auto found = std::find_if(myItems.begin(), myItems.end(),
                                    [&theRef](const ProfileRef& theItem) {
                                        return SameRegion(theItem, theRef);
                                    });
    if (found != myItems.end()) {
        myItems.erase(found);
        return;
    }
    myItems.push_back(theRef);
}

void ProfileSelection::Clear()
{
    myItems.clear();
}

bool ProfileSelection::Contains(const ProfileRef& theRef) const
{
    return std::any_of(myItems.begin(), myItems.end(), [&theRef](const ProfileRef& theItem) {
        return SameRegion(theItem, theRef);
    });
}

void ProfileSelection::Prune(const std::vector<ProfileRegion>& theRegions)
{
    if (myItems.empty()) {
        return;
    }

    std::vector<ProfileRef> kept;
    for (const ProfileRef& item : myItems) {
        const int index = ProfileRegionAt(theRegions, item.seed);
        if (index < 0) {
            continue;  // nothing under the old seed any more
        }
        const ProfileRef& current = theRegions[static_cast<std::size_t>(index)].ref;

        // The region under the old seed has to still be bounded by at
        // least one of the curves that bounded the original, or it is a
        // different region that merely swallowed the point -- which is
        // exactly what happens when the circle enclosing a small region is
        // deleted and the surrounding region spreads over where it was.
        if (!ShareBoundaryCurve(current.boundary, item.boundary)) {
            continue;
        }

        if (std::none_of(kept.begin(), kept.end(), [&current](const ProfileRef& theKept) {
                return SameRegion(theKept, current);
            })) {
            kept.push_back(current);
        }
    }

    myItems.swap(kept);
}

} // namespace lcad
