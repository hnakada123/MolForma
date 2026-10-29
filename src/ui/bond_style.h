// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QColor>
#include <QString>
#include <QVector>

namespace mf {
// How the 3D view draws a bond in ball-and-stick, the same with QPainter (molecule_view) and with
// VTK (vtk_molecule_renderer). Lengths are in pixels.
struct BondStyle {
    static constexpr double Spacing = 5;    // Between the lines of a multiple bond.
    static constexpr double DashOffset = 5; // The dashed line beside an aromatic bond.
    // The arrowhead of a dative bond: how far along the bond, its length and half its width.
    static constexpr double ArrowAt = .72, ArrowLength = 9, ArrowHalfWidth = 4;
    QColor color;
    QVector<double> offsets; // The parallel lines across the bond: one, two or three.
    bool dashed = false;     // Aromatic.
    bool arrow = false;      // Dative, pointing at the second atom.
};
// The style of each bond kind, made once: views draw every bond on every frame.
inline const BondStyle &bondStyle(const QString &kind) {
    static const auto styles = [] {
        QVector<std::pair<QString, BondStyle>> out;
        for (const QString kind : {"single", "double", "triple", "aromatic", "dative"}) {
            BondStyle style;
            style.color = QColor(kind == "dative" ? "#2879bd" : kind == "aromatic" ? "#a36a2b" : "#748595");
            const int count = kind == "double" ? 2 : kind == "triple" ? 3 : 1;
            for (int j = 0; j < count; ++j)
                style.offsets << (j - (count - 1) * .5) * BondStyle::Spacing;
            style.dashed = kind == "aromatic";
            style.arrow = kind == "dative";
            out.append({kind, style});
        }
        return out;
    }();
    for (const auto &[name, style] : styles)
        if (name == kind)
            return style;
    return styles.first().second; // Other kinds are drawn as single bonds.
}
// Whether a bond is drawn as one plain line along it, the same from every direction.
inline bool plainBond(const BondStyle &style) {
    return style.offsets.size() == 1 && !style.dashed && !style.arrow;
}
// Stick and wireframe draw each bond as two halves in the colors of its atoms, one line per bond.
inline bool halfBonds(const QString &displayStyle) {
    return displayStyle == "stick" || displayStyle == "wireframe";
}
// The positive and negative lobes of an isosurface.
inline QColor surfaceColor(bool positive) {
    return QColor(positive ? "#ce5268" : "#397cce");
}
} // namespace mf
