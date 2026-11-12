// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_validation.h"
#include "model/i18n.h"
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
void realRangeError(const NwchemSettingSpec &spec, ValidationReport &report) {
    // Preserve the stable codes and messages of the original checks. The bounds themselves always
    // come from the specifications, including for settings added in the future.
    struct Message {
        const char *key, *code, *text;
    };
    static const Message messages[] = {
        {"dielectric", "dielectric.range", QT_TR_NOOP("誘電率は1〜1000で指定してください。")},
        {"irc_stride", "irc.stride", QT_TR_NOOP("IRCの刻み幅は0.01〜1で指定してください。")},
        {"neb_spring", "neb.spring", QT_TR_NOOP("NEBのばね定数は0.001〜100で指定してください。")},
        {"neb_stepsize", "neb.stepsize", QT_TR_NOOP("NEBの刻み幅は0.001〜10で指定してください。")}};
    for (const auto &message : messages)
        if (spec.key == message.key) {
            report.error(message.code, spec.key, translated(message.text));
            return;
        }
    report.error("settings.real", spec.key,
                 tr("%1 は %2〜%3 の有限の数値で指定してください。")
                     .arg(spec.key)
                     .arg(spec.minimum)
                     .arg(spec.maximum));
}
} // namespace
void validateNwchemSettingValues(const Document &document, const QJsonObject &settings,
                                 ValidationReport &report) {
    if (document.data.contains("nwchem") && !document.data["nwchem"].isObject())
        report.error("settings.object", "nwchem", tr("NWChem設定はオブジェクトである必要があります。"));
    for (const auto &spec : nwchemSettingSpecs()) {
        const auto value = nwchemSettingValue(document, settings, spec.key);
        if (value.type() != spec.initial.type())
            report.error("settings.type", spec.key, tr("設定 %1 の型が不正です。").arg(spec.key));
        if (!spec.choices.isEmpty() &&
            std::none_of(spec.choices.begin(), spec.choices.end(),
                         [&](const auto &c) { return value.toString() == c.value; }))
            report.error("settings.choice", spec.key,
                         tr("設定 %1 の値「%2」は未対応です。").arg(spec.key, value.toString()));
        const bool integer = spec.kind == NwchemSettingKind::Integer;
        if ((!integer && spec.kind != NwchemSettingKind::Real) ||
            (!spec.validateUnused && !nwchemSettingApplies(spec, settings)))
            continue;
        const double n = value.toDouble();
        if (value.isDouble() && std::isfinite(n) && n >= spec.minimum && n <= spec.maximum &&
            (!integer || std::floor(n) == n))
            continue;
        if (integer)
            report.error("settings.integer", spec.key,
                         tr("%1 は %2〜%3 の整数で指定してください。")
                             .arg(spec.key)
                             .arg(spec.minimum)
                             .arg(spec.maximum));
        else
            realRangeError(spec, report);
    }
}
} // namespace mf
