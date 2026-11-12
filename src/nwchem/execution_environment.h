// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "nwchem/remote_host.h"
#include <QJsonObject>
#include <QMap>
#include <QVector>
#include <optional>

namespace mf {
// The machine-wide part of running calculations has its one owner here: what this computer's
// Preferences keep of it (model/settings.h names the keys), the functions that read and save each part,
// and the snapshot of all of it that the checks and generators of a run are given. No other code of this
// layer reads these preferences.

// The execution settings the user saved for new documents (Preferences "execution/defaults"). Only
// machine-wide keys are kept (executionPreferenceKeys); per-project ones such as reference_directory
// are not. A key that is not saved, or saved with another type, has its default.
QJsonObject executionPreferences();
void saveExecutionPreferences(const QJsonObject &settings);
// Writes the saved preferences into a new document (only values that differ from the defaults).
void applyExecutionPreferences(Document &document);
// Lines the user wants in every job script before NWChem starts ("module load nwchem"): kept in
// Preferences only, never in a project, since they are run as they are.
QString executionJobPrelude();
void saveExecutionJobPrelude(const QString &lines);
// The destinations of remote runs saved in Preferences ("execution/remotes").
QVector<RemoteHost> remoteHosts();
// Checks every destination (remoteHostErrors, names told apart) and saves them; fails otherwise.
void saveRemoteHosts(const QVector<RemoteHost> &hosts);
// The saved destination of that name, for the forms that show it.
std::optional<RemoteHost> findRemoteHost(const QString &name);
// The header template of a scheduler (slurm, pbs) saved in Preferences, else the default.
QString jobHeaderTemplate(const QString &scheduler);
// Saves a template (the default when it is empty or the same); fails when jobHeaderErrors finds any.
void saveJobHeaderTemplate(const QString &scheduler, const QString &text);

// What the Preferences say about running calculations, as one value. The checks and generators of a run
// (validateExecution, executionPlan, executionJobScript, saveExecutionBundle, …) take it as an argument
// and read nothing from the Preferences themselves: their results depend on their arguments alone, and
// one run or one saved job is checked and written with one state of the Preferences. A
// default-constructed one is a computer without saved preferences: no destinations, no preparation
// lines, the default headers, and nothing that makes a network location trusted.
struct ExecutionEnvironment {
    // executionPreferences(): the network locations a document may name without the user having chosen
    // them elsewhere (untrustedNetworkPath).
    QJsonObject preferences;
    // remoteHosts(): the destinations of remote runs.
    QVector<RemoteHost> remotes;
    // executionJobPrelude(): the lines of every job script before NWChem starts.
    QString jobPrelude;
    // The header templates of job scripts by scheduler ("slurm", "pbs"), as saved; see jobHeader().
    QMap<QString, QString> jobHeaders;

    // The destination of that name; none for an empty name or one that is not among `remotes`.
    std::optional<RemoteHost> remote(const QString &name) const;
    // The header template of a scheduler: the one of `jobHeaders`, or the default when that is absent,
    // blank or not a header (jobHeaderErrors). What jobHeaderTemplate() gives for the saved one.
    QString jobHeader(const QString &scheduler) const;
};
// The one loader: the snapshot of the Preferences as they are now. Runs, queued calculations and saved
// jobs load it once where they begin and pass it down.
ExecutionEnvironment executionEnvironment();
} // namespace mf
