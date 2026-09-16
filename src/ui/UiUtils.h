#pragma once

#include "core/Feature.h"

#include <QColor>
#include <QPalette>

namespace lcad {

class Document;

// A single fixed hex code only reads well against one theme -- GNOME dark
// needs a lighter red than a light desktop does to stay legible. Picks
// between two hand-tuned shades based on how dark the caller's current
// palette actually is, so panels never have to hardcode a background or
// text color to get a theme-appropriate error red.
QColor ErrorTextColor(const QPalette& thePalette);

// The tree/timeline widgets tag each row/button with the Feature it
// represents as a raw pointer (QVariant has no built-in shared_ptr
// support), so this turns that back into the owning FeaturePtr for
// document mutators, which all take FeaturePtr. Mirrors the linear search
// Document::IndexOf does internally.
FeaturePtr FindFeatureByRaw(const Document& theDocument, const Feature* theRaw);

} // namespace lcad
