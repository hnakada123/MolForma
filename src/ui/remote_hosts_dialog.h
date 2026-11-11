// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/remote_execution.h"
#include <QDialog>
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
namespace mf {
// "リモート実行先": the computers of Preferences that run calculations over SSH, as a list and the
// form of the chosen one, with "接続を確認". Saved on OK (saveRemoteHosts). MCP cannot operate it.
class RemoteHostsDialog : public QDialog {
    Q_OBJECT
  public:
    // executable, launcher: the names "3 実行設定" gives, which a destination without its own uses.
    RemoteHostsDialog(const QString &executable, const QString &launcher, QWidget *parent = nullptr);
    void accept() override;
    // Shows the destination of that name (after the list was filled).
    void select(const QString &name);

  private:
    QVector<RemoteHost> hosts;
    int current = -1;
    QString executable, launcher;
    QListWidget *list;
    QWidget *form;
    QLineEdit *name, *host, *directory, *nwchem, *mpi;
    QSpinBox *port, *interval;
    QComboBox *scheduler;
    QPlainTextEdit *header, *prelude;
    QPushButton *remove, *probe;
    QLabel *problems, *probeResult;
    RemoteProbe prober;
    bool showing = false;
    void show(int index);
    void store(); // The form into hosts[current].
    void check(); // The problems of the destination shown.
};
} // namespace mf
