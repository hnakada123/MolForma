// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/remote_hosts_dialog.h"
#include "nwchem/execution_environment.h"
#include "ui/chrome_layouts.h"
#include "ui/line_icons.h"
#include "ui/theme.h"
#include <QtWidgets>

namespace mf {
namespace {
QPlainTextEdit *lines(const QString &objectName, const QString &placeholder, int rows) {
    auto *edit = new QPlainTextEdit;
    edit->setObjectName(objectName);
    edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    edit->setTabChangesFocus(true);
    edit->setFixedHeight(edit->fontMetrics().lineSpacing() * rows + 16);
    edit->setPlaceholderText(placeholder);
    return edit;
}
QLabel *note(const QString &text) {
    auto *label = new QLabel(text);
    label->setProperty("caption", true);
    label->setWordWrap(true);
    return label;
}
} // namespace

RemoteHostsDialog::RemoteHostsDialog(const QString &program, const QString &mpiLauncher, QWidget *parent)
    : QDialog(parent), hosts(remoteHosts()), executable(program), launcher(mpiLauncher), prober(this) {
    setObjectName("remoteHostsDialog");
    setProperty("mcpProtected", true); // Where calculations go and what runs there: the user's choice.
    setProperty("canvas", true);
    setWindowTitle(tr("リモート実行先"));
    resize(860, 640);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);
    auto *intro = new QLabel(tr("SSHで接続する計算機を登録します。「3 "
                                "実行設定」の「実行先」で選ぶと、「計算を実行」でジョブフォルダーを"
                                "ssh・scpで複写して投入し、終わると結果を同じジョブフォルダーへ回収します。鍵"
                                "とパスワードは保存しません"
                                "（ssh-agent と ~/.ssh/config を使います）。"));
    intro->setWordWrap(true);
    intro->setProperty("muted", true);
    layout->addWidget(intro);
    auto *split = new QHBoxLayout;
    split->setSpacing(12);
    layout->addLayout(split, 1);
    auto *left = new QVBoxLayout;
    list = new QListWidget;
    list->setObjectName("remoteHostsList");
    list->setMinimumWidth(180);
    list->setMaximumWidth(240);
    left->addWidget(list, 1);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton(tr("追加"));
    add->setObjectName("remoteHostsAdd");
    add->setIcon(lineIcon("plus"));
    remove = new QPushButton(tr("削除"));
    remove->setObjectName("remoteHostsRemove");
    buttons->addWidget(add);
    buttons->addWidget(remove);
    left->addLayout(buttons);
    split->addLayout(left);

    form = new QWidget;
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(form);
    split->addWidget(scroll, 1);
    auto *rows = new FormLayout(form);
    rows->setContentsMargins(0, 0, 4, 0);
    rows->setHorizontalSpacing(12);
    rows->setVerticalSpacing(8);
    name = new QLineEdit;
    name->setObjectName("remoteHostName");
    name->setPlaceholderText(tr("例: cluster"));
    rows->addRow(tr("名前"), name);
    host = new QLineEdit;
    host->setObjectName("remoteHostHost");
    host->setPlaceholderText(tr("例: user@login.example.org"));
    host->setToolTip(tr("ssh に渡す接続先です。~/.ssh/config の Host 名も使えます。"));
    rows->addRow(tr("接続先（ssh）"), host);
    port = new QSpinBox;
    port->setObjectName("remoteHostPort");
    port->setRange(0, 65535);
    port->setSpecialValueText(tr("sshの設定どおり"));
    rows->addRow(tr("ポート"), port);
    directory = new QLineEdit;
    directory->setObjectName("remoteHostDirectory");
    directory->setPlaceholderText("~/molforma-jobs");
    directory->setToolTip(
        tr("実行先でジョブフォルダーを作る場所です。~/ で始めるとホームフォルダーからのパスです。"));
    rows->addRow(tr("作業フォルダー"), directory);
    scheduler = new QComboBox;
    scheduler->setObjectName("remoteHostScheduler");
    for (const auto &kind : remoteSchedulers())
        scheduler->addItem(remoteSchedulerName(kind), kind);
    scheduler->setToolTip(tr("シェルはログインした計算機で job.sh をバックグラウンドで起動し、SlurmとPBSは"
                             "スケジューラーへ投入します。"));
    rows->addRow(tr("投入方式"), scheduler);
    nwchem = new QLineEdit;
    nwchem->setObjectName("remoteHostExecutable");
    nwchem->setPlaceholderText(tr("空欄: 「3 実行設定」の名前"));
    rows->addRow(tr("NWChem実行ファイル"), nwchem);
    mpi = new QLineEdit;
    mpi->setObjectName("remoteHostLauncher");
    mpi->setPlaceholderText(tr("空欄: 「3 実行設定」の名前"));
    rows->addRow(tr("MPIランチャー"), mpi);
    interval = new QSpinBox;
    interval->setObjectName("remoteHostInterval");
    interval->setRange(10, 3600);
    interval->setSuffix(tr(" 秒"));
    interval->setToolTip(tr("実行中のジョブの状態とログを確かめる間隔です。1回ごとにSSHで接続します。"));
    rows->addRow(tr("状態の確認間隔"), interval);
    header = lines("remoteHostHeader", tr("例: #SBATCH --account=project123"), 3);
    header->setToolTip(tr("SlurmまたはPBSの見出し（環境設定のテンプレート）の後に加える行です。# で始め、"
                          "{ranks} などのプレースホルダーも使えます。"));
    rows->addRow(tr("見出しの追加行"), header);
    prelude = lines("remoteHostPrelude", tr("例: module load nwchem/7.3.1"), 3);
    prelude->setToolTip(tr("この実行先の job.sh で、NWChemの前に実行する行です（環境設定の準備の行の後）。"));
    rows->addRow(tr("準備の行"), prelude);
    problems = new QLabel;
    problems->setObjectName("remoteHostProblems");
    problems->setProperty("bad", true);
    problems->setWordWrap(true);
    problems->setTextFormat(Qt::PlainText);
    rows->addRow(QString(), problems);
    auto *probeRow = new QHBoxLayout;
    probe = new QPushButton(tr("接続を確認"));
    probe->setObjectName("remoteHostProbe");
    probe->setToolTip(tr("一度SSHで接続し、作業フォルダー・スケジューラーのコマンド・NWChemを確かめます。"));
    probeRow->addWidget(probe);
    probeRow->addStretch();
    rows->addRow(QString(), probeRow);
    probeResult = new QLabel;
    probeResult->setObjectName("remoteHostProbeResult");
    probeResult->setWordWrap(true);
    probeResult->setTextFormat(Qt::PlainText);
    probeResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rows->addRow(QString(), probeResult);
    rows->addRow(QString(),
                 note(tr("初めて接続する計算機は、先に端末で「ssh 接続先」を実行してホスト鍵を登録して"
                         "ください。パスワードの入力が必要な接続には対応していません。")));

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, &RemoteHostsDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &RemoteHostsDialog::reject);

    for (const auto &h : std::as_const(hosts))
        list->addItem(h.name);
    connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
        store();
        show(row);
    });
    connect(add, &QPushButton::clicked, this, [this] {
        store();
        RemoteHost h;
        QStringList names;
        for (const auto &other : std::as_const(hosts))
            names << other.name;
        h.name = tr("実行先");
        for (int n = 2; names.contains(h.name); ++n)
            h.name = tr("実行先 %1").arg(n);
        hosts << h;
        list->addItem(h.name);
        list->setCurrentRow(list->count() - 1);
        host->setFocus();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = list->currentRow();
        if (row < 0 || row >= hosts.size())
            return;
        current = -1; // Not stored again.
        hosts.removeAt(row);
        delete list->takeItem(row);
        show(list->currentRow());
    });
    for (auto *line : {name, host, directory, nwchem, mpi})
        connect(line, &QLineEdit::textEdited, this, [this] {
            store();
            check();
        });
    for (auto *spin : {port, interval})
        connect(spin, &QSpinBox::valueChanged, this, [this] {
            store();
            check();
        });
    connect(scheduler, &QComboBox::currentIndexChanged, this, [this] {
        store();
        check();
    });
    for (auto *text : {header, prelude})
        connect(text, &QPlainTextEdit::textChanged, this, [this] {
            store();
            check();
        });
    connect(probe, &QPushButton::clicked, this, [this] {
        store();
        if (current < 0)
            return;
        probe->setEnabled(false);
        probeResult->setProperty("bad", false);
        probeResult->setProperty("good", false);
        repolish(probeResult);
        probeResult->setText(tr("%1 に接続しています…").arg(hosts[current].host));
        // As a job script prepares NWChem there: with the preparation lines saved in Preferences.
        prober.start(hosts[current], executionJobPrelude(), executable, launcher);
    });
    connect(&prober, &RemoteProbe::finished, this, [this](bool ok, const QString &report) {
        probe->setEnabled(current >= 0);
        probeResult->setText(report);
        probeResult->setProperty("good", ok);
        probeResult->setProperty("bad", !ok);
        repolish(probeResult);
    });
    list->setCurrentRow(hosts.isEmpty() ? -1 : 0);
    show(list->currentRow());
}
void RemoteHostsDialog::select(const QString &wanted) {
    for (int row = 0; row < hosts.size(); ++row)
        if (hosts[row].name == wanted)
            list->setCurrentRow(row);
}
void RemoteHostsDialog::show(int index) {
    QScopedValueRollback guard(showing, true);
    prober.cancel();
    current = index >= 0 && index < hosts.size() ? index : -1;
    form->setEnabled(current >= 0);
    remove->setEnabled(current >= 0);
    probe->setEnabled(current >= 0);
    probeResult->clear();
    const auto h = current >= 0 ? hosts[current] : RemoteHost{};
    name->setText(current >= 0 ? h.name : QString());
    host->setText(h.host);
    port->setValue(h.port);
    directory->setText(current >= 0 ? h.directory : QString());
    scheduler->setCurrentIndex(std::max(0, scheduler->findData(h.scheduler)));
    nwchem->setText(h.executable);
    mpi->setText(h.launcher);
    interval->setValue(h.interval);
    header->setPlainText(h.header);
    prelude->setPlainText(h.prelude);
    check();
}
void RemoteHostsDialog::store() {
    if (showing || current < 0 || current >= hosts.size())
        return;
    auto &h = hosts[current];
    h.name = name->text().trimmed();
    h.host = host->text().trimmed();
    h.port = port->value();
    h.directory = directory->text().trimmed();
    h.scheduler = scheduler->currentData().toString();
    h.executable = nwchem->text().trimmed();
    h.launcher = mpi->text().trimmed();
    h.interval = interval->value();
    h.header = header->toPlainText().trimmed();
    h.prelude = prelude->toPlainText().trimmed();
    if (auto *item = list->item(current); item && item->text() != h.name)
        item->setText(h.name.isEmpty() ? tr("（名前なし）") : h.name);
}
void RemoteHostsDialog::check() {
    header->setEnabled(current >= 0 && hosts[current].scheduler != "shell");
    QStringList errors;
    if (current >= 0) {
        errors = remoteHostErrors(hosts[current]);
        for (int i = 0; i < hosts.size(); ++i)
            if (i != current && hosts[i].name == hosts[current].name)
                errors << tr("実行先の名前が重複しています: %1").arg(hosts[current].name);
    }
    problems->setText(errors.join('\n'));
    problems->setVisible(!errors.isEmpty());
}
void RemoteHostsDialog::accept() {
    store();
    try {
        saveRemoteHosts(hosts);
    } catch (const std::exception &e) {
        QMessageBox box(QMessageBox::Warning, tr("リモート実行先"), QString::fromUtf8(e.what()),
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }
    QDialog::accept();
}
} // namespace mf
