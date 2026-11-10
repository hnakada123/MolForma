// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/run_state.h"
#include <QJsonArray>
namespace mf {
// Stable codes, original evidence and setting targets; translated descriptions are never persisted.
QJsonArray diagnoseRun(RunState state, const QString &output, const QString &errors,
                       const QString &launchError = {});
QString runDiagnosisText(const QString &code);
} // namespace mf
