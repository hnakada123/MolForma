#pragma once
#include "nwchem.h"

namespace mf {
enum class AdvancedKind { Real, Integer, Choice, Flag, Path, Temperatures, GridAxis };
struct AdvancedOption {
    QString key, group, label, keyword, help, defaultText;
    AdvancedKind kind = AdvancedKind::Real;
    double minimum = 0, maximum = 1;
    QVector<NwchemChoice> choices;
};
const QVector<AdvancedOption> &nwchemAdvancedOptions();
QVector<NwchemChoice> nwchemAdvancedGroups();
QJsonObject nwchemAdvanced(const Document &document);
bool nwchemAdvancedApplies(const AdvancedOption &option, const Document &document);
QString nwchemAdvancedError(const AdvancedOption &option, const QJsonValue &value);
QJsonValue parseNwchemAdvanced(const AdvancedOption &option, const QString &text);
QString nwchemAdvancedText(const QJsonValue &value);
void setNwchemAdvanced(Document &document, const QString &key, const QJsonValue &value);
void resetNwchemAdvanced(Document &document, const QString &group = {});
void validateNwchemAdvanced(const Document &document, NwchemValidation &result);
QString nwchemAdvancedNumber(const Document &document, const QString &key, const QString &fallback);
// Additional, validated directives for a module; replacement values are handled by nwchemInput.
QString nwchemAdvancedLines(const Document &document, const QString &group);
QJsonObject nwchemAdvancedPreset(const QJsonObject &settings);
void applyNwchemAdvancedPreset(Document &document, const QJsonObject &settings);
} // namespace mf
