#pragma once
#include "model/document.h"
namespace mf {
// xyzFrame selects one structure of a multi-frame XYZ file (0-based); a multi-frame file without
// it is rejected. NWChem (.nw) and Gaussian (.gjf/.com) inputs give their Cartesian structure,
// charge and multiplicity (.nw also the calculation settings, see importNwchemInput).
Document importStructure(const QString &path, int pdbModel = 1, const QString &altloc = {},
                         int xyzFrame = -1);
// The frames of an XYZ file (each as its own XYZ text) and their comment lines.
QStringList xyzFrames(const QString &text);
// Cartesian structure, charge and multiplicity of a Gaussian input (route and title are skipped).
Document gaussianStructure(const QString &text);
// Reads XYZ text and assigns bond orders from the geometry; XYZ itself carries none.
Document xyzStructure(const QString &text);
void exportStructure(const Document &d, const QString &path);
Document fromSmiles(const QString &smiles);
Document refine(const Document &d, bool regenerate = false, const Ids &moving = {});
struct Conformer {
    Document document;
    double energy = 0; // Force-field energy in kcal/mol.
};
// Rotor search with the MMFF94 (else UFF) force field: up to `count` distinct conformers, lowest
// energy first, each minimized. The atoms and their order stay; fixed atoms are not allowed.
QVector<Conformer> searchConformers(const Document &d, int count = 10);
QStringList pdbModels(const QString &text);
QString preparePdb(const QString &text, int model, const QString &altloc);
} // namespace mf
