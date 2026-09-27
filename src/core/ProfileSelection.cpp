#include "core/ProfileSelection.h"

#include <algorithm>

namespace lcad {


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
    if (myItems.empty()) {
        return;
    }
    myItems.clear();
    NotifyChanged();
}

void ProfileSelection::Toggle(const ProfileRef& theRef, bool theAdditive)
{
    if (theRef.IsNull()) {
        // A click on empty space clears, unless the user was adding.
        if (!theAdditive && !myItems.empty()) {
            myItems.clear();
            NotifyChanged();
        }
        return;
    }

    if (!theAdditive) {
        myItems.clear();
        myItems.push_back(theRef);
        NotifyChanged();
        return;
    }

    const auto found = std::find_if(myItems.begin(), myItems.end(),
                                    [&theRef](const ProfileRef& theItem) {
                                        return theItem.SameRegion(theRef);
                                    });
    if (found != myItems.end()) {
        myItems.erase(found);
    } else {
        myItems.push_back(theRef);
    }
    NotifyChanged();
}

void ProfileSelection::Clear()
{
    if (myItems.empty()) {
        return;
    }
    myItems.clear();
    NotifyChanged();
}

void ProfileSelection::NotifyChanged()
{
    if (myOnChanged) {
        myOnChanged();
    }
}

bool ProfileSelection::Contains(const ProfileRef& theRef) const
{
    return std::any_of(myItems.begin(), myItems.end(), [&theRef](const ProfileRef& theItem) {
        return theItem.SameRegion(theRef);
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

        // The region under the old seed has to be bounded by the same
        // curves, or it is a different region that merely swallowed the
        // point -- which is what happens when the curve enclosing a small
        // region is deleted and the surrounding one spreads over it. The
        // highlight must never claim a region the user did not pick, for
        // the same reason FindProfile refuses to resolve one.
        if (!current.SameBoundary(item)) {
            continue;
        }

        if (std::none_of(kept.begin(), kept.end(), [&current](const ProfileRef& theKept) {
                return theKept.SameRegion(current);
            })) {
            kept.push_back(current);
        }
    }

    // Re-pointing a pick at the same region as it is now changes nothing
    // anyone can see; only a pick that went away is worth a redraw.
    const bool lost = kept.size() != myItems.size();
    myItems.swap(kept);
    if (lost) {
        NotifyChanged();
    }
}

} // namespace lcad
