// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/execution_environment.h"
#include "foundation/i18n.h"
#include "model/settings.h"
#include "nwchem/nwchem_execution.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace mf {
namespace {
// Each part as a store holds it, so that the loader reads all of them from one.
QJsonObject storedPreferences(const QSettings &store) {
    const auto stored =
        QJsonDocument::fromJson(store.value(Preference::executionDefaults).toByteArray()).object();
    const auto defaults = executionDefaults();
    QJsonObject result;
    for (const auto &key : executionPreferenceKeys())
        result[key] = stored[key].type() == defaults[key].type() ? stored[key] : defaults[key];
    return result;
}
QVector<RemoteHost> storedRemoteHosts(const QSettings &store) {
    const auto stored = QJsonDocument::fromJson(store.value(Preference::remoteHosts).toByteArray()).array();
    QVector<RemoteHost> hosts;
    for (const auto &value : stored)
        if (value.isObject())
            hosts << RemoteHost::fromJson(value.toObject());
    return hosts;
}
// A saved template is used only while it is a header: lines that could run as commands never are.
QString usableJobHeader(const QString &scheduler, const QString &stored) {
    return stored.trimmed().isEmpty() || !jobHeaderErrors(stored).isEmpty() ? defaultJobHeader(scheduler)
                                                                            : stored;
}
} // namespace
QJsonObject executionPreferences() {
    return storedPreferences(userPreferences());
}
void saveExecutionPreferences(const QJsonObject &settings) {
    QJsonObject stored;
    for (const auto &key : executionPreferenceKeys())
        if (settings.contains(key))
            stored[key] = settings[key];
    userPreferences().setValue(Preference::executionDefaults,
                               QJsonDocument(stored).toJson(QJsonDocument::Compact));
}
void applyExecutionPreferences(Document &d) {
    const auto defaults = executionDefaults(), preferences = executionPreferences();
    for (auto it = preferences.begin(); it != preferences.end(); ++it)
        if (it.value() != defaults[it.key()])
            setExecutionSetting(d, it.key(), it.value());
}
QString executionJobPrelude() {
    return userPreferences().value(Preference::jobPrelude).toString();
}
void saveExecutionJobPrelude(const QString &lines) {
    if (lines.size() > 4000 || lines.contains(QRegularExpression("[\\x00-\\x08\\x0b-\\x1f\\x7f]")))
        fail(tr("ジョブスクリプトの準備の行は、制御文字を含めず4000文字以内で指定してください。"));
    userPreferences().setValue(Preference::jobPrelude, lines.trimmed());
}
QVector<RemoteHost> remoteHosts() {
    return storedRemoteHosts(userPreferences());
}
void saveRemoteHosts(const QVector<RemoteHost> &hosts) {
    QJsonArray stored;
    QSet<QString> names;
    for (const auto &h : hosts) {
        const auto errors = remoteHostErrors(h);
        if (!errors.isEmpty())
            fail(tr("実行先「%1」: %2").arg(h.name, errors.join('\n')));
        if (names.contains(h.name))
            fail(tr("実行先の名前が重複しています: %1").arg(h.name));
        names.insert(h.name);
        stored.append(h.json());
    }
    userPreferences().setValue(Preference::remoteHosts, QJsonDocument(stored).toJson(QJsonDocument::Compact));
}
std::optional<RemoteHost> findRemoteHost(const QString &name) {
    if (name.isEmpty())
        return std::nullopt;
    for (const auto &h : remoteHosts())
        if (h.name == name)
            return h;
    return std::nullopt;
}
QString jobHeaderTemplate(const QString &scheduler) {
    return usableJobHeader(scheduler, userPreferences().value(Preference::jobHeader(scheduler)).toString());
}
void saveJobHeaderTemplate(const QString &scheduler, const QString &text) {
    if (scheduler != "slurm" && scheduler != "pbs")
        fail("Unknown scheduler: " + scheduler);
    if (const auto errors = jobHeaderErrors(text); !errors.isEmpty())
        fail(errors.join('\n'));
    auto settings = userPreferences();
    const auto key = Preference::jobHeader(scheduler);
    QStringList lines;
    for (const auto &line : text.split('\n'))
        if (!line.trimmed().isEmpty())
            lines << line.trimmed();
    const auto normalized = lines.isEmpty() ? QString() : lines.join('\n') + '\n';
    if (normalized.isEmpty() || normalized == defaultJobHeader(scheduler))
        settings.remove(key);
    else
        settings.setValue(key, normalized);
}

std::optional<RemoteHost> ExecutionEnvironment::remote(const QString &name) const {
    if (name.isEmpty())
        return std::nullopt;
    for (const auto &host : remotes)
        if (host.name == name)
            return host;
    return std::nullopt;
}
QString ExecutionEnvironment::jobHeader(const QString &scheduler) const {
    return usableJobHeader(scheduler, jobHeaders.value(scheduler));
}
ExecutionEnvironment executionEnvironment() {
    const auto store = userPreferences();
    ExecutionEnvironment environment;
    environment.preferences = storedPreferences(store);
    environment.remotes = storedRemoteHosts(store);
    environment.jobPrelude = store.value(Preference::jobPrelude).toString();
    for (const auto &scheduler : {QString("slurm"), QString("pbs")})
        environment.jobHeaders[scheduler] = store.value(Preference::jobHeader(scheduler)).toString();
    return environment;
}
} // namespace mf
