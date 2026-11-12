// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/theme.h"
#include <QList>
#include <QStyle>
#include <QWidget>
#include <cmath>
#include <utility>

namespace mf {
namespace {
QColor mix(const QColor &a, const QColor &b, double share) { // share of a
    return QColor::fromRgbF(float(a.redF() * share + b.redF() * (1 - share)),
                            float(a.greenF() * share + b.greenF() * (1 - share)),
                            float(a.blueF() * share + b.blueF() * (1 - share)));
}
double luminance(const QColor &c) {
    const auto channel = [](double v) { return v <= .03928 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * channel(c.redF()) + .7152 * channel(c.greenF()) + .0722 * channel(c.blueF());
}
double contrast(const QColor &a, const QColor &b) {
    auto x = luminance(a), y = luminance(b);
    if (x < y)
        std::swap(x, y);
    return (x + .05) / (y + .05);
}
// The first color on the way from `from` to `to` that reads at 4.5:1 on the background.
QColor readable(const QColor &from, const QColor &to, const QColor &background) {
    for (int step = 0; step <= 10; ++step)
        if (const auto c = mix(to, from, step / 10.); contrast(c, background) >= 4.5)
            return c;
    return to;
}
} // namespace

ChromeColors::ChromeColors(const QPalette &p) {
    base = p.color(QPalette::Base);
    window = p.color(QPalette::Window);
    text = p.color(QPalette::Text);
    const bool dark = window.lightness() < 128;
    // MolForma's own accent, the cobalt of the redesign, whatever the desktop's selection color
    // (a pale one would wash out the chosen tool and step); a little lighter on dark themes.
    accent = QColor(dark ? "#2f6fe4" : "#1d5fd1");
    onAccent = QColor(Qt::white);
    line = mix(text, base, .17);
    hover = mix(text, base, .07);
    segment = mix(text, window, .08);
    tint = mix(accent, base, .16);
    muted = readable(mix(text, base, .55), text, base);
    accentText = readable(accent, text, tint);
    candidate = QColor(dark ? "#3b2a12" : "#fff4e5");
    candidateLine = QColor(dark ? "#8a5a1c" : "#f3c98b");
    candidateText = QColor(dark ? "#fcd9a8" : "#5c2e04");
    good = goodColor(p);
    goodTint = mix(good, base, .13);
    bad = errorColor(p);
    badTint = mix(bad, base, .12);
}

QString chromeRules(QString rules, const QPalette &palette) {
    const ChromeColors c(palette);
    const auto accentHover = c.accent.lightness() > 128 ? c.accent.darker(112) : c.accent.lighter(115);
    // Longer names first, so that "@accentText" is not taken for "@accent" followed by "Text".
    const QList<std::pair<const char *, QColor>> tokens{{"@candidateLine", c.candidateLine},
                                                        {"@candidateText", c.candidateText},
                                                        {"@accentHover", accentHover},
                                                        {"@accentText", c.accentText},
                                                        {"@candidate", c.candidate},
                                                        {"@goodTint", c.goodTint},
                                                        {"@onAccent", c.onAccent},
                                                        {"@badTint", c.badTint},
                                                        {"@segment", c.segment},
                                                        {"@accent", c.accent},
                                                        {"@window", c.window},
                                                        {"@error", c.bad},
                                                        {"@hover", c.hover},
                                                        {"@muted", c.muted},
                                                        {"@good", c.good},
                                                        {"@base", c.base},
                                                        {"@line", c.line},
                                                        {"@tint", c.tint},
                                                        {"@text", c.text}};
    for (const auto &[token, color] : tokens)
        rules.replace(token, color.name());
    return rules;
}

QString settingsRules() {
    return R"(
        QFrame[toolbar="true"] { background: @base; border: none; border-bottom: 1px solid @line; }
        QFrame[banner="true"] { background: @candidate; border: none; border-bottom: 1px solid @candidateLine; }
        QFrame[banner="true"] QLabel { color: @candidateText; }
        QFrame[card="true"] { background: @base; border: 1px solid @line; border-radius: 10px; }
        QFrame[card="true"][state="ok"] { background: @goodTint; }
        QFrame[card="true"][state="warn"] { background: @candidate; border-color: @candidateLine; }
        QFrame[card="true"][state="error"] { background: @badTint; }
        QLabel[headline="ok"] { color: @good; font-weight: bold; }
        QLabel[headline="warn"] { color: @candidateText; font-weight: bold; }
        QLabel[headline="error"] { color: @error; font-weight: bold; }
        QLabel[headline="waiting"] { color: @muted; font-weight: bold; }
        QLabel[cardTitle="true"] { color: @text; font-weight: bold; font-size: 14px; }
        QLabel[caption="true"] { color: @muted; font-size: 11px; }
        QLabel[muted="true"] { color: @muted; }
        QLabel[good="true"] { color: @good; font-weight: bold; }
        QLabel[bad="true"] { color: @error; font-weight: bold; }
        QLabel[mono="true"] { font-family: monospace; }
        QFrame[choice="segments"] { background: @segment; border: none; border-radius: 7px; }
        QFrame[choice="segments"] QToolButton { border: none; border-radius: 5px; padding: 4px 10px; color: @muted;
                                                background: transparent; }
        QFrame[choice="segments"] QToolButton:hover { color: @text; }
        QFrame[choice="segments"] QToolButton:checked { background: @base; color: @accentText; font-weight: bold; }
        QFrame[choice="chips"] { background: transparent; border: none; }
        QFrame[choice="chips"] QToolButton { border: 1px solid @line; border-radius: 13px; padding: 4px 11px;
                                             background: @base; color: @text; }
        QFrame[choice="chips"] QToolButton:hover { background: @hover; }
        QFrame[choice="chips"] QToolButton:checked { background: @tint; border-color: @accent; color: @accentText;
                                                     font-weight: bold; }
        QFrame[choice] QToolButton:disabled { color: @muted; }
        QFrame[choice] QLabel { color: @error; }
        QPushButton { border: 1px solid @line; border-radius: 6px; background: @base; color: @text; padding: 5px 10px; }
        QPushButton:hover { background: @hover; }
        QPushButton:disabled { color: @muted; border-color: @hover; }
        QPushButton[primary="true"] { background: @accent; border-color: @accent; color: @onAccent; font-weight: bold;
                                      padding: 6px 14px; }
        QPushButton[primary="true"]:hover { background: @accentHover; }
        QPushButton[primary="true"]:disabled { background: @line; border-color: @line; color: @muted; }
        QPushButton[danger="true"] { color: @error; border-color: @error; font-weight: bold; }
        QPushButton[danger="true"]:disabled { color: @muted; border-color: @hover; font-weight: normal; }
        QPushButton[link="true"] { border: none; background: transparent; color: @accentText; padding: 2px 0;
                                   text-align: left; }
        QPushButton[link="true"]:hover { text-decoration: underline; }
        QPushButton[flat="true"] { border: none; background: transparent; padding: 3px; }
        QPushButton[flat="true"]:hover { background: @hover; }
        QToolButton[fold="true"] { border: none; border-radius: 5px; padding: 3px; }
        QToolButton[fold="true"]:hover { background: @hover; }
        QToolButton[disclosure="true"] { border: none; background: transparent; color: @accentText;
            font-weight: bold; padding: 6px 2px 2px 0; }
        QLabel[disclosureDetail="true"] { color: @muted; padding-top: 4px; }
        QToolButton[flat="true"] { border: none; border-radius: 5px; padding: 3px 6px; color: @text; }
        QToolButton[flat="true"]:hover { background: @hover; }
        QToolButton[flat="true"]:checked { background: @tint; color: @accentText; font-weight: bold; }
        QLineEdit[search="true"] { border: 1px solid @line; border-radius: 6px; padding: 4px; background: @base;
                                   color: @text; }
        QScrollArea[canvas="true"] { background: @window; border: none; }
        QWidget[canvas="true"] { background: @window; }
        QListWidget[nav="true"] { background: @base; border: none; }
        QTabWidget[tabs="underline"]::pane { background: @base; border: none; border-top: 1px solid @line; }
        QTabWidget[tabs="underline"]::tab-bar { left: 4px; }
        QTabWidget[tabs="underline"] > QTabBar { background: transparent; }
        QTabWidget[tabs="underline"] > QTabBar::tab { background: transparent; border: none; color: @muted;
                                                      border-bottom: 2px solid transparent; padding: 6px 3px;
                                                      margin-right: 14px; }
        QTabWidget[tabs="underline"] > QTabBar::tab:hover { color: @text; }
        QTabWidget[tabs="underline"] > QTabBar::tab:selected { color: @accentText; border-bottom-color: @accent; }
        QTabWidget[tabs="underline"] > QTabBar::tab:disabled { color: @line; }
        QTableView[plain="true"], QTreeView[plain="true"] { background: @base; border: 1px solid @line;
            border-radius: 8px; gridline-color: @hover; selection-background-color: @tint; selection-color: @text;
            alternate-background-color: @window; }
        QTableView[plain="true"]::item:selected, QTreeView[plain="true"]::item:selected { background: @tint;
                                                                                         color: @text; }
        QTableView[plain="true"] QHeaderView::section, QTreeView[plain="true"] QHeaderView::section {
            background: @base; color: @muted; border: none; border-bottom: 1px solid @line; padding: 5px 8px; }
        QTableView[plain="true"] QTableCornerButton::section { background: @base; border: none; }
        QPlainTextEdit[log="true"] { background: @window; border: 1px solid @line; border-radius: 6px; }
    )";
}
void repolish(QWidget *widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}
} // namespace mf
