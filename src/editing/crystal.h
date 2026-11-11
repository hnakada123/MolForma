// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"

namespace mf {
// A small-molecule crystal structure of a CIF file: the cell, the symmetry operations and the atoms of the
// asymmetric unit in fractional coordinates (X-ray and neutron structures of the CSD, COD and journals).
struct CrystalAtom {
    QString label, element;
    Vec fraction = Vec::Zero();
    double occupancy = 1;
    int group = 0; // The disorder group (_atom_site_disorder_group); 0 when the atom is ordered.
};
struct Crystal {
    QString name, spaceGroup, formula;
    double a = 0, b = 0, c = 0, alpha = 90, beta = 90, gamma = 90; // Å and degrees.
    QStringList operations;                                        // As the file gives them ("-x,y+1/2,-z").
    QVector<CrystalAtom> atoms;
    // Fractional to Cartesian coordinates (Å): a along x, b in the xy plane.
    Mat matrix() const;
};
Crystal readCrystal(const QByteArray &text);
// A symmetry operation ("-x+1/2, y, -z") as rotation and translation of fractional coordinates.
std::pair<Mat, Vec> crystalOperation(const QString &text);
// What a structure offers to read: the molecules of a formula unit (those that complete the asymmetric
// unit, as many as a molecule on a special position needs), those of the unit cell, and whether there is
// disorder or an endless (polymeric) network.
struct CrystalContents {
    int asymmetricAtoms = 0, molecules = 0, cellMolecules = 0, cellAtoms = 0;
    bool disorder = false, polymeric = false;
};
struct CrystalSelection {
    bool cell = false;  // Every molecule of the unit cell, not only those of the asymmetric unit.
    bool minor = false; // Also the atoms of the minor disorder groups (2, 3, …), with those of group 1.
};
CrystalContents crystalContents(const Crystal &crystal, const CrystalSelection &selection = {});
// The molecules as Cartesian coordinates (Å) with bonds by distance, as an XYZ file is read: each molecule
// complete, across the cell's edges. By default a formula unit: the molecules that hold the asymmetric unit
// (with the counterions and solvent), each as many times as a molecule on a special position asks (two
// cations for an anion on an inversion center), placed next to one another.
Document crystalStructure(const Crystal &crystal, const CrystalSelection &selection = {});
} // namespace mf
