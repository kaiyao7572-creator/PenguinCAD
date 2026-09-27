#pragma once

#include <QString>
#include <QWidget>

#include <array>
#include <functional>

namespace lcad {

// Fusion's marking menu: right-click the canvas and eight commands appear
// in a ring around the cursor, each always in the same place, so after a
// week the hand knows where Undo is without looking.
//
// A popup, so a click anywhere outside it -- or Escape, or a click on the
// dead centre -- closes it without doing anything. Point into a wedge to
// light it, click to run it. The command runs AFTER the popup has closed,
// so one that opens a modal dialog does not open it underneath a popup
// that is still grabbing the mouse.
//
// Opened by a right press HELD still (ui/MarkingMenuGesture.h), the ring
// comes up with the button still down, and letting go over a wedge runs it
// -- a second click would be one more than Fusion asks for. Letting go in
// the dead centre leaves it open for an ordinary click.
class MarkingMenu : public QWidget
{
    Q_OBJECT

public:
    struct Item
    {
        QString label;          // empty: nothing in this wedge
        bool    enabled = true;
        QString tooltip;
        std::function<void()> action;
    };
    using ItemList = std::array<Item, 8>;

    // Eight items, clockwise from straight up (ui/MarkingMenuGeometry.h).
    explicit MarkingMenu(const ItemList& theItems, QWidget* theParent = nullptr);

    // Show centred on theGlobalPos, where the right button went down.
    // theButtonHeld: it is still down, and its release chooses.
    void PopUp(const QPoint& theGlobalPos, bool theButtonHeld = false);

    // The pointer, in global coordinates, when it reaches the ring some way
    // other than as this widget's own mouse events: a release the canvas
    // saw rather than the popup.
    void PointAt(const QPoint& theGlobalPos);

    // The held button came up at theGlobalPos. Runs the wedge under it and
    // closes; in the dead zone or over a greyed wedge the ring stays open.
    // Only the FIRST release after a held PopUp counts, whichever way it
    // arrives, so one release seen twice cannot run a command twice.
    void ReleaseAt(const QPoint& theGlobalPos);

    // The ring's centre in this widget's own coordinates.
    QPoint Centre() const { return m_centre; }

    int HighlightedWedge() const { return m_highlight; }
    const ItemList& Items() const { return m_items; }

protected:
    void paintEvent(QPaintEvent* theEvent) override;
    void mouseMoveEvent(QMouseEvent* theEvent) override;
    void mousePressEvent(QMouseEvent* theEvent) override;
    void mouseReleaseEvent(QMouseEvent* theEvent) override;
    void keyPressEvent(QKeyEvent* theEvent) override;

private:
    // The wedge under thePos, or -1 -- also -1 for an empty or disabled
    // wedge, which is never lit and never runs.
    int ActiveWedgeAt(const QPointF& thePos) const;
    void HighlightAt(const QPointF& thePos);
    void Activate(int theWedge);

    ItemList m_items;
    QPoint m_centre;
    int    m_highlight = -1;
    bool   m_awaitingRelease = false;
};

// What a right-button GESTURE shows instead of the ring: a stroke from
// where the button went down to the pointer, the hub with the wedge it
// points into lit, and that one wedge's label where the ring would put it
// -- so a flick teaches the same places the ring does. A greyed wedge is
// shown grey (it will not run); an empty one shows nothing.
//
// A transparent tool window over the canvas that never takes the pointer,
// so the drag keeps going to the viewport underneath.
class MarkingMenuTrail : public QWidget
{
public:
    explicit MarkingMenuTrail(QWidget* theParent = nullptr);

    // Show the gesture from thePressGlobal to theCursorGlobal over theItems.
    void Track(const QPoint& thePressGlobal, const QPoint& theCursorGlobal,
               const MarkingMenu::ItemList& theItems);

    // The wedge the stroke points into (-1 inside the threshold), and what
    // is there; for the script harness to read back.
    int LitWedge() const { return m_wedge; }
    const MarkingMenu::Item* LitItem() const;

protected:
    void paintEvent(QPaintEvent* theEvent) override;

private:
    MarkingMenu::ItemList m_items;
    QPoint m_centre;   // the press point, in this widget's coordinates
    QPoint m_cursor;
    int    m_wedge = -1;
};

} // namespace lcad
