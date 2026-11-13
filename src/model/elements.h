// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <QVector>

namespace mf {
// A color as the model keeps it: plain channels, so that the model needs no Qt Gui. The UI makes a QColor
// of it (toColor in ui/theme.h).
struct Rgb {
    quint8 red = 0, green = 0, blue = 0;
    bool operator==(const Rgb &) const = default;
};
// The element table (assets/elements.json, embedded): symbol, atomic number, standard atomic mass,
// covalent and van der Waals radius (Å), display color and names.
struct Element {
    QString symbol;
    int number;
    double mass, radius, vdw;
    Rgb color;
    QString nameJa, nameEn;
    QString name() const; // In the interface language.
    // Whether the element is a metal: every element but H, B, C, N, O, Si, P, S, Ge, As, Se, Sb, Te, Po and
    // the halogens and noble gases up to At and Rn. Bonds to metals are coordination, not valence: they
    // stay single when bond orders are found from a geometry, and a cut through one gets no hydrogen.
    bool metal() const;
};
const QVector<Element> &elements();
const Element &element(const QString &symbol);
} // namespace mf
