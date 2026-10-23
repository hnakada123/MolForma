#pragma once
#include "results/results.h"
#include <QMap>

namespace mf {
struct MoldenPrimitive {
    double exponent, coefficient;
};
struct MoldenTerm {
    std::array<int, 3> powers;
    double coefficient;
};
struct MoldenShell {
    Vec center; // Bohr
    int angularMomentum = 0;
    QVector<MoldenPrimitive> primitives;
    QVector<QVector<MoldenTerm>> functions;
};
struct MoldenWavefunction {
    QVector<MoldenShell> shells;
    QVector<QVector<double>> coefficients;
    QString normalization;
    double value(int orbital, const Vec &angstrom) const;
};
// Standard MOLDEN ordering, including Cartesian and real spherical d/f/g functions.
MoldenWavefunction moldenWavefunction(const QMap<QString, QStringList> &blocks, const Document &atoms,
                                      const QVector<QVector<double>> &coefficients,
                                      const QString &normalization = "auto");
int frontierOrbital(const QVector<ResultOrbital> &orbitals, const QString &spin, bool lumo = false);
QVector<double> moldenOrbitalGrid(const MoldenWavefunction &wavefunction, int orbital, const Vec &origin,
                                  const Vec &step, const std::array<int, 3> &grid);
} // namespace mf
