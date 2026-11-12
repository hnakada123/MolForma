// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QString>

namespace mf {
// Runs on a remote destination. The record (run.json) of such a run has "remote": the destination as it
// was ("host"), the job folder there ("directory"), the scheduler's ID of the job ("job"), what was last
// seen ("state") and why the last look failed ("message", "detail").
bool remoteRun(const QJsonObject &manifest);
// One line on where a remote run is and how it stands, for the run panel and the history.
QString remoteRunText(const QJsonObject &manifest);
} // namespace mf
