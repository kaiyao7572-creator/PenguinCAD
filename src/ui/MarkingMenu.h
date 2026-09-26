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

    // Eight items, clockwise from straight up (ui/MarkingMenuGeometry.h).
    explicit MarkingMenu(const std::array<Item, 8>& theItems, QWidget* theParent = nullptr);

    // Show centred on theGlobalPos, where the right-click happened.
    void PopUp(const QPoint& theGlobalPos);

    // The ring's centre in this widget's own coordinates.
    QPoint Centre() const { return m_centre; }

    int HighlightedWedge() const { return m_highlight; }
    const std::array<Item, 8>& Items() const { return m_items; }

protected:
    void paintEvent(QPaintEvent* theEvent) override;
    void mouseMoveEvent(QMouseEvent* theEvent) override;
    void mousePressEvent(QMouseEvent* theEvent) override;
    void keyPressEvent(QKeyEvent* theEvent) override;

private:
    // The wedge under thePos, or -1 -- also -1 for an empty or disabled
    // wedge, which is never lit and never runs.
    int ActiveWedgeAt(const QPointF& thePos) const;
    void Activate(int theWedge);

    // Where wedge theIndex's label is drawn, in widget coordinates.
    QRectF LabelRect(int theIndex) const;

    std::array<Item, 8> m_items;
    QPoint m_centre;
    int    m_highlight = -1;
};

} // namespace lcad
