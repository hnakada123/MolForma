// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/pdb.h"
#include <optional>
class QWidget;
namespace mf {
// Shared choices for opening, replacing and appending a structure. Cancellation is not an error;
// callers retain their document and check its freshness after these modal dialogs return.
struct StructureImportOptions {
    MacromoleculeSelection selection; // PDB, mmCIF
    int frame = -1;                   // Multi-frame XYZ
};
// Without ask, the choices the dialogs propose: the first model, the alternate location of the highest
// occupancy, everything, the last frame.
std::optional<StructureImportOptions> chooseStructureImport(QWidget *parent, const QString &path,
                                                            bool ask = true);
} // namespace mf
