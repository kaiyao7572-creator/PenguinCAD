#include "ui/UiUtils.h"

#include "core/Document.h"

namespace lcad {

QColor ErrorTextColor(const QPalette& thePalette)
{
    // QPalette::Window tracks the desktop theme; use its lightness as a
    // cheap dark/light detector rather than hardcoding one compromise red
    // that looks muddy on dark and washed out on light.
    const int lightness = thePalette.color(QPalette::Window).lightness();
    return (lightness < 128) ? QColor(0xFF, 0x6B, 0x6B)    // dark theme: lighter coral for contrast
                              : QColor(0xC0, 0x1C, 0x28);   // light theme: deep red (GNOME's own destructive-red)
}

FeaturePtr FindFeatureByRaw(const Document& theDocument, const Feature* theRaw)
{
    if (theRaw == nullptr) {
        return nullptr;
    }
    for (const FeaturePtr& feature : theDocument.Features()) {
        if (feature.get() == theRaw) {
            return feature;
        }
    }
    return nullptr;
}

} // namespace lcad
