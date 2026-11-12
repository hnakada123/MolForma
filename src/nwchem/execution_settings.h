// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "nwchem/execution_environment.h"
#include "nwchem/nwchem.h"

namespace mf {
// The checks and generators below that depend on this computer's Preferences take them as an
// ExecutionEnvironment and read no preference themselves. Its default is the snapshot of the Preferences
// as they are at the call (executionEnvironment()); pass one snapshot to several calls that belong
// together.

// Expands "~" and "~/" to the home directory and resolves relative paths against base.
// Every execution path goes through this, and the MCP access policy checks the same result.
QString absolutePath(QString path, const QString &base);
// A UNC path (\\server\share or //server/share): on Windows, merely looking at one connects to that
// server and offers it the user's credentials.
bool networkPath(const QString &path);
// The network location the document's settings name where the saved preferences of the environment do
// not (empty if none): a project from elsewhere must not make MolForma look at it before the user chose
// it.
QString untrustedNetworkPath(const Document &document,
                             const ExecutionEnvironment &environment = executionEnvironment());
QJsonObject executionDefaults();
// The whole-number settings and their ranges, for the checks and the controls of "3 実行設定" and
// Preferences.
struct ExecutionRange {
    QString key;
    int minimum = 0, maximum = 0;
};
const QVector<ExecutionRange> &executionRanges();
ExecutionRange executionRange(const QString &key);
// The settings that Preferences keep for new documents (executionPreferences in
// execution_environment.h): the machine-wide ones, not per-project ones such as reference_directory.
QStringList executionPreferenceKeys();
QJsonObject executionSettings(const Document &document);
void setExecutionSetting(Document &document, const QString &key, const QJsonValue &value);
// One item of the local checks before a run: the program, folder or input checked, what was
// found ("/usr/bin/nwchem"), and the error it stands for in ExecutionValidation::errors, if any.
struct ExecutionCheck {
    // The setting the check is about, as its diagnostic names it ("executable", "launcher",
    // "job_root"); empty for the check of the NWChem input. Find a check by this, never by its label.
    QString key;
    QString label, detail, error;
    bool ok = true;
};
struct ExecutionValidation : ValidationReport {
    QString executable, launcher, referenceDirectory;
    QString remote; // The destination of a remote run, in words (remoteHostSummary); empty for this PC.
    QMap<QString, QString> environment; // Only explicit overrides, never the inherited environment.
    QVector<ExecutionCheck> checks;     // With checkLocal: programs, folders and the NWChem input.
};
ExecutionValidation validateExecution(const Document &document, const QString &projectDirectory,
                                      bool checkLocal = true,
                                      const ExecutionEnvironment &environment = executionEnvironment());
} // namespace mf
