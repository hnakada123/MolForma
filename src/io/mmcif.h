// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "io/macromolecule.h"
#include "model/document.h"

namespace mf {
// An atom of the _atom_site table of a PDBx/mmCIF file. Residues and chains take the author's names (auth_*),
// which the PDB file of the same entry uses, and the PDBx names (label_*) where those are missing.
struct MmcifAtom {
    QString element, name, residue, chain, insertion, altloc, entity;
    int number = 0, model = 1, charge = 0;
    double occupancy = 1;
    Vec position = Vec::Zero();
    bool hetatm = false;
    // From the type of the entity (_entity.type), else from the record (group_PDB) and water names.
    ResidueKind kind = ResidueKind::Polymer;
};
struct MmcifEntry {
    QVector<MmcifAtom> atoms;             // The _atom_site table, in file order.
    QHash<QString, QString> descriptions; // The entities' names (_entity.pdbx_description) by ID.
};
// The first data block.
MmcifEntry readMmcif(const QByteArray &text);
MacromoleculeContents mmcifContents(const MmcifEntry &entry);
// The selected atoms: one model at one alternate location, of the chosen chains and kinds.
QVector<MmcifAtom> selectMmcif(const QVector<MmcifAtom> &atoms, const MacromoleculeSelection &selection);

// The first data block of any CIF file: its items and tables by their tags in lower case. Values keep their
// text; the unquoted . and ? are empty.
struct CifBlock {
    struct Table {
        QList<QByteArray> tags;
        QVector<QStringList> rows;
    };
    QString name; // After data_.
    QHash<QByteArray, QString> items;
    QVector<Table> tables;
    // The values of a tag: its column, or its single item; empty when the block has neither.
    QStringList column(const QByteArray &tag) const;
    // The table with that tag, or nullptr.
    const Table *table(const QByteArray &tag) const;
};
CifBlock readCifBlock(const QByteArray &text);
// Whether a CIF file is a small-molecule crystal structure (_atom_site_fract_x) rather than a PDBx/mmCIF
// entry (_atom_site.Cartn_x).
bool crystalCif(const QByteArray &text);
} // namespace mf
