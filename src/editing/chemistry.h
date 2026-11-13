// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
namespace mf {
// Chemistry through the Open Babel C++ API. Reading and writing structure files with it is in
// io/babel_io.h.
// The bonds of atoms that come with none (XYZ, a crystal, the geometry of an NWChem input), in place of
// those the document has: by distance (Document::inferBonds), with their orders from the geometry. Bonds
// to metals and ghosts stay single.
void perceiveBonds(Document &document);
// Reads XYZ text, with the bonds of perceiveBonds.
Document xyzStructure(const QString &text);
Document fromSmiles(const QString &smiles);
// Whether the molecule has a metal atom (Element::metal), which fromSmiles and refine() do not build.
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
