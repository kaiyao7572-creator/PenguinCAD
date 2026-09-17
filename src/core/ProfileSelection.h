#pragma once

#include "core/ProfileProvider.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lcad {

// Which closed regions of a sketch the user has picked.
//
// This lives in core rather than with the sketch code because it is the
// hand-off between two subsystems that deliberately know nothing about
// each other: the sketch fills it in when the user clicks a shaded
// region, and Extrude reads it to learn what to build. Routing that
// through core keeps the ProfileProvider seam intact -- src/features
// never has to include a sketch header.
//
// Deliberately separate from SketchSelection, which holds curves and
// points. A profile is not a curve: the constraint and modify commands
// want the curve the user clicked, while Extrude and Revolve want the
// area it encloses. One list carrying both would make every consumer
// filter, and would make Ctrl+click ambiguous about what it was adding.
class ProfileSelection
{
public:
    static ProfileSelection& Instance();

    // Which sketch these regions belong to. Setting a different one
    // empties the selection: a reference is only meaningful against the
    // sketch whose entity ids it names.
    const std::string& SketchName() const { return mySketchName; }
    void SetSketchName(const std::string& theName);

    // theAdditive keeps what was already picked (Ctrl+click), and takes a
    // region back out if it was already in -- the same bargain every other
    // selection in the app strikes.
    void Toggle(const ProfileRef& theRef, bool theAdditive);
    void Clear();

    bool Contains(const ProfileRef& theRef) const;

    const std::vector<ProfileRef>& Items() const { return myItems; }
    std::size_t Count() const { return myItems.size(); }
    bool IsEmpty() const { return myItems.empty(); }

    // Re-point every pick at the regions as they are NOW, dropping the
    // ones that no longer exist. A trim or an undo can reshape a region
    // under the selection; re-resolving keeps the stored reference exact
    // rather than letting it drift a little further from the truth with
    // every edit.
    //
    // Takes the regions rather than the sketch because the caller has
    // already built them to draw with -- computing the planar arrangement
    // again here, once per selected region, was measurably wasteful for an
    // answer that was already sitting in the caller's hand.
    void Prune(const std::vector<ProfileRegion>& theRegions);

private:
    ProfileSelection() = default;

    std::string             mySketchName;
    std::vector<ProfileRef> myItems;
};

} // namespace lcad
