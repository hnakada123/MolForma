// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem.h"
// Internal catalog shared by setting choices, validation and input generation.
namespace mf::nwchem_detail {
struct LibraryBasis {
    const char *name, *label;
    QVector<QPair<int, int>> elements;
    const char *ecp;
    bool relativistic = false; // All-electron, contracted for scalar-relativistic Hamiltonians.
};
struct Pasted {
    NwchemPastedBasis basis, ecp;
};
struct Functional {
    const char *value, *label, *xc, *extra;
    bool d3, d3bj, numericalHessian, metaGga = false;
};

const QVector<LibraryBasis> &libraryBases();
const LibraryBasis *libraryBasis(const QString &name);
int ecpCore(const QString &basis, int z);
Pasted pasted(const QJsonObject &settings);
QString elementBasis(const QJsonObject &settings, const QString &symbol);
int coreElectrons(const QJsonObject &settings, const Pasted &p, const QString &symbol, int z);
int atomicCoreOrbitals(int z);
bool basisCovers(const QString &basis, int z);
const QVector<Functional> &functionals();
const Functional *functional(const QString &value);
bool cartesianByConvention(const QString &basis);
bool fittingCovers(int z);
} // namespace mf::nwchem_detail
