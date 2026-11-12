// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/remote_host.h"
#include <QObject>
#include <QProcess>

namespace mf {
// "接続を確認" of a destination: connects once, runs the preparation lines of every job script (prelude:
// executionJobPrelude(), given by the caller) and then its own, and reports what was found there (the
// folder, the scheduler's commands, NWChem and the launcher).
class RemoteProbe : public QObject {
    Q_OBJECT
  public:
    explicit RemoteProbe(QObject *parent = nullptr);
    ~RemoteProbe() override;
    // executable and launcher: as "3 実行設定" names them when the destination does not.
    void start(const RemoteHost &host, const QString &prelude, const QString &executable = "nwchem",
               const QString &launcher = "mpirun");
    void cancel();
    bool active() const {
        return process.state() != QProcess::NotRunning;
    }
  signals:
    void finished(bool ok, QString report);

  private:
    QProcess process;
    RemoteHost probed;
    QString nonce, executable, launcher;
    int probes = 0;          // Probes started, so that a time limit ends only its own.
    bool cancelling = false; // A cancelled probe reports nothing.
    void done();
};
} // namespace mf
