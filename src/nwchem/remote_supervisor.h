// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QString>

namespace mf {
// The supervisor (runJobMain) of a remote run: copies the job there, submits it, follows it and copies
// the folder back when it ends. Started again for a job it left (resume), it goes on from the record.
// It loads the destinations of Preferences once, when it starts: only a destination that is still
// among them, where it still leads, is connected to.
int remoteJobMain(const QString &directory, QJsonObject manifest);
// Starts a supervisor for a remote run whose supervisor has ended (the computer restarted): it looks
// at the job again and copies its results back once it ends. False when the record is no such run
// (or is followed on another computer). program: this executable (runSupervisorProgram()).
bool resumeRemoteRun(const QString &directory, const QString &program, qint64 *pid = nullptr);
} // namespace mf
