#include "ui/CommandIcon.h"

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QImageReader>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QStyleOption>

#include <utility>

namespace lcad {

namespace {

// The icons are drawn for a light toolbar, the way Fusion's are. Line work
// that sits on bare background -- a sketch line, a dimension, an arrow -- is
// stroked in currentColor, which the root <svg color="..."> sets; outlines
// that sit on a filled face keep their literal dark ink. On a dark palette
// only that root colour changes, so a sketch line turns light enough to
// read while a solid keeps crisp dark edges against its own light faces.
// Replacing the one attribute, rather than every dark stroke, is what keeps
// those two cases apart.
const QByteArray kLightInk = "color=\"#2B3A48\"";
const QByteArray kDarkInk = "color=\"#D3DCE4\"";

QByteArray ReadSvg(const QString& theResource, bool theDarkTheme)
{
    QFile file(theResource);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    QByteArray svg = file.readAll();
    if (theDarkTheme) {
        svg.replace(kLightInk, kDarkInk);
    }
    return svg;
}

// Through the SVG image-format plugin rather than QSvgRenderer: this box
// has Qt's SVG plugins but not the QtSvg development headers, and the
// plugin is all a rasterisation needs.
QImage Rasterize(const QByteArray& theSvg, const QSize& thePixels)
{
    if (theSvg.isEmpty() || thePixels.isEmpty()) {
        return QImage();
    }
    QBuffer buffer;
    buffer.setData(theSvg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "svg");
    reader.setScaledSize(thePixels);
    return reader.read();
}

// Renders at exactly the size and device pixel ratio each widget asks for,
// so a 32 px ribbon icon on a 2x screen is drawn from the SVG at 64 px
// rather than blown up from a smaller bitmap. Disabled and selected looks
// come from the style, the same way Qt greys any other icon.
class SvgIconEngine : public QIconEngine
{
public:
    explicit SvgIconEngine(QByteArray theSvg)
        : mySvg(std::move(theSvg))
    {
    }

    void paint(QPainter* thePainter, const QRect& theRect, QIcon::Mode theMode,
               QIcon::State theState) override
    {
        const qreal scale = thePainter->device() != nullptr
                                ? thePainter->device()->devicePixelRatio()
                                : 1.0;
        thePainter->drawPixmap(theRect, scaledPixmap(theRect.size(), theMode, theState, scale));
    }

    QSize actualSize(const QSize& theSize, QIcon::Mode /*theMode*/,
                     QIcon::State /*theState*/) override
    {
        return theSize;
    }

    QPixmap pixmap(const QSize& theSize, QIcon::Mode theMode, QIcon::State theState) override
    {
        return scaledPixmap(theSize, theMode, theState, 1.0);
    }

    // Qt 6 hands this the LOGICAL size and the scale separately.
    QPixmap scaledPixmap(const QSize& theSize, QIcon::Mode theMode, QIcon::State /*theState*/,
                         qreal theScale) override
    {
        const QSize pixels = (QSizeF(theSize) * theScale).toSize();
        const QString key = QString("%1x%2/%3").arg(pixels.width()).arg(pixels.height()).arg(int(theMode));
        const auto cached = myCache.constFind(key);
        if (cached != myCache.constEnd()) {
            return *cached;
        }

        QPixmap pixmap = QPixmap::fromImage(Rasterize(mySvg, pixels));
        if (!pixmap.isNull() && theMode != QIcon::Normal && QApplication::style() != nullptr) {
            QStyleOption option;
            option.palette = QApplication::palette();
            pixmap = QApplication::style()->generatedIconPixmap(theMode, pixmap, &option);
        }
        pixmap.setDevicePixelRatio(theScale);
        myCache.insert(key, pixmap);
        return pixmap;
    }

    QIconEngine* clone() const override { return new SvgIconEngine(*this); }
    QString key() const override { return QStringLiteral("lcad-svg"); }
    bool isNull() override { return mySvg.isEmpty(); }

private:
    QByteArray              mySvg;
    QHash<QString, QPixmap> myCache;
};

} // namespace

bool IsIconResource(const std::string& theIcon)
{
    return theIcon.rfind(":/", 0) == 0;
}

bool IsDarkPalette(const QPalette& thePalette)
{
    // Same test the panels use for their error red (ui/UiUtils.cpp).
    return thePalette.color(QPalette::Window).lightness() < 128;
}

QIcon CommandIcon(const std::string& theIcon, const QPalette& thePalette)
{
    if (!IsIconResource(theIcon)) {
        return QIcon();
    }
    const QString resource = QString::fromStdString(theIcon);
    QByteArray svg = ReadSvg(resource, IsDarkPalette(thePalette));
    // Refuse up front what could never draw, so the caller keeps its text
    // rather than showing an empty square.
    if (Rasterize(svg, QSize(16, 16)).isNull()) {
        return QIcon();
    }
    return QIcon(new SvgIconEngine(std::move(svg)));
}

QImage RenderCommandIcon(const QString& theResource, int thePixels, bool theDarkTheme)
{
    return Rasterize(ReadSvg(theResource, theDarkTheme), QSize(thePixels, thePixels));
}

} // namespace lcad
