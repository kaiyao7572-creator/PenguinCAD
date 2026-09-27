#include "ui/MarkingMenuController.h"

#include <QApplication>
#include <QTimer>
#include <QWidget>

namespace lcad {

MarkingMenuController::MarkingMenuController(QWidget* theParent, ItemSource theItems)
    : myParent(theParent)
    , myItemSource(std::move(theItems))
{
}

MarkingMenuController::~MarkingMenuController() = default;

const MarkingMenu::ItemList& MarkingMenuController::ItemsForThisPress()
{
    if (!myItems) {
        myItems = myItemSource ? myItemSource() : MarkingMenu::ItemList{};
    }
    return *myItems;
}

const MarkingMenuTrail* MarkingMenuController::VisibleTrail() const
{
    return (!myTrail.isNull() && myTrail->isVisible()) ? myTrail.data() : nullptr;
}

void MarkingMenuController::HideTrail()
{
    if (!myTrail.isNull()) {
        myTrail->hide();
    }
}

void MarkingMenuController::OpenRing(const QPoint& theCentreGlobal, bool theButtonHeld)
{
    auto* ring = new MarkingMenu(ItemsForThisPress(), myParent);
    ring->PopUp(theCentreGlobal, theButtonHeld);
    myHeldRing = theButtonHeld ? ring : nullptr;
    myRingOpened = true;
}

void MarkingMenuController::Feed(MarkingMenuInput theInput, const QPoint& thePressGlobal,
                                   const QPoint& theCursorGlobal)
{
    switch (theInput) {
        case MarkingMenuInput::None:
            return;

        case MarkingMenuInput::Pressed:
            // Asked for lazily: a press that never becomes anything --
            // abandoned before it was classified -- needs no items at all.
            myItems.reset();
            myHeldRing = nullptr;
            myRingOpened = false;
            HideTrail();
            return;

        case MarkingMenuInput::Click:
            HideTrail();
            OpenRing(thePressGlobal, false);
            return;

        case MarkingMenuInput::GestureMove:
            if (myTrail.isNull()) {
                myTrail = new MarkingMenuTrail(myParent);
            }
            myTrail->Track(thePressGlobal, theCursorGlobal, ItemsForThisPress());
            return;

        case MarkingMenuInput::GestureRelease: {
            HideTrail();
            const QPoint travel = theCursorGlobal - thePressGlobal;
            const int wedge = MarkingGestureWedge(travel.x(), travel.y());
            if (wedge < 0) {
                return;   // dragged back to the start: never mind
            }
            const MarkingMenu::Item& item = ItemsForThisPress()[static_cast<std::size_t>(wedge)];
            if (item.label.isEmpty() || !item.enabled || !item.action) {
                return;   // greyed or empty: exactly what the ring would do
            }
            // After the mouse handler has unwound, as the ring runs its
            // commands, so one that opens a modal dialog does not open it
            // from inside a mouse event.
            QTimer::singleShot(0, qApp, item.action);
            return;
        }

        case MarkingMenuInput::HoldMove:
            HideTrail();
            // Once per press: a ring the user has since closed (Escape,
            // with the button still down) must not spring back.
            if (!myRingOpened) {
                OpenRing(thePressGlobal, true);
            }
            if (!myHeldRing.isNull()) {
                myHeldRing->PointAt(theCursorGlobal);
            }
            return;

        case MarkingMenuInput::HoldRelease:
            HideTrail();
            // Held past the hold time and let go before anything noticed:
            // the ring still comes up, and the release still counts.
            if (!myRingOpened) {
                OpenRing(thePressGlobal, true);
            }
            if (!myHeldRing.isNull()) {
                myHeldRing->ReleaseAt(theCursorGlobal);
            }
            return;

        case MarkingMenuInput::Abandon:
            // A ring that is up stays up: it has the pointer now, and its
            // own release handling decides.
            HideTrail();
            return;
    }
}

} // namespace lcad
