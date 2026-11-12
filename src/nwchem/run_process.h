// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QString>

namespace mf {
// The processes of a run as its record (run.json) names them: which process a record means, whether it
// has ended, and asking the supervisor of a run to stop it.

// Where a run's process lives, recorded in run.json when it starts: host, boot ID and, on Linux,
// the process start time, so a reused PID is not taken for the run.
QJsonObject executionProcessIdentity(qint64 pid);
// For a record left as starting/running/stopping: true when its process is known to have ended
// (recorded on this host, and the PID is gone or reused, or the system restarted since).
bool executionProcessGone(const QJsonObject &manifest);
// Asks the supervisor of a running job to stop it. False when the job has no supervisor on this
// computer (then only the application that runs it can stop it).
bool requestRunStop(const QString &jobDirectory);
// Whether a record left as starting/running/stopping still has its supervisor.
bool runSupervised(const QJsonObject &manifest);
// For such a record: true when the run is known to have ended, its supervisor (if it had one) and
// NWChem alike (see executionProcessGone).
bool executionRunGone(const QJsonObject &manifest);
} // namespace mf
