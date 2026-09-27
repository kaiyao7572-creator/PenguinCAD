#pragma once

#include "ui/MarkingMenu.h"
#include "ui/MarkingMenuGesture.h"

#include <QPoint>
#include <QPointer>

#include <functional>
#include <optional>

class QWidget;

namespace lcad {

// Turns what the right button did over the canvas into what Fusion shows
// for it: a click opens the ring, a flick runs the wedge it points into
// without the ring ever appearing, and a press held still brings the ring
// up under the button so that letting go over a wedge runs it.
//
// The eight items are asked for ONCE per press, and that one list feeds
// the ring and the gesture alike, so a flick and the ring can never
// disagree about what is where -- not even when a tool starts or stops
// between the press and the release and Undo turns into Cancel.
class MarkingMenuController
{
public:
    using ItemSource = std::function<MarkingMenu::ItemList()>;

    // theParent owns the ring and the trail (both are top-level windows).
    MarkingMenuController(QWidget* theParent, ItemSource theItems);
    ~MarkingMenuController();

    MarkingMenuController(const MarkingMenuController&) = delete;
    MarkingMenuController& operator=(const MarkingMenuController&) = delete;

    // Feed what the viewport's RightButtonTracker said, with where the
    // button went down and where the pointer is now, both GLOBAL.
    void Feed(MarkingMenuInput theInput, const QPoint& thePressGlobal,
                const QPoint& theCursorGlobal);

    // The gesture trail, when one is on screen; for the script harness.
    const MarkingMenuTrail* VisibleTrail() const;

private:
    const MarkingMenu::ItemList& ItemsForThisPress();
    void OpenRing(const QPoint& theCentreGlobal, bool theButtonHeld);
    void HideTrail();

    QWidget*   myParent = nullptr;
    ItemSource myItemSource;

    std::optional<MarkingMenu::ItemList> myItems;   // this press's, once asked for
    QPointer<MarkingMenu>      myHeldRing;          // the ring a hold brought up
    bool                       myRingOpened = false;   // for this press
    QPointer<MarkingMenuTrail> myTrail;
};

} // namespace lcad
