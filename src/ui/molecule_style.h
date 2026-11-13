// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QColor>
#include <QString>
#include <QVector>
#include <algorithm>

namespace mf {
// How the 3D view draws a molecule: what is drawn, how large and in which colors, the same with QPainter
// (molecule_view) and with VTK (vtk_molecule_renderer). The two only differ in how they put it on the
// screen. Lengths are in pixels; `scale` is that of the view, in pixels per Å. The display styles are
// "ball_and_stick", "stick", "spacefill", "wireframe" and "backbone".

// The background of a structure, a gradient from the top of the view to its bottom, in either theme.
inline QColor viewBackground(bool top) {
    return QColor(top ? "#f8fafc" : "#e9eff4");
}
// Whether an atom is drawn as a sphere: a ghost and a hydrogen of a view without hydrogens keep their
// flat marks, drawn over the scene.
inline bool atomSphere(const Atom &atom, bool hydrogens) {
    return !atom.ghost && (hydrogens || atom.element != "H");
}
// Whether the bond between two shown atoms is drawn: not one to a hydrogen of a view without hydrogens.
inline bool bondShown(const Atom &a, const Atom &b, bool hydrogens) {
    return hydrogens || (a.element != "H" && b.element != "H");
}
// What an atom is for its size: a hydrogen is smaller than the others; an atom without a bond is all that
// the wireframe style shows of it (an ion), and an atom the backbone line runs through is as thin as the
// line of a stick.
enum class AtomSize { Heavy, Hydrogen, Unbonded, TraceNode };
inline AtomSize atomSize(const QString &style, const Atom &atom, bool bonded, bool traceNode) {
    return style == "wireframe" && !bonded ? AtomSize::Unbonded
           : traceNode                     ? AtomSize::TraceNode
           : atom.element == "H"           ? AtomSize::Hydrogen
                                           : AtomSize::Heavy;
}
// The radius of an atom's sphere. Spacefill spheres have the van der Waals radius (vdw, Å) of the element;
// the others are sizes on the screen that grow with the scale between two limits. 0: the style draws no
// sphere for it (the atoms of a wireframe with bonds, which its lines show).
inline double atomRadius(const QString &style, AtomSize size, double scale, double vdw) {
    if (style == "spacefill")
        return vdw * scale;
    if (style == "stick" || size == AtomSize::TraceNode)
        return std::clamp(scale * .1, 3., 7.);
    if (style == "wireframe")
        return size == AtomSize::Unbonded ? std::clamp(scale * .12, 4., 8.) : 0.;
    const bool hydrogen = size == AtomSize::Hydrogen;
    return std::clamp(scale * (hydrogen ? .12 : .20), hydrogen ? 5. : 8., hydrogen ? 11. : 20.);
}
// The width of the line of a bond, and of the line of the backbone style.
inline double bondWidth(const QString &style, double scale) {
    return style == "stick" ? std::clamp(scale * .2, 5., 12.) : style == "wireframe" ? 2. : 3.5;
}
inline double traceWidth(double scale) {
    return std::clamp(scale * .35, 4., 16.);
}
// How a bond is drawn in ball-and-stick.
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
