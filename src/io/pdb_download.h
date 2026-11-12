// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "io/file_download.h"
namespace mf {
// Entries of the Protein Data Bank downloaded by their ID: the gzip-compressed mmCIF files of RCSB PDB. Only
// the ID goes out, to this address alone.
constexpr auto PdbSource = "https://files.rcsb.org/download/%1.cif.gz";
// The PDB ID in a text: four characters (7DMU, in upper case) or an extended ID (pdb_00007dmu, in lower
// case); empty when the text is no PDB ID.
QString pdbId(const QString &text);
class PdbDownload : public FileDownload {
    Q_OBJECT
  public:
    // The entry's file goes to folder as <ID>.cif.gz. source has %1 for the ID.
    PdbDownload(const QString &id, const QString &folder, const QString &source = PdbSource,
                QObject *parent = nullptr);
};
} // namespace mf
