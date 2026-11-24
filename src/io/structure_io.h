// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "io/crystal.h"
#include "io/mmcif.h"
#include "io/pdb.h"
#include "model/document.h"
#include <memory>

namespace mf {
// A project replaces all fields. Other formats carry a structure; NWChem/Gaussian also carry
// charge and multiplicity, and NWChem calculation settings. PDB and mmCIF files read the selection; a
// multi-frame XYZ requires xyzFrame; a small-molecule CIF (fractional coordinates) reads the crystal's
// molecules as chosen.
Document importStructure(const QString &path, const MacromoleculeSelection &selection = {}, int xyzFrame = -1,
                         const CrystalSelection &crystal = {});
// What to read of a structure file, as the dialogs of ui/structure_import.h choose it.
struct StructureImportOptions {
    MacromoleculeSelection selection; // PDB, mmCIF
    int frame = -1;                   // Multi-frame XYZ
    CrystalSelection crystal;         // Small-molecule CIF
    // The PDBx/mmCIF entry as read for the choice, which the reading takes instead of the file: reading
    // a virus capsid of millions of atoms takes seconds.
    std::shared_ptr<const MmcifEntry> mmcif;
};
Document importStructure(const QString &path, const StructureImportOptions &options);
// The same reading with what the file holds beside the structure that was not imported: for an NWChem
// input (.nw, .nwinp) the directives and options MolForma does not convert, for PDB and mmCIF files the
// atoms of unknown elements (such as UNX, an unknown atom or ion) left out; nothing for other formats.
struct StructureImport {
    Document document;
    QStringList notes;
};
StructureImport importStructureWithNotes(const QString &path, const StructureImportOptions &options = {});
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
