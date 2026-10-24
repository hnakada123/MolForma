#pragma once
#include "model/document.h"
namespace mf {
Document importStructure(const QString &path, int pdbModel = 1, const QString &altloc = {});
// Reads XYZ text and assigns bond orders from the geometry; XYZ itself carries none.
Document xyzStructure(const QString &text);
void exportStructure(const Document &d, const QString &path);
Document fromSmiles(const QString &smiles);
Document refine(const Document &d, bool regenerate = false, const Ids &moving = {});
QStringList pdbModels(const QString &text);
QString preparePdb(const QString &text, int model, const QString &altloc);
} // namespace mf
