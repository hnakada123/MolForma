// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
namespace mf {
// Chemistry through the Open Babel C++ API. Reading and writing structure files with it is in
// io/babel_io.h.
// Reads XYZ text and assigns bond orders from the geometry; XYZ itself carries none.
Document xyzStructure(const QString &text);
Document fromSmiles(const QString &smiles);
// Whether the molecule has a metal atom, which fromSmiles and refine() do not build (Open Babel's metals).
bool hasMetal(const Document &document);
Document refine(const Document &d, bool regenerate = false, const Ids &moving = {});
struct Conformer {
    Document document;
    double energy = 0; // Force-field energy in kcal/mol.
    QString forceField;
};
// Rotor search with the MMFF94 (else UFF) force field: up to `count` distinct conformers, lowest
// energy first, each minimized. The atoms and their order stay; fixed atoms are not allowed.
QVector<Conformer> searchConformers(const Document &d, int count = 10);
} // namespace mf
