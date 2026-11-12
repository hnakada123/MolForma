// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "io/crystal.h"
#include "io/pdb.h"
#include "model/document.h"

namespace mf {
// A project replaces all fields. Other formats carry a structure; NWChem/Gaussian also carry
// charge and multiplicity, and NWChem calculation settings. PDB and mmCIF files read the selection; a
// multi-frame XYZ requires xyzFrame; a small-molecule CIF (fractional coordinates) reads the crystal's
// molecules as chosen.
Document importStructure(const QString &path, const MacromoleculeSelection &selection = {}, int xyzFrame = -1,
                         const CrystalSelection &crystal = {});
// The same reading with what the file holds beside the structure that was not imported: for an NWChem
// input (.nw, .nwinp) the directives and options MolForma does not convert; nothing for other formats.
struct StructureImport {
    Document document;
    QStringList notes;
};
StructureImport importStructureWithNotes(const QString &path, const MacromoleculeSelection &selection = {},
                                         int xyzFrame = -1, const CrystalSelection &crystal = {});
// The end of an NEB path read from a file: the last structure of an NWChem log (.out, .log, .nwo), the last
// frame of an XYZ file with several, or the structure of another structure file. The file must be a
// regular file of at most MaximumNebEndFileBytes.
inline constexpr qint64 MaximumNebEndFileBytes = 128 * 1024 * 1024;
// Checks the file and tells whether it is a log. A log goes through the result readers: nebEndFromFile
// reads it at once; a caller that must stay responsive reads it with a cancellable ResultLoader and gives
// the result to nebEndFromLog.
bool nebEndFileIsLog(const QString &path);
struct ResultData;
Document nebEndFromLog(const ResultData &log);
Document nebEndFromFile(const QString &path);
void exportStructure(const Document &document, const QString &path);
QStringList xyzFrames(const QString &text);
Document gaussianStructure(const QString &text);
} // namespace mf
