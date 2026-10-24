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
// "距離 1–2" / "角度 1–2–3" / "二面角 1–2–3–4" with the current 1-based atom numbers.
QString nwchemCoordinateLabel(const Document &document, const Ids &ids);
// The bonded molecules that act as counterpoise (BSSE) monomers, in atom order.
QVector<Ids> nwchemBsseMonomers(const Document &document);
QString nwchemInput(const Document &document);
} // namespace mf
