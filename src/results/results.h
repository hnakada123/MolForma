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
enum class ResultOutcome { NotApplicable, Incomplete, Normal, Error };
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
        bool energy = false, frequency = false, bsse = false;
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
    // The log ends: sections still open are incomplete. Completes the result.
    void finish();
    const NwchemEnergySummary &summary() const {
        return energySummary;
    }
    // The tasks of an input in their order, in words ("構造最適化", "振動数"), and the most steps
    // its DRIVER takes (maxiter; 0 without a DRIVER block).
    static QStringList tasks(const QString &input);
    static int driverSteps(const QString &input);
    int started = 0;   // Tasks begun; the current one is started - 1.
    int stepTask = -1; // The task of the steps, and the task whose optimization converged.
    int convergedTask = -1;
    QVector<ResultStep> steps;                                // Of the latest optimization.
    QJsonObject thresholds;                                   // {gmax, grms, xmax, xrms} of the DRIVER.
    double energy = std::numeric_limits<double>::quiet_NaN(); // The last total energy / Hartree,
    QString energyMethod;                                     // and its method ("SCF", "MP2").
    bool ended = false;                                       // NWChem's closing timing line.

  private:
    ResultData *result;
    NwchemEnergySummary energySummary;
    int lineNumber = 0;
    bool busy = false; // The current task has printed something since its input banner.
    bool recognized = false, fatal = false;
    QString abort, orbitalSpin, ircDirection, shieldingElement;
    QStringList recent; // The lines before this one, for the message that precedes an error.
    QVector<ResultOrbital> orbitals;
    QJsonArray thermochemistry, ircPoints, shielding;
    QJsonObject irc, solvation;
    QMap<int, QJsonObject> scan;
    int scanPoint = 0, shieldingAtom = 0, ircConverged = 0;
    // The sections being read: each takes the following lines until it returns false, and nullptr
    // once the log ends.
    QVector<std::function<bool(const QString *line)>> sections;
    void read(const QString &line);
    void progress(const QString &line);
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
// How an NWChem log ended: "normal" (the termination marker), "incomplete" (no marker: running,
// stopped or cut short) or "error"; empty for other files, whose outcome is not judged.
QString resultOutcome(const ResultData &data);
} // namespace mf
