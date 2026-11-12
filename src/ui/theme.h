// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/elements.h"
#include <QColor>
#include <QPalette>
#include <QString>
class QWidget;

namespace mf {
// The colors and the style rules of the interface, all of them here: the model's colors as Qt draws them,
// those derived from the widget palette, the chrome colors over both (ChromeColors) and the style sheets
// written with their tokens.

// A color of the model (an element's) as Qt draws it.
inline QColor toColor(const Rgb &color) {
    return QColor(color.red, color.green, color.blue);
}
// Colors derived from the widget palette, so light and dark themes both stay readable.
inline bool darkPalette(const QPalette &palette) {
    return palette.color(QPalette::Window).lightness() < 128;
}
inline QColor errorColor(const QPalette &palette) {
    return darkPalette(palette) ? QColor("#ff7b72") : QColor("#b91c1c");
}
// Values that pass a check (a converged criterion, a normal termination).
inline QColor goodColor(const QPalette &palette) {
    return darkPalette(palette) ? QColor("#7ee2a0") : QColor("#17602c");
}
// Accent colors for syntax highlighting: the same hue, lighter on dark backgrounds.
inline QColor accentColor(const QPalette &palette, const char *light, const char *dark) {
    return QColor(darkPalette(palette) ? dark : light);
}

// Colors of the editor chrome (header, tool rail, option bar, overlays): MolForma's own accent, and
// the rest derived from the widget palette so that light and dark themes both stay readable.
// accentText keeps 4.5:1 on tint.
struct ChromeColors {
    QColor base, window, text, muted, line, hover, segment, accent, accentText, tint, onAccent;
    QColor candidate, candidateLine, candidateText; // The banner of a pending change.
    QColor good, goodTint, bad, badTint;            // Outcomes: text and a tinted background.
    explicit ChromeColors(const QPalette &palette);
};
// A style sheet with the chrome colors put in for the tokens "@accent", "@accentText", "@base",
// "@window", "@text", "@muted", "@line", "@hover", "@segment", "@tint", "@onAccent", "@accentHover",
// "@candidate…", "@good", "@goodTint", "@error" and "@badTint".
QString chromeRules(QString rules, const QPalette &palette);
// The rules shared by every tab, as tokens for chromeRules(): the toolbar and banner rows, cards
// (with their states), segmented controls and chips (ChoiceButtons), buttons (with the properties
// primary, danger, link and flat), search fields, status texts, tabs with an underline (a QTabWidget
// with the property tabs="underline"), plain tables (plain=true) and logs (log=true).
QString settingsRules();
// Applies style rules that depend on a dynamic property ("state", "pill") again.
void repolish(QWidget *widget);
} // namespace mf
