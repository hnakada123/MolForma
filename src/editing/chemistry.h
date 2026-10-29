#pragma once
#include "model/document.h"
namespace mf {
// Native Open Babel formats; routing of project/XYZ/NWChem/Gaussian files is in io/structure_io.h.
Document importBabelStructure(const QString &path, int pdbModel = 1, const QString &altloc = {});
void exportBabelStructure(const Document &document, const QString &path);
// Reads XYZ text and assigns bond orders from the geometry; XYZ itself carries none.
Document xyzStructure(const QString &text);
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
