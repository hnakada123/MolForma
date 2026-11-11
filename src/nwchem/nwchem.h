// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/diagnostic.h"
#include "model/document.h"
#include <optional>

namespace mf {
struct NwchemChoice {
    QString value, label;
};
struct NwchemPreset {
    QString name;
    QJsonObject settings;
};
enum class NwchemMethod { Hf, Dft, Mp2, Ccsd, CcsdT, Unknown };
enum class NwchemTask {
    Energy,
    Gradient,
    Optimize,
    Frequencies,
    OptimizeFrequencies,
    Saddle,
    SaddleFrequencies,
    Irc,
    Scan,
    Neb,
    Unknown
};
// The kind of calculation that settings describe, read once from their "method" and "task"; values
// MolForma does not offer are Unknown and take part in nothing.
struct NwchemCalculation {
    explicit NwchemCalculation(const QJsonObject &settings);
    NwchemMethod method = NwchemMethod::Unknown;
    NwchemTask task = NwchemTask::Unknown;
    bool relaxedScan = false; // A scan that optimizes the other coordinates at each point.
    bool dft() const {
        return method == NwchemMethod::Dft;
    }
    // MP2, CCSD and CCSD(T), which correlate an SCF reference.
    bool correlated() const {
        return method == NwchemMethod::Mp2 || ccsd();
    }
    bool ccsd() const {
        return method == NwchemMethod::Ccsd || method == NwchemMethod::CcsdT;
    }
    bool saddle() const {
        return task == NwchemTask::Saddle || task == NwchemTask::SaddleFrequencies;
    }
    // Minimizations and saddle searches, alone or followed by frequencies.
    bool optimization() const {
        return task == NwchemTask::Optimize || task == NwchemTask::OptimizeFrequencies || saddle();
    }
    bool frequencies() const {
        return task == NwchemTask::Frequencies || task == NwchemTask::OptimizeFrequencies ||
               task == NwchemTask::SaddleFrequencies;
    }
    // Tasks that run NWChem's DRIVER optimizer: optimizations and relaxed scans.
    bool driver() const {
        return optimization() || (task == NwchemTask::Scan && relaxedScan);
    }
    // Tasks that compute a series of structures along a path: scans, IRC and NEB.
    bool path() const {
        return task == NwchemTask::Scan || task == NwchemTask::Irc || task == NwchemTask::Neb;
    }
};
// The calculations that a setting, a section of "2 計算条件" or an advanced option takes part in.
enum class NwchemScope {
    Always,
    Dft,
    ReferenceScf, // HF, MP2 and CCSD: an SCF, not DFT.
    CustomXc,
    Correlation,
    Mp2,
    Ccsd,
    Driver,      // Optimizations and relaxed scans.
    DriverOrIrc, // Also IRC, whose MEPGS takes DRIVER's iteration limit.
    Optimization,
    UnconstrainedOptimization, // Optimizations without internal-coordinate constraints.
    Frequency,
    NumericalHessian, // Frequencies from a numerical Hessian (needs the document).
    AnalyticHessian,  // Frequencies from an analytic Hessian (needs the document).
    Irc,
    Scan,
    Neb,
    Followup,
    FollowupDft,
    Solvent,
    Dielectric,
    Cosmo, // A solvent for HF or DFT.
    ExcitedToggle,
    Excited,
    ExcitedOptimization, // TDDFT optimizations of an excited state.
    Relativistic,
    SpinSpin, // NMR spin–spin couplings.
    Scan2     // The second coordinate of a two-dimensional scan.
};
bool nwchemScopeApplies(NwchemScope scope, const QJsonObject &settings, const Document *document = nullptr);
// The calculations of the scopes of advanced options in words ("DFTの計算"); empty for the others.
QString nwchemScopeText(NwchemScope scope);
enum class NwchemSettingKind { String, Choice, Integer, Real, Flag, Object, Array };
struct NwchemSettingSpec {
    QString key;
    NwchemSettingKind kind;
    QJsonValue initial;
    double minimum = 0, maximum = 0;
    int decimals = 0;
    NwchemScope scope = NwchemScope::Always;
    // Structural settings (charge, multiplicity) are fields of the Document, not of its NWChem settings.
    bool structural = false, allowDisable = false, validateUnused = true, preset = true;
    QVector<NwchemChoice> choices{}; // Source-language labels, translated only at the UI boundary.
};
const QVector<NwchemSettingSpec> &nwchemSettingSpecs();
const NwchemSettingSpec &nwchemSettingSpec(const QString &key);
bool nwchemSettingApplies(const NwchemSettingSpec &spec, const QJsonObject &settings);
bool nwchemSettingEditable(const NwchemSettingSpec &spec, const QJsonObject &settings);
// The value of a setting: from `settings` (nwchemSettings), or from the document for structural ones.
QJsonValue nwchemSettingValue(const Document &document, const QJsonObject &settings, const QString &key);
// These edit only the named field; unknown fields and the project envelope are preserved.
void setNwchemSetting(Document &document, const QString &key, const QJsonValue &value);
struct NwchemValidation : ValidationReport {
    int electrons = 0, alpha = 0, beta = 0, ecpElectrons = 0, frozenOrbitals = 0;
    QString reference;
    bool numericalFrequencies = false;
};
QJsonObject nwchemDefaults();
// Resolved settings of the last electronic calculation (including an optional follow-up).
QJsonObject nwchemFinalSettings(const Document &document);
QJsonObject nwchemSettings(const Document &document);
QVector<NwchemChoice> nwchemChoices(const QString &key);
QVector<NwchemPreset> nwchemPresets();
// Presets exclude identity, resources, units, charge, multiplicity and unknown fields.
QJsonObject nwchemPresetSettings(const QJsonObject &settings);
void applyNwchemPreset(Document &document, const QJsonObject &settings);
NwchemValidation validateNwchem(const Document &document);
bool nwchemNumericalFrequencies(const Document &document);
// Display names: the common basis with per-element bases, and the DFT functional ("xc ..." for a
// custom XC line).
QString nwchemBasisLabel(const QJsonObject &settings);
QString nwchemFunctionalLabel(const QJsonObject &settings);
// The XC line MolForma writes for a functional choice (empty for "custom" or unknown values).
QString nwchemFunctionalXc(const QString &value);
// Minimizations and saddle searches without fixed or ghost atoms run in NWChem's automatic internal
// coordinates (AUTOZ), which usually take fewer steps; NWChem falls back to Cartesian coordinates
// where it cannot build them (linear molecules). "driver_cartesian" (details) keeps Cartesian.
bool nwchemAutoz(const Document &document);
// "距離 1–2" / "角度 1–2–3" / "二面角 1–2–3–4" with the current 1-based atom numbers.
QString nwchemCoordinateLabel(const Document &document, const Ids &ids);
// Basis functions or ECPs pasted in NWChem's format (as the Basis Set Exchange writes it for NWChem):
// the elements they define, in their order, the core electrons of ECPs ("nelec") and the first problem
// found. Comments, "basis …"/"ecp" header lines and "end" are left out; shells must be "<element>
// <S|P|D|F|G|H|I|SP|L>" followed by lines of numbers (ECPs: "<element> nelec <n>", "<element> ul" or
// "<element> <S…I>" followed by lines of three numbers).
struct NwchemPastedBasis {
    QStringList elements;
    QMap<QString, int> cores;
    QMap<QString, QStringList> lines; // By element, the lines as they go into the input.
    QString error;
};
NwchemPastedBasis nwchemPastedBasis(const QString &text, bool ecp);
// The fragments of an initial guess from fragment orbitals ("fragment_guess"): atom indices in atom
// order, with the charge and multiplicity of each.
struct NwchemFragment {
    QVector<int> atoms;
    int charge = 0, multiplicity = 1;
};
QVector<NwchemFragment> nwchemFragments(const Document &document);
// The atom pairs (indices, first < second) whose NMR spin–spin couplings are asked for: of the real H, F
// and P atoms ("spinspin_pairs" hfp), or of all real atoms.
QVector<QPair<int, int>> nwchemSpinSpinPairs(const Document &document);
// The bonded molecules that act as counterpoise (BSSE) monomers, as atom indices in atom order.
QVector<QVector<int>> nwchemBsseMonomers(const Document &document);
// The SCF/DFT convergence threshold written for scf_convergence ("1e-7"); empty for "default",
// which leaves NWChem's own thresholds.
QString nwchemScfThreshold(const QJsonObject &settings);
// A calculation that continues another job's NWChem database (settings "restart"): "continue" runs
// the tasks that job did not finish, "hessian" searches a transition state with the Hessian of its
// frequency calculation, "reuse" derives frequencies and thermochemistry again from that Hessian
// (other temperatures or isotopes). The input then says "restart", has no geometry (NWChem keeps the
// one of the database) and, for "continue", lacks the tasks listed in "done" (their lines in the
// job's input). "geometry" is nwchemRestartDigest() of the document the restart was prepared for.
struct NwchemRestart {
    QString mode, job, geometry;
    QStringList done;
    bool active() const {
        return !mode.isEmpty();
    }
};
NwchemRestart nwchemRestart(const QJsonObject &settings);
// What a restart cannot change, as a digest: the atoms (elements, ghosts, positions, fixed flags and,
// except for "reuse", masses) and the internal-coordinate constraints.
QString nwchemRestartDigest(const Document &document, bool masses);
// The task lines of an NWChem input in their order ("task dft optimize").
QStringList nwchemTaskLines(const QString &input);
// The end of the path of an NEB calculation ("neb_end": atom positions in Å by atom ID): the document
// with every atom moved there, or nothing while the setting does not hold every atom (incomplete,
// with a reason, when it holds some).
struct NwchemNebEnd {
    std::optional<Document> document;
    QString problem; // Empty when complete; why not, otherwise.
};
NwchemNebEnd nwchemNebEnd(const Document &document);
// The "neb_end" setting for a structure with the same atoms in the same order (elements and ghosts as
// the document's): the positions by atom ID. With superpose, the structure is first fitted onto the
// document (Kabsch), so that the path holds no rotation or translation of the whole molecule.
QJsonObject nwchemNebEndSetting(const Document &document, const Document &end, bool superpose);
// The beads of NEB's first path (NWChem interpolates the start and the end linearly in Cartesian
// coordinates), with both ends.
QVector<Document> nwchemNebInitialPath(const Document &start, const Document &end, int beads);
// Why a section of "2 計算条件" takes no part in the current calculation ("最適化のみ"), or empty
// when it does. Sections: "path" (IRC・座標スキャン・NEB), "constraints", "optimization" (最適化・振動数),
// "followup", "bsse" and "excited" (TDDFT); the others always take part.
QString nwchemSectionUnused(const Document &document, const QString &section);
QString nwchemInput(const Document &document);
// A top-level directive or block of a generated input, and where it is set.
struct NwchemInputBlock {
    int line = 0, lines = 1; // First line (from 0) and the number of lines, up to its "end".
    QString keyword, title;  // "dft", and its header line as shown ("geometry（3 原子）").
    QList<int> tabs;         // The workspace tabs that set it, by number (1 分子 … 3 実行設定).
    QString setting, group;  // A setting key, and a group of the details of "2 計算条件", if any.
    bool operator==(const NwchemInputBlock &) const = default;
};
struct NwchemInputOutline {
    QVector<NwchemInputBlock> blocks;
    // Lines (from 0) that the details and the additional input of "2 計算条件" add or change.
    QSet<int> advanced, extra;
    QHash<QString, QSet<int>> settings; // Setting/advanced keys to their generated lines.
    bool operator==(const NwchemInputOutline &) const = default;
};
// Text and provenance are emitted together; line numbers always refer to this exact text.
struct NwchemGeneratedInput {
    QString text;
    NwchemInputOutline outline;
};
NwchemGeneratedInput generateNwchemInput(const Document &document);
} // namespace mf
