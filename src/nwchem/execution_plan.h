// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "nwchem/execution_environment.h"
#include "nwchem/nwchem.h"
#include <QJsonObject>
#include <QMap>

namespace mf {
// What starts one calculation: its job folder, the program with its arguments and environment, the
// generated input and the record (run.json) of the job. The environment of the functions is that of
// execution_settings.h.
struct ExecutionPlan {
    QString directory, program, input;
    NwchemInputOutline inputOutline;
    QStringList arguments;
    QMap<QString, QString> environment;
    QJsonObject manifest;
    int timeoutSeconds = 0;
};
ExecutionPlan executionPlan(const Document &document, const QString &projectDirectory,
                            const QString &jobDirectory, bool checkLocal = true,
                            const ExecutionEnvironment &environment = executionEnvironment());
// Side-effect-free plan with a placeholder job directory; never saves or launches anything.
ExecutionPlan executionPreviewPlan(const Document &document, const QString &projectDirectory,
                                   bool checkLocal = false,
                                   const ExecutionEnvironment &environment = executionEnvironment());
QString executionCommand(const ExecutionPlan &plan);
ExecutionPlan saveExecutionBundle(const Document &document, const QString &projectDirectory,
                                  bool forRun = false,
                                  const ExecutionEnvironment &environment = executionEnvironment());
} // namespace mf
