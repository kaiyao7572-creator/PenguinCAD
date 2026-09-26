#include "ui/MarkingMenu.h"
#include "ui/MarkingMenuGeometry.h"

#include <QApplication>
#include <QFontMetrics>
#include <QKeyEvent>
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

// Fusion's highlight blue.
const QColor kHighlight(6, 150, 215);

} // namespace

MarkingMenu::MarkingMenu(const std::array<Item, 8>& theItems, QWidget* theParent)
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

void MarkingMenu::PopUp(const QPoint& theGlobalPos)
{
    move(theGlobalPos - m_centre);
    show();
    setFocus();
}

QRectF MarkingMenu::LabelRect(int theIndex) const
{
    double dx = 0.0;
    double dy = 0.0;
    MarkingMenuWedgeAnchor(theIndex, kRingRadius, dx, dy);

    const QFontMetrics metrics(font());
    const double width = metrics.horizontalAdvance(m_items[theIndex].label) + 2.0 * kPillPadding;
    const QPointF anchor(m_centre.x() + dx, m_centre.y() + dy);

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

int MarkingMenu::ActiveWedgeAt(const QPointF& thePos) const
{
    const int wedge = MarkingMenuWedgeAt(thePos.x() - m_centre.x(), thePos.y() - m_centre.y(),
                                         kDeadZone);
    if (wedge < 0 || m_items[wedge].label.isEmpty() || !m_items[wedge].enabled) {
        return -1;
    }
    return wedge;
}

void MarkingMenu::paintEvent(QPaintEvent* /*theEvent*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QPalette& colours = palette();
    QColor pill = colours.color(QPalette::Window);
    pill.setAlpha(235);
    const QColor border = colours.color(QPalette::Mid);

    // The centre: a ring, with the lit wedge's slice filled in so the eye
    // can see which way it is pointing before reading any label.
    const QRectF hub(m_centre.x() - kDeadZone, m_centre.y() - kDeadZone, 2.0 * kDeadZone,
                     2.0 * kDeadZone);
    painter.setPen(QPen(border, 1.5));
    painter.setBrush(pill);
    painter.drawEllipse(hub);
    if (m_highlight >= 0) {
        // Qt's angles start at three o'clock and run anticlockwise;
        // wedge i is centred i * 45 degrees clockwise from twelve.
        const double start = 90.0 - m_highlight * 45.0 - 22.5;
        painter.setPen(Qt::NoPen);
        painter.setBrush(kHighlight);
        painter.drawPie(hub.adjusted(3, 3, -3, -3), static_cast<int>(start * 16.0), 45 * 16);
    }

    for (int i = 0; i < kMarkingMenuWedges; ++i) {
        const Item& item = m_items[i];
        if (item.label.isEmpty()) {
            continue;
        }
        const QRectF rect = LabelRect(i);
        const bool lit = (i == m_highlight);
        painter.setPen(QPen(lit ? kHighlight : border, 1.0));
        painter.setBrush(lit ? kHighlight : pill);
        painter.drawRoundedRect(rect, 4.0, 4.0);

        QColor text = colours.color(QPalette::WindowText);
        if (lit) {
            text = Qt::white;
        } else if (!item.enabled) {
            text = colours.color(QPalette::Disabled, QPalette::WindowText);
        }
        painter.setPen(text);
        painter.drawText(rect, Qt::AlignCenter, item.label);
    }
}

void MarkingMenu::mouseMoveEvent(QMouseEvent* theEvent)
{
    const int wedge = ActiveWedgeAt(theEvent->position());
    if (wedge != m_highlight) {
        m_highlight = wedge;
        const QString tip = wedge >= 0 ? m_items[wedge].tooltip : QString();
        setToolTip(tip);
        update();
    }
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

} // namespace lcad
