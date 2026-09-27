#pragma once

#include <QIcon>
#include <QImage>
#include <QPalette>
#include <QString>

#include <string>

namespace lcad {

// Command::Icon() names a bundled SVG, ":/icons/<command id>.svg". Anything
// else is taken as text and shown in place of an icon.
bool IsIconResource(const std::string& theIcon);

// The icon drawn for this palette, or a null QIcon when theIcon is text or
// the SVG cannot be read -- the caller then falls back to the label alone,
// so a machine without Qt's SVG plugin still gets a usable ribbon.
QIcon CommandIcon(const std::string& theIcon, const QPalette& thePalette);

// One rendering at an exact pixel size, for the startup check and for
// contact sheets. Null when the resource is missing or unreadable.
QImage RenderCommandIcon(const QString& theResource, int thePixels, bool theDarkTheme);

// True for a palette the icons should be recoloured for.
bool IsDarkPalette(const QPalette& thePalette);

} // namespace lcad
