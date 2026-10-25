#pragma once
#include "model/document.h"

namespace mf {
struct NwchemChoice {
    QString value, label;
};
struct NwchemPreset {
    QString name;
    QJsonObject settings;
};
struct NwchemValidation {
    QStringList errors, warnings;
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
};
struct NwchemInputOutline {
    QVector<NwchemInputBlock> blocks;
    // Lines (from 0) that the details and the additional input of "2 計算条件" add or change.
    QSet<int> advanced, extra;
};
// The outline of an input generated for the document (nwchemInput, or the input of an execution
// plan). A line comes from the details (or the additional input) when the input generated without
// those settings lacks it; blocks take their tabs from their keyword and from such lines.
NwchemInputOutline nwchemInputOutline(const Document &document, const QString &input);
} // namespace mf
