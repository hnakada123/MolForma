#pragma once
#include <QColor>
#include <QPalette>

namespace mf {
// Colors derived from the widget palette, so light and dark themes both stay readable.
inline bool darkPalette(const QPalette &palette) {
    return palette.color(QPalette::Window).lightness() < 128;
}
inline QColor errorColor(const QPalette &palette) {
    return darkPalette(palette) ? QColor("#ff7b72") : QColor("#b91c1c");
}
// Accent colors for syntax highlighting: the same hue, lighter on dark backgrounds.
inline QColor accentColor(const QPalette &palette, const char *light, const char *dark) {
    return QColor(darkPalette(palette) ? dark : light);
}
} // namespace mf
