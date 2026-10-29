#pragma once
#include "model/diagnostic.h"
#include "model/document.h"

namespace mf {
struct NwchemChoice {
    QString value, label;
};
struct NwchemPreset {
    QString name;
    QJsonObject settings;
};
enum class NwchemSettingKind { String, Choice, Integer, Real, Flag, Object, Array };
enum class NwchemSettingScope {
    Always,
    Dft,
    CustomXc,
    Correlation,
    Ccsd,
    Driver,
    Frequency,
    Irc,
    Scan,
    FollowupToggle,
    Followup,
    FollowupDft,
    Solvent,
    Dielectric,
    ExcitedToggle,
    Excited,
    Relativistic
};
struct NwchemSettingSpec {
    QString key;
    NwchemSettingKind kind;
    QJsonValue initial;
    double minimum = 0, maximum = 0;
    int decimals = 0;
    NwchemSettingScope scope = NwchemSettingScope::Always;
    bool job = false, allowDisable = false, validateUnused = true, preset = true;
    QVector<NwchemChoice> choices{}; // Source-language labels, translated only at the UI boundary.
};
const QVector<NwchemSettingSpec> &nwchemSettingSpecs();
const NwchemSettingSpec &nwchemSettingSpec(const QString &key);
bool nwchemSettingApplies(const NwchemSettingSpec &spec, const QJsonObject &settings);
bool nwchemSettingEditable(const NwchemSettingSpec &spec, const QJsonObject &settings);
// These edit only the named field; unknown fields and the project envelope are preserved.
void setNwchemSetting(Document &document, const QString &key, const QJsonValue &value);
struct NwchemValidation : ValidationReport {
    int electrons = 0, alpha = 0, beta = 0, ecpElectrons = 0, frozenOrbitals = 0;
    QString reference;
    bool numericalFrequencies = false;
};
QJsonObject nwchemDefaults();
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
// Tasks that run NWChem's DRIVER optimizer: minimizations, saddle searches and relaxed scans.
bool nwchemDriverTask(const QJsonObject &settings);
// Minimizations and saddle searches without fixed or ghost atoms run in NWChem's automatic internal
// coordinates (AUTOZ), which usually take fewer steps; NWChem falls back to Cartesian coordinates
// where it cannot build them (linear molecules). "driver_cartesian" (details) keeps Cartesian.
bool nwchemAutoz(const Document &document);
// "距離 1–2" / "角度 1–2–3" / "二面角 1–2–3–4" with the current 1-based atom numbers.
QString nwchemCoordinateLabel(const Document &document, const Ids &ids);
// The bonded molecules that act as counterpoise (BSSE) monomers, in atom order.
QVector<Ids> nwchemBsseMonomers(const Document &document);
// The SCF/DFT convergence threshold written for scf_convergence ("1e-7"); empty for "default",
// which leaves NWChem's own thresholds.
QString nwchemScfThreshold(const QJsonObject &settings);
// Why a section of "2 計算条件" takes no part in the current calculation ("最適化のみ"), or empty
// when it does. Sections: "path" (IRC・座標スキャン), "constraints", "optimization" (最適化・振動数),
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
