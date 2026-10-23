#pragma once
#include "document.h"
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
struct ResultData {
    QString source, format, status, log;
    QStringList warnings;
    QVector<ResultFrame> frames;
    QVector<ResultEnergy> energies;
    QVector<ResultOrbital> orbitals;
    QVector<ResultMode> modes;
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
} // namespace mf
