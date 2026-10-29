#pragma once
#include "nwchem/nwchem.h"

namespace mf {
enum class AdvancedKind { Real, Integer, Choice, Flag, Path, Temperatures, GridAxis };
struct AdvancedOption {
    QString key, group, label, keyword, help, defaultText;
    AdvancedKind kind = AdvancedKind::Real;
    double minimum = 0, maximum = 1;
    QVector<NwchemChoice> choices;
    // The calculations the option takes part in, and the other advanced values it needs (condition)
    // with the calculations in words when they narrow them (scopeText).
    NwchemScope scope = NwchemScope::Always;
    bool (*condition)(const QJsonObject &advanced) = nullptr;
    QString scopeText;
    // Written as "keyword value" by nwchemAdvancedLines; otherwise by the input block that owns it.
    bool written = true;
};
const QVector<AdvancedOption> &nwchemAdvancedOptions();
QVector<NwchemChoice> nwchemAdvancedGroups();
QJsonObject nwchemAdvanced(const Document &document);
// Whether the current calculation uses the option: its scope and its condition.
bool nwchemAdvancedApplies(const AdvancedOption &option, const Document &document);
// The calculations an option is used in, in words ("DFTの計算").
QString nwchemAdvancedScope(const AdvancedOption &option);
// What NWChem uses while the option is blank, with where it comes from: "1e-7（SCF収束: 厳密）" for
// the thresholds that follow a setting of "2 計算条件", otherwise the option's defaultText.
QString nwchemAdvancedDefault(const AdvancedOption &option, const Document &document);
// The keyword of the input block that carries the option ("dft", "driver"; "scratch_dir" for a
// top-level directive), for showing it in the generated input.
QString nwchemAdvancedBlock(const AdvancedOption &option, const Document &document);
QString nwchemAdvancedError(const AdvancedOption &option, const QJsonValue &value);
QJsonValue parseNwchemAdvanced(const AdvancedOption &option, const QString &text);
QString nwchemAdvancedText(const QJsonValue &value);
void setNwchemAdvanced(Document &document, const QString &key, const QJsonValue &value);
void resetNwchemAdvanced(Document &document, const QString &group = {});
void validateNwchemAdvanced(const Document &document, NwchemValidation &result);
QString nwchemAdvancedNumber(const Document &document, const QString &key, const QString &fallback);
// Additional, validated directives for a module; replacement values are handled by nwchemInput.
NwchemGeneratedInput nwchemAdvancedLines(const Document &document, const QString &group);
QJsonObject nwchemAdvancedPreset(const QJsonObject &settings);
void applyNwchemAdvancedPreset(Document &document, const QJsonObject &settings);
} // namespace mf
