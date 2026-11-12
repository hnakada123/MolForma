// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <array>
#include <functional>
#include <limits>

namespace mf {
struct SurfaceTriangle {
    Vec a, b, c;
    bool positive = true;
    // Unit normals at a, b and c from the gradient of the field, pointing out of the lobe, for
    // smooth shading; zero when unknown (the face normal is used then).
    Vec na = Vec::Zero(), nb = Vec::Zero(), nc = Vec::Zero();
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
    // The molecular orbital's number within its spin, counted from 1 as NWChem and MOLDEN do (a log
    // prints only some orbitals, from "Vector N").
    int number = 0;
};
struct ResultMode {
    double frequency = 0, intensity = std::numeric_limits<double>::quiet_NaN();
    QVector<Vec> displacement;
};
// One amplitude of an excited state, as NWChem prints those above 0.05: from an occupied orbital to a
// virtual one (their numbers within the spin, counted from 1 like ResultOrbital::number; a frozen core
// does not shift them). spin is "alpha" / "beta", or empty for restricted states. An X amplitude
// (excitation), or a Y one (deexcitation); Tamm–Dancoff states have X only.
struct ResultContribution {
    int from = 0, to = 0;
    QString spin;
    double coefficient = 0;
    bool deexcitation = false;
};
// One TDDFT excited state. spin is "singlet" / "triplet", or empty for unrestricted states.
struct ResultExcitation {
    static constexpr int maxContributions = 50;
    int root = 0;
    QString spin, symmetry;
    double ev = 0, oscillator = 0, s2 = std::numeric_limits<double>::quiet_NaN();
    // The largest amplitudes (at most maxContributions) in NWChem's order; none when the log ends
    // within them.
    QVector<ResultContribution> contributions = {};
};
// One DRIVER optimization step ("@" lines): energy / Hartree, gradients / Hartree·Bohr⁻¹, steps / Bohr.
struct ResultStep {
    int step = 0;
    double energy = 0, deltaE = 0, gmax = 0, grms = 0, xrms = 0, xmax = 0, seconds = 0;
    bool operator==(const ResultStep &) const = default;
};
// The last DRIVER optimization as one unit. Later energy/property/frequency tasks keep it;
// starting another optimization replaces its steps, thresholds and convergence together.
struct NwchemOptimization {
    int task = -1;
    bool present = false, converged = false;
    QVector<ResultStep> steps;
    QJsonObject thresholds;
};
enum class ResultOutcome { NotApplicable, Incomplete, Normal, Error };
// A bond index of this size or more (in magnitude) is listed as a bond; the reader keeps at most
// maxBondIndices atom pairs of a log.
inline constexpr double NotableBondIndex = .05;
inline constexpr int maxBondIndices = 20000;
struct ResultData {
    ResultOutcome outcome = ResultOutcome::NotApplicable;
    QString source, format, status, log;
    QStringList warnings;
    QVector<ResultFrame> frames;
    QVector<ResultEnergy> energies;
    QVector<ResultOrbital> orbitals;
    QVector<ResultMode> modes;
    QVector<ResultExcitation> excitations;
    QVector<ResultStep> steps; // Steps of the last optimization in the log.
    // Log summaries: "neb" {points [{bead, energy, frame, distance}] of the last path read (distance:
    // the length of the path up to the bead, Å), iteration, steps of "@neb", thresholds, converged},
    // "thermochemistry" (one object per temperature), "dipole" {x, y, z, debye} in
    // a.u./Debye, "thresholds" {gmax, grms, xmax, xrms} of the DRIVER, "optimization" state. Of the
    // Mulliken analysis: "mulliken" (charges), "valency" and "free_electrons" per atom, and
    // "bond_indices" [{a, b, value}] with atom numbers a < b from 1: every pair, or with
    // "bond_indices_omitted" (their number) without the pairs below NotableBondIndex of a molecule
    // with more than maxBondIndices pairs. "tddft" {alpha_electrons, beta_electrons} of the excited
    // states' reference. "spinspin" [{a, b, isotope_a, isotope_b, hz}]: the isotropic NMR spin–spin
    // couplings (atom numbers from 1, isotopes as "13-C"). "scan" points name a second coordinate
    // ("coordinate2", "value2") in a two-dimensional scan.
    QJsonObject properties;
    Document vibrationGeometry;
    QVector<SurfaceTriangle> surface;
    QVector<int> datasets;
    std::array<int, 3> grid{};
    QVector<double> values; // Selected CUBE dataset; bounded to two million values.
    Vec gridOrigin = Vec::Zero();
    Mat gridAxes = Mat::Identity();
    int dataset = 0;
    double isovalue = .03, minimum = 0, maximum = 0;
    bool orbitalBasis = false;
    int selectedOrbital = -1, orbitalFrame = -1, orbitalDivisions = 40;
    QString orbitalError, orbitalNormalization, normalizationRequest = "auto";
};
// The energy of a calculation, fed an NWChem log line by line: that of the last task with one; for
// a frequency task the reference structure's (numerical Hessians print the energies of displaced
// structures after it), for BSSE the corrected energy. NWChem reads the input again ("NWChem
// Input Module") before each task.
class NwchemEnergySummary {
  public:
    // Lines add() may use; others can be skipped without decoding them.
    static bool relevant(QByteArrayView line);
    void add(const QString &line);
    // {"hartree", "method", "source": final|frequency_reference|bsse_corrected}; empty without one.
    QJsonObject energy() const;
    // The last counterpoise correction: {"error", "supermolecular", "corrected"} summed over all the
    // monomers, with "nwchem_error" when NWChem printed another error and, when the monomer energies
    // were found, "interaction" and "interaction_uncorrected" (Hartree); empty without one.
    QJsonObject bsse() const;
    bool normalTermination() const {
        return ended;
    }

  private:
    struct Task {
        double first = 0, last = 0, corrected = 0, supermolecular = 0, error = 0, nwchemError = 0,
               monomerSum = qQNaN(); // The monomers in their own bases.
        QString firstMethod, lastMethod;
        bool energy = false, frequency = false, bsse = false, raman = false;
        // The energies since the monomers were listed (or since the last correction), for the sum.
        int monomers = 0;
        QVector<QPair<QString, double>> since;
    };
    QVector<Task> tasks{Task{}};
    bool ended = false;
};
// One reading of an NWChem log, fed its lines in order: the whole log for the result viewer, a growing
// log for the run panel, the end of an old one for the history. Sections that span several lines
// (coordinate and normal-mode tables, excited states, charges, thermochemistry, ...) are read as their
// lines arrive.
class NwchemLog {
  public:
    // With a result, everything goes into it and finish() completes it; without, only the progress
    // below and the energy summary are read.
    explicit NwchemLog(ResultData *result = nullptr);
    void add(const QString &line);
    // A line as read from the file (UTF-8). Without a result, the lines that change neither the
    // progress nor the energy summary are passed over without decoding them.
    void addBytes(QByteArrayView line);
    // The log ends: sections still open are incomplete. Completes the result.
    void finish();
    const NwchemEnergySummary &summary() const {
        return energySummary;
    }
    // The tasks of an input in their order, in words ("構造最適化", "振動数"), and the most steps
    // its DRIVER takes (maxiter; 0 without a DRIVER block).
    static QStringList tasks(const QString &input);
    static int driverSteps(const QString &input);
    int started = 0; // Tasks begun; the current one is started - 1.
    const NwchemOptimization &optimization() const {
        return driver;
    }
    double energy = std::numeric_limits<double>::quiet_NaN(); // The last total energy / Hartree,
    QString energyMethod;                                     // and its method ("SCF", "MP2").
    bool ended = false;                                       // NWChem's closing timing line.

  private:
    ResultData *result;
    NwchemEnergySummary energySummary;
    NwchemOptimization driver;
    int lineNumber = 0;
    bool busy = false; // The current task has printed something since its input banner.
    bool recognized = false, fatal = false;
    QString abort, orbitalSpin, ircDirection, shieldingElement;
    QString responseGauge;
    double responseFrequency = 0;
    int alphaElectrons = -1, betaElectrons = -1; // As last printed (-1: not yet).
    int bondAtoms = 0;  // The atoms of the bond-index matrix just read, for the valencies after it.
    QStringList recent; // The lines before this one, for the message that precedes an error.
    QVector<ResultOrbital> orbitals;
    QJsonArray thermochemistry, ircPoints, shielding, spinspin;
    QStringList spinPairAtoms; // The atoms and isotopes of the spin–spin coupling being read.
    QJsonObject irc, solvation;
    QMap<int, QJsonObject> scan;
    int scanPoint = 0, shieldingAtom = 0, ircConverged = 0;
    // NEB: the step being computed; the path energies of a step, and the path structures and their
    // energies of a step (printed with print_shift); the "@neb" steps, the thresholds and whether
    // NWChem said the path converged (-1: not said).
    int nebIteration = 0, nebEnergiesIteration = -1, nebPathIteration = -1, nebConverged = -1;
    QVector<double> nebEnergies, nebPathEnergies;
    QVector<Document> nebPath;
    QJsonArray nebSteps;
    QJsonObject nebThresholds;
    // The sections being read: each takes the following lines until it returns false, and nullptr
    // once the log ends.
    QVector<std::function<bool(const QString *line)>> sections;
    void read(const QString &line);
    void readCoordinates(const QString &line);
    void readModes(const QString &line);
    void readExcitations(const QString &line);
    void progress(const QString &line);
    void beginOptimization();
};
// Parsing/meshing is bounded and runs in a cancellable native process in the UI. With a cache folder
// (the result loader's, for its session), a CUBE or MOLDEN file read before is not parsed again for
// another dataset, orbital or isovalue, nor an orbital evaluated again for another isovalue.
ResultData readResult(const QString &path, double isovalue = .03, int dataset = 0, int orbital = -1,
                      const QString &normalization = "auto", int orbitalDivisions = 40,
                      const QString &cacheFolder = {});
// A term of a combination of CUBE files: factor × a dataset of a CUBE file.
struct CubeTerm {
    QString path;
    double factor = 1;
    int dataset = 0;
};
// Writes the sum of the terms to output, a Gaussian CUBE with one dataset: a difference density
// ρ(AB) − ρ(A) − ρ(B) is +1 × AB, −1 × A, −1 × B. The files must share their grid (points, origin and
// spacing); the atoms written are those of the first. Fails otherwise, or when output is one of them.
void combineCubes(const QVector<CubeTerm> &terms, const QString &output);
void saveResultData(const QString &path, const ResultData &data);
ResultData loadResultData(const QString &path);
int resultWorker(const QString &request, const QString &response);
// The process of a result loader's worker: with the request and response paths as arguments
// (--result-worker <request> <response>), or started ahead of its request (--result-worker-ready) and
// given them as a JSON line on standard input; the end of input ends it. Its exit code, or -1 when
// argv starts neither (prepare runs once the application exists, e.g. for the translations).
int resultWorkerMain(int argc, char **argv, void (*prepare)() = nullptr);
QStringList resultFiles(const QString &directory);
Document resultModeGeometry(const ResultData &data, int mode, double phase, double amplitude);
// Translations and rotations among the normal modes: the 6 (linear: 5, atom: 3) modes with the
// smallest |frequency| of a complete 3N-mode set. Other mode sets are all treated as vibrations.
QVector<bool> resultRigidModes(const ResultData &data);
// How an NWChem log ended: "normal" (the termination marker), "incomplete" (no marker: running,
// stopped or cut short) or "error"; empty for other files, whose outcome is not judged.
QString resultOutcome(const ResultData &data);
} // namespace mf
