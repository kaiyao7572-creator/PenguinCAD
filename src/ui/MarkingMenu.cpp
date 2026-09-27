#include "ui/MarkingMenu.h"
#include "ui/MarkingMenuGeometry.h"
#include "ui/MarkingMenuGesture.h"

#include <QApplication>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace lcad {

namespace {

// Logical pixels. The ring is wide enough that eight labels never touch,
// and the dead zone big enough that a click that did not mean to move
// does not pick anything.
constexpr double kRingRadius = 105.0;
constexpr double kDeadZone = 22.0;
constexpr double kPillHeight = 28.0;
constexpr double kPillPadding = 12.0;
constexpr double kMargin = 8.0;

// How far round the press point the gesture trail can draw. The label sits
// on the ring radius and the widest one fits inside this; a stroke that
// goes further is cut off at the edge, which is fine -- its direction is
// all that matters, and that is already decided.
constexpr double kTrailHalfSize = kRingRadius + 190.0;

// Fusion's highlight blue.
const QColor kHighlight(6, 150, 215);

// Where wedge theIndex's label is drawn, around theCentre.
QRectF PillRect(const QFontMetrics& theMetrics, const QString& theLabel, int theIndex,
                const QPointF& theCentre)
{
    double dx = 0.0;
    double dy = 0.0;
    MarkingMenuWedgeAnchor(theIndex, kRingRadius, dx, dy);

    const double width = theMetrics.horizontalAdvance(theLabel) + 2.0 * kPillPadding;
    const QPointF anchor(theCentre.x() + dx, theCentre.y() + dy);

    // Labels on the right grow rightwards from the ring and labels on the
    // left grow leftwards, as Fusion lays them out, so a long name never
    // reaches back across the centre.
    double left = anchor.x() - width / 2.0;
    if (dx > 1.0) {
        left = anchor.x() - kPillHeight / 2.0;
    } else if (dx < -1.0) {
        left = anchor.x() - width + kPillHeight / 2.0;
    }
    return QRectF(left, anchor.y() - kPillHeight / 2.0, width, kPillHeight);
}

// The centre: a ring, with the lit wedge's slice filled in so the eye can
// see which way it is pointing before reading any label.
void PaintHub(QPainter& thePainter, const QPointF& theCentre, double theRadius, int theLit,
              const QColor& theLitColour, const QPalette& theColours)
{
    QColor pill = theColours.color(QPalette::Window);
    pill.setAlpha(235);
    const QRectF hub(theCentre.x() - theRadius, theCentre.y() - theRadius, 2.0 * theRadius,
                     2.0 * theRadius);
    thePainter.setPen(QPen(theColours.color(QPalette::Mid), 1.5));
    thePainter.setBrush(pill);
    thePainter.drawEllipse(hub);
    if (theLit >= 0) {
        // Qt's angles start at three o'clock and run anticlockwise;
        // wedge i is centred i * 45 degrees clockwise from twelve.
        const double start = 90.0 - theLit * 45.0 - 22.5;
        thePainter.setPen(Qt::NoPen);
        thePainter.setBrush(theLitColour);
        thePainter.drawPie(hub.adjusted(3, 3, -3, -3), static_cast<int>(start * 16.0), 45 * 16);
    }
}

void PaintPill(QPainter& thePainter, const QRectF& theRect, const MarkingMenu::Item& theItem,
               bool theLit, const QPalette& theColours)
{
    QColor pill = theColours.color(QPalette::Window);
    pill.setAlpha(235);
    thePainter.setPen(QPen(theLit ? kHighlight : theColours.color(QPalette::Mid), 1.0));
    thePainter.setBrush(theLit ? kHighlight : pill);
    thePainter.drawRoundedRect(theRect, 4.0, 4.0);

    QColor text = theColours.color(QPalette::WindowText);
    if (theLit) {
        text = Qt::white;
    } else if (!theItem.enabled) {
        text = theColours.color(QPalette::Disabled, QPalette::WindowText);
    }
    thePainter.setPen(text);
    thePainter.drawText(theRect, Qt::AlignCenter, theItem.label);
}

} // namespace

// ---- MarkingMenu ----

MarkingMenu::MarkingMenu(const ItemList& theItems, QWidget* theParent)
    : QWidget(theParent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , m_items(theItems)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);

    // Big enough for the widest label out at east or west, whichever side.
    const QFontMetrics metrics(font());
    double widest = 0.0;
    for (const Item& item : m_items) {
        widest = std::max(widest, static_cast<double>(metrics.horizontalAdvance(item.label)));
    }
    const double halfWidth = kRingRadius + widest + 2.0 * kPillPadding + kMargin;
    const double halfHeight = kRingRadius + kPillHeight + kMargin;
    resize(static_cast<int>(std::ceil(2.0 * halfWidth)), static_cast<int>(std::ceil(2.0 * halfHeight)));
    m_centre = QPoint(width() / 2, height() / 2);
}

void MarkingMenu::PopUp(const QPoint& theGlobalPos, bool theButtonHeld)
{
    m_awaitingRelease = theButtonHeld;
    move(theGlobalPos - m_centre);
    show();
    setFocus();
}

void MarkingMenu::PointAt(const QPoint& theGlobalPos)
{
    HighlightAt(mapFromGlobal(QPointF(theGlobalPos)));
}

void MarkingMenu::ReleaseAt(const QPoint& theGlobalPos)
{
    if (!m_awaitingRelease) {
        return;
    }
    m_awaitingRelease = false;
    const QPointF pos = mapFromGlobal(QPointF(theGlobalPos));
    HighlightAt(pos);
    const int wedge = ActiveWedgeAt(pos);
    if (wedge >= 0) {
        Activate(wedge);
    }
    // In the dead zone, or over a greyed wedge: stay open. The hand let go
    // without choosing, and the ring is now an ordinary click-to-pick one.
}

int MarkingMenu::ActiveWedgeAt(const QPointF& thePos) const
{
    const int wedge = MarkingMenuWedgeAt(thePos.x() - m_centre.x(), thePos.y() - m_centre.y(),
                                         kDeadZone);
    if (wedge < 0 || m_items[wedge].label.isEmpty() || !m_items[wedge].enabled) {
        return -1;
    }
    return wedge;
}

void MarkingMenu::HighlightAt(const QPointF& thePos)
{
    const int wedge = ActiveWedgeAt(thePos);
    if (wedge != m_highlight) {
        m_highlight = wedge;
        const QString tip = wedge >= 0 ? m_items[wedge].tooltip : QString();
        setToolTip(tip);
        update();
    }
}

void MarkingMenu::paintEvent(QPaintEvent* /*theEvent*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    PaintHub(painter, m_centre, kDeadZone, m_highlight, kHighlight, palette());

    const QFontMetrics metrics(font());
    for (int i = 0; i < kMarkingMenuWedges; ++i) {
        const Item& item = m_items[i];
        if (item.label.isEmpty()) {
            continue;
        }
        PaintPill(painter, PillRect(metrics, item.label, i, m_centre), item, i == m_highlight,
                  palette());
    }
}

void MarkingMenu::mouseMoveEvent(QMouseEvent* theEvent)
{
    HighlightAt(theEvent->position());
}

void MarkingMenu::mousePressEvent(QMouseEvent* theEvent)
{
    // A press outside the popup never arrives here: Qt closes a popup on
    // an outside click by itself.
    const QPointF pos = theEvent->position();
    const int wedge = ActiveWedgeAt(pos);
    if (wedge >= 0) {
        Activate(wedge);
        return;
    }
    if (std::hypot(pos.x() - m_centre.x(), pos.y() - m_centre.y()) < kDeadZone) {
        close();
    }
    // A greyed or empty wedge: stay open, the user has not chosen yet.
}

void MarkingMenu::mouseReleaseEvent(QMouseEvent* theEvent)
{
    // The release of the right button that was HELD to bring the ring up.
    // A popup grabs the pointer as it opens, so on a real display that
    // release lands here rather than on the canvas where the press was.
    if (theEvent->button() == Qt::RightButton) {
        ReleaseAt(theEvent->globalPosition().toPoint());
    }
}

void MarkingMenu::keyPressEvent(QKeyEvent* theEvent)
{
    if (theEvent->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(theEvent);
}

void MarkingMenu::Activate(int theWedge)
{
    // Copied out first: closing deletes this widget (WA_DeleteOnClose).
    const std::function<void()> action = m_items[theWedge].action;
    close();
    if (action) {
        QTimer::singleShot(0, qApp, action);
    }
}

// ---- MarkingMenuTrail ----

MarkingMenuTrail::MarkingMenuTrail(QWidget* theParent)
    : QWidget(theParent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
                             | Qt::WindowTransparentForInput)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    const int half = static_cast<int>(kTrailHalfSize);
    resize(2 * half, 2 * half);
    m_centre = QPoint(half, half);
}

void MarkingMenuTrail::Track(const QPoint& thePressGlobal, const QPoint& theCursorGlobal,
                             const MarkingMenu::ItemList& theItems)
{
    m_items = theItems;
    const QPoint travel = theCursorGlobal - thePressGlobal;
    m_cursor = m_centre + travel;
    m_wedge = MarkingGestureWedge(travel.x(), travel.y());
    move(thePressGlobal - m_centre);
    if (!isVisible()) {
        show();
    }
    update();
}

const MarkingMenu::Item* MarkingMenuTrail::LitItem() const
{
    return m_wedge >= 0 ? &m_items[static_cast<std::size_t>(m_wedge)] : nullptr;
}

void MarkingMenuTrail::paintEvent(QPaintEvent* /*theEvent*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const MarkingMenu::Item* item = LitItem();
    const bool named = item != nullptr && !item->label.isEmpty();
    const bool live = named && item->enabled;
    // Grey for a wedge that will not run, so the hand learns that letting
    // go here does nothing before it lets go.
    const QColor stroke = live ? kHighlight : palette().color(QPalette::Mid);

    // The stroke first, so the hub sits over its root.
    const QPointF centre(m_centre);
    const QPointF cursor(m_cursor);
    const double length = QLineF(centre, cursor).length();
    if (length > kMarkingGestureTravel) {
        const QPointF start = centre + (cursor - centre) * (kMarkingGestureTravel / length);
        painter.setPen(QPen(stroke, 3.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(start, cursor);
    }

    PaintHub(painter, centre, kMarkingGestureTravel, named ? m_wedge : -1, stroke, palette());

    if (named) {
        PaintPill(painter, PillRect(QFontMetrics(font()), item->label, m_wedge, centre), *item, live,
                  palette());
    }
}

} // namespace lcad
