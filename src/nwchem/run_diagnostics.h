// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/run_state.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
namespace mf {
// Stable codes, original evidence and setting targets; translated descriptions are never persisted.
QJsonArray diagnoseRun(RunState state, const QString &output, const QString &errors,
                       const QString &launchError = {});
QString runDiagnosisText(const QString &code);
// What the diagnostics of a record say, for the run panel and the history: of each its description and,
// when it has one, the line it was found in.
QStringList runDiagnosisLines(const QJsonObject &record);
} // namespace mf
