// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <array>

namespace mf {
// What a PDB or PDBx/mmCIF entry holds, and the part of it to read: the same for both formats, whose readers
// (io/pdb.h, io/mmcif.h) name the atoms of their files to the classes below.

// The residues of an entry: polymer chains (proteins and nucleic acids, with their modified residues), the
// other hetero groups (ligands, ions, sugars) and water.
enum class ResidueKind { Polymer, Ligand, Water };
// The most atoms read of an entry. MolForma edits a structure and cuts the part of it that NWChem calculates;
// its views and panels follow every edit of the whole document, which a virus capsid of millions of atoms
// would keep busy for minutes.
constexpr int MacromoleculeAtomLimit = 100000;
// Fails, naming the limit, for a selection of more atoms than MacromoleculeAtomLimit.
void checkMacromoleculeAtoms(qsizetype atoms);
struct MacromoleculeChain {
    QString id, description;    // The molecule's name, where the file gives it (COMPND, _entity).
    int residues = 0;           // Of the polymer.
    QStringList groups;         // The names of the other residues (ligands, water), once each.
    std::array<int, 3> atoms{}; // Of each ResidueKind, at one alternate location of each residue.
};
struct MacromoleculeSelection {
    int model = 1;
    QString altloc;     // Empty: in each residue, the location with the highest total occupancy.
    QStringList chains; // Empty: every chain.
    bool polymer = true, ligands = true, water = true;
    bool keeps(const QString &chain, ResidueKind kind) const;
};
struct MacromoleculeContents {
    QStringList models;
    bool altlocs = false;
    QVector<MacromoleculeChain> chains;
    int polymers = 0, ligands = 0, waters = 0; // Residues of each kind, in the first model.
    // The atoms of the first model that a selection keeps, its model aside: an estimate where the locations
    // of a residue differ in their atoms.
    int atoms(const MacromoleculeSelection &selection = {}) const;
};
// The chains, residues and atoms of an entry, counted from the atoms of its first model.
class MacromoleculeCount {
  public:
    explicit MacromoleculeCount(MacromoleculeContents &contents) : contents(contents) {}
    // An atom: its residue (a text that tells the residues of the entry apart), the residue's chain, name
    // and kind, its alternate location, and the name of the chain's molecule where the atom's record leads to
    // one.
    void add(const QString &residue, const QString &chain, const QString &code, ResidueKind kind,
             const QString &altloc, const QString &description = {});

  private:
    MacromoleculeContents &contents;
    QSet<QString> residues;
    QHash<QString, int> chains;
    QHash<QString, QString> locations; // The first alternate location of each residue that has them.
    QString last;                      // The residue of the atom before, in the chain numbered current.
    int current = -1;
};
// Which alternate location of each residue is read: the one chosen for the entry, or in each residue the
// one whose atoms have the highest total occupancy (the first of equals, by name).
class AltlocChoice {
  public:
    explicit AltlocChoice(const QString &chosen) : chosen(chosen) {}
    // An atom that may be read; one without an alternate location says nothing.
    void add(const QString &residue, const QString &altloc, double occupancy);
    // Whether an atom is read: any without an alternate location, the others at their residue's location.
    bool keeps(const QString &residue, const QString &altloc) const;

  private:
    QString chosen;
    QHash<QString, QMap<QString, double>> occupancies;
};
} // namespace mf
