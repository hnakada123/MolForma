// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "io/structure_io.h"
#include <functional>
#include <optional>
class QWidget;
namespace mf {
// Runs work (the reading of a file) on a thread of the pool, so that the window is drawn and the reading can
// be stopped: work done within a moment is waited for as before; longer work shows a dialog
// (structureReadingDialog) whose 中止 returns false at once, the work going on unseen to its end and its
// result unused, so that it keeps what it reads to itself. The work's exceptions are thrown here.
bool readInBackground(QWidget *parent, const QString &path, std::function<void()> work);
// Shared choices for opening, replacing and appending a structure. Cancellation is not an error;
// callers retain their document and check its freshness after these modal dialogs return.
// Without ask, the choices the dialogs propose: the first model, the alternate location of the highest
// occupancy, everything, the last frame. PDB, mmCIF and XYZ files are read for the choice in the background
// (readInBackground): the mmCIF of a virus capsid, millions of atoms, takes seconds.
std::optional<StructureImportOptions> chooseStructureImport(QWidget *parent, const QString &path,
                                                            bool ask = true);
// The structure as chosen, read with the wait cursor: the largest entries of the PDB take seconds.
StructureImport importChosenStructure(const QString &path, const StructureImportOptions &options);
// What a file held that its reading left out (StructureImport::notes), in a message box if there is any: the
// atoms of unknown elements of a PDB or mmCIF entry, and with calculation (where the reading takes over an
// NWChem input's calculation settings, as opening it does) the parts of the input that it did not take over.
void showImportNotes(QWidget *parent, const QString &path, const QStringList &notes,
                     bool calculation = false);
} // namespace mf
