#pragma once
#include "model/document.h"
#include <array>
#include <limits>

namespace mf {
struct SurfaceTriangle {
    Vec a, b, c;
    bool positive = true;
};
struct ResultFrame {
    Document molecule;
    QString label;
};
struct ResultEnergy {
    QString method;
    double hartree = 0;
    int line = 0;
};
struct ResultOrbital {
    QString symmetry, spin;
    double energy = 0, occupation = 0;
};
struct ResultMode {
    double frequency = 0, intensity = std::numeric_limits<double>::quiet_NaN();
    QVector<Vec> displacement;
};
// One TDDFT excited state. spin is "singlet" / "triplet", or empty for unrestricted states.
struct ResultExcitation {
    int root = 0;
    QString spin, symmetry;
    double ev = 0, oscillator = 0, s2 = std::numeric_limits<double>::quiet_NaN();
};
// One DRIVER optimization step ("@" lines): energy / Hartree, gradients / Hartree·Bohr⁻¹, steps / Bohr.
struct ResultStep {
    int step = 0;
    double energy = 0, deltaE = 0, gmax = 0, grms = 0, xrms = 0, xmax = 0, seconds = 0;
};
struct ResultData {
    QString source, format, status, log;
    QStringList warnings;
    QVector<ResultFrame> frames;
    QVector<ResultEnergy> energies;
    QVector<ResultOrbital> orbitals;
    QVector<ResultMode> modes;
    QVector<ResultExcitation> excitations;
    QVector<ResultStep> steps; // Steps of the last optimization in the log.
    // Log summaries: "thermochemistry" (one object per temperature), "dipole" {x, y, z, debye} in
    // a.u./Debye, "thresholds" {gmax, grms, xmax, xrms} of the DRIVER, "optimization" state.
    QJsonObject properties;
    Document vibrationGeometry;
    QVector<SurfaceTriangle> surface;
    QVector<int> datasets;
    std::array<int, 3> grid{};
    int dataset = 0;
    double isovalue = .03, minimum = 0, maximum = 0;
    bool orbitalBasis = false;
    int selectedOrbital = -1, orbitalFrame = -1, orbitalDivisions = 40;
    QString orbitalError, orbitalNormalization, normalizationRequest = "auto";
};
// Parsing/meshing is bounded and runs in a cancellable native process in the UI.
ResultData readResult(const QString &path, double isovalue = .03, int dataset = 0, int orbital = -1,
                      const QString &normalization = "auto", int orbitalDivisions = 40);
void saveResultData(const QString &path, const ResultData &data);
ResultData loadResultData(const QString &path);
int resultWorker(const QString &request, const QString &response);
QStringList resultFiles(const QString &directory);
Document resultModeGeometry(const ResultData &data, int mode, double phase, double amplitude);
// Translations and rotations among the normal modes: the 6 (linear: 5, atom: 3) modes with the
// smallest |frequency| of a complete 3N-mode set. Other mode sets are all treated as vibrations.
QVector<bool> resultRigidModes(const ResultData &data);
} // namespace mf
