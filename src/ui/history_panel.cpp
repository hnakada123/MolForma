// SPDX-License-Identifier: Apache-2.0
// History browsing/reuse workflow adapted from KAGAMI app.py for native NWChem bundles.
#include "ui/history_panel.h"
#include "ui/execution_panel.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include <QtWidgets>

namespace mf {
namespace {
class SortItem : public QTableWidgetItem {
  public:
    SortItem(const QString &text, const QVariant &key) : QTableWidgetItem(text) {
        setData(Qt::UserRole + 1, key);
    }
    bool operator<(const QTableWidgetItem &other) const override {
        const auto a = data(Qt::UserRole + 1), b = other.data(Qt::UserRole + 1);
        if (a.metaType().id() == QMetaType::Double || a.metaType().id() == QMetaType::LongLong)
            return a.toDouble() < b.toDouble();
        return QString::localeAwareCompare(a.toString(), b.toString()) < 0;
    }
};
void text(QPlainTextEdit *edit, const QString &value, bool follow = false) {
    if (edit->toPlainText() == value)
        return;
    const int old = edit->verticalScrollBar()->value();
    edit->setPlainText(value);
    edit->verticalScrollBar()->setValue(follow ? edit->verticalScrollBar()->maximum() : old);
}
} // namespace
HistoryPanel::HistoryPanel(MainWindow *owner) : QWidget(owner), window(owner), scan(this) {
    setObjectName("historyTab");
    auto *layout = new QVBoxLayout(this);
    auto *paths = new QHBoxLayout;
    paths->addWidget(new QLabel("ジョブ保存先"));
    rootField = new QLineEdit;
    rootField->setObjectName("historyRoot");
    // With --mcp-allow-dir, MCP scans only checked paths through history_action.
    rootField->setProperty("mcpPath", true);
    paths->addWidget(rootField, 1);
    auto *browse = new QPushButton("参照…");
    browse->setProperty("mcpPath", true);
    paths->addWidget(browse);
    auto *current = new QPushButton("実行設定の保存先");
    current->setObjectName("historyExecutionRoot");
    current->setProperty("mcpPath", true);
    paths->addWidget(current);
    layout->addLayout(paths);
    auto *filters = new QHBoxLayout;
    search = new QLineEdit;
    search->setObjectName("historySearch");
    search->setPlaceholderText("計算名・手法・基底・目的・フォルダーを検索");
    search->setClearButtonEnabled(true);
    filters->addWidget(search, 1);
    filter = new QComboBox;
    filter->setObjectName("historyFilter");
    filter->addItem("すべての状態", "");
    for (const auto &s :
         {"prepared", "running", "unconfirmed", "finished", "failed", "cancelled", "timed_out", "invalid"})
        filter->addItem(QString(s) == "running"       ? "このウィンドウで実行中"
                        : QString(s) == "unconfirmed" ? "稼働未確認"
                                                      : historyStatus(s),
                        s);
    filters->addWidget(filter);
    refreshButton = new QPushButton("更新");
    refreshButton->setObjectName("historyRefresh");
    filters->addWidget(refreshButton);
    cancel = new QPushButton("読込を中止");
    cancel->setObjectName("historyCancel");
    cancel->setEnabled(false);
    filters->addWidget(cancel);
    layout->addLayout(filters);
    auto *actions = new QHBoxLayout;
    auto button = [&](const QString &label, const QString &name) {
        auto *b = new QPushButton(label);
        b->setObjectName(name);
        actions->addWidget(b);
        return b;
    };
    viewer = button("結果ビューアで開く", "historyResults");
    reuse = button("分子・条件を再利用", "historyReuse");
    folder = button("保存先を開く", "historyFolder");
    csv = button("一覧CSVを保存", "historyCsv");
    actions->addStretch();
    stop = button("選択した計算を停止", "historyStop");
    trash = button("ごみ箱へ", "historyTrash");
    layout->addLayout(actions);
    status = new QLabel;
    status->setObjectName("historyStatus");
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto *split = new QSplitter(Qt::Vertical);
    layout->addWidget(split, 1);
    table = new QTableWidget(0, 7);
    table->setObjectName("historyTable");
    table->setHorizontalHeaderLabels({"作成日時（ローカル）", "計算名", "手法 / 基底", "目的", "実行状態",
                                      "最終検出E / Hartree", "経過 / 秒"});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->hide();
    table->setAlternatingRowColors(true);
    table->setSortingEnabled(true);
    table->sortItems(0, Qt::DescendingOrder);
    const QList<int> widths{175, 160, 240, 145, 205, 180, 85};
    for (int i = 0; i < widths.size(); ++i)
        table->setColumnWidth(i, widths[i]);
    table->horizontalHeader()->setStretchLastSection(true);
    split->addWidget(table);
    auto *lower = new QWidget;
    auto *lowerLayout = new QVBoxLayout(lower);
    lowerLayout->setContentsMargins(0, 0, 0, 0);
    follow = new QCheckBox("ログを追尾（1秒間隔・末尾128 KiB）");
    follow->setObjectName("historyFollow");
    follow->setChecked(true);
    lowerLayout->addWidget(follow);
    details = new QTabWidget;
    details->setObjectName("historyDetails");
    lowerLayout->addWidget(details, 1);
    auto pane = [&](const QString &label, const QString &name) {
        auto *p = new QPlainTextEdit;
        p->setObjectName(name);
        p->setReadOnly(true);
        p->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        p->setLineWrapMode(QPlainTextEdit::NoWrap);
        details->addTab(p, label);
        return p;
    };
    summary = pane("概要", "historySummary");
    summary->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    input = pane("保存した入力", "historyInput");
    output = pane("標準出力", "historyOutput");
    errors = pane("標準エラー", "historyError");
    record = pane("実行記録", "historyRecord");
    split->addWidget(lower);
    split->setSizes({300, 400});
    connect(rootField, &QLineEdit::editingFinished, this, [this] {
        if (rootField->isModified())
            setRoot(rootField->text());
        rootField->setModified(false);
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto p = QFileDialog::getExistingDirectory(this, "ジョブ保存先", root);
        if (!p.isEmpty())
            setRoot(p);
    });
    connect(current, &QPushButton::clicked, this, [this] {
        window->execution->commitPendingEdits();
        setRoot(historyRoot(window->document(), window->path.isEmpty()
                                                    ? QDir::currentPath()
                                                    : QFileInfo(window->path).absolutePath()),
                false);
    });
    connect(search, &QLineEdit::textChanged, this, [this] { populate(); });
    connect(filter, &QComboBox::currentIndexChanged, this, [this] { populate(); });
    connect(refreshButton, &QPushButton::clicked, this, &HistoryPanel::refresh);
    connect(cancel, &QPushButton::clicked, this, [this] {
        scan.cancel();
        cancel->setEnabled(false);
        refreshButton->setEnabled(true);
        status->setText("読込を中止しました。前の一覧を表示しています。");
    });
    connect(&scan, &HistoryScan::finished, this, [this] {
        entries = scan.entries;
        for (const auto &path : std::as_const(changedJobs)) {
            bool found = false;
            for (auto &e : entries)
                if (e.directory == path) {
                    e = readHistoryEntry(path);
                    found = true;
                    break;
                }
            if (!found && entries.size() < 2000)
                entries << readHistoryEntry(path);
        }
        changedJobs.clear();
        cancel->setEnabled(false);
        refreshButton->setEnabled(true);
        populate();
        if (!scan.message.isEmpty())
            status->setText(status->text() + "\n" + scan.message);
    });
    connect(table, &QTableWidget::itemSelectionChanged, this, &HistoryPanel::showDetails);
    connect(table, &QTableWidget::cellDoubleClicked, this,
            [this] { window->safe([this] { openResult(); }); });
    connect(viewer, &QPushButton::clicked, this, [this] { window->safe([this] { openResult(); }); });
    connect(reuse, &QPushButton::clicked, this, [this] { window->safe([this] { reuseSelected(); }); });
    connect(folder, &QPushButton::clicked, this, [this] {
        window->safe([this] {
            if (selectedDirectory().isEmpty())
                return;
            window->checkMcpPath(selectedDirectory());
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(selectedDirectory())))
                fail("保存先を開けません。");
        });
    });
    connect(csv, &QPushButton::clicked, this, [this] {
        auto p =
            QFileDialog::getSaveFileName(this, "表示中の一覧を保存", "nwchem-history.csv", "CSV (*.csv)");
        if (!p.isEmpty())
            window->safe([&] {
                if (QFileInfo(p).suffix().isEmpty())
                    p += ".csv";
                exportCsv(p);
            });
    });
    connect(stop, &QPushButton::clicked, this, [this] {
        if (selectedDirectory() == activeDirectory())
            window->execution->stopCalculation();
    });
    connect(trash, &QPushButton::clicked, this, [this] { window->safe([this] { trashSelected(); }); });
    timer.setInterval(1000);
    connect(&timer, &QTimer::timeout, this, [this] {
        if (!isVisible())
            return;
        const auto active = activeDirectory();
        if (!active.isEmpty())
            notifyJob(active);
        if (follow->isChecked())
            updateLogs();
        updateActions();
    });
    timer.start();
    const auto saved = window->settings.value("history/root").toString();
    followsExecution = saved.isEmpty();
    root = saved;
    rootField->setText(root);
    updateActions();
}
void HistoryPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (followsExecution) {
        const auto next =
            historyRoot(window->document(), window->path.isEmpty() ? QDir::currentPath()
                                                                   : QFileInfo(window->path).absolutePath());
        if (next != root) {
            setRoot(next, false);
            return;
        }
    }
    refresh();
}
void HistoryPanel::setRoot(const QString &path, bool remember) {
    auto value = path.trimmed();
    if (value == "~")
        value = QDir::homePath();
    else if (value.startsWith("~/"))
        value = QDir::home().filePath(value.mid(2));
    if (value.isEmpty())
        return;
    value = QFileInfo(value).absoluteFilePath();
    followsExecution = !remember;
    window->settings.setValue("history/root", remember ? value : QString());
    if (root != value) {
        scan.cancel();
        entries.clear();
        changedJobs.clear();
        pendingSelection.clear();
        root = value;
        populate();
    }
    rootField->setText(root);
    refresh();
}
void HistoryPanel::refresh() {
    if (root.isEmpty())
        return;
    try { // Showing the tab over MCP must not scan outside --mcp-allow-dir.
        window->checkMcpPath(root);
    } catch (const std::exception &e) {
        status->setText(QString::fromUtf8(e.what()));
        return;
    }
    if (pendingSelection.isEmpty())
        pendingSelection = selectedDirectory();
    cancel->setEnabled(true);
    refreshButton->setEnabled(false);
    status->setText("ジョブを読み込んでいます…");
    scan.start(root);
}
void HistoryPanel::selectDirectory(const QString &path) {
    if (path.isEmpty())
        return;
    const auto parent = QFileInfo(path).absolutePath();
    if (parent != root)
        setRoot(parent);
    pendingSelection = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    refresh();
}
QString HistoryPanel::selectedDirectory() const {
    const auto selected = table->selectionModel()->selectedRows();
    return selected.isEmpty() ? QString()
                              : table->item(selected.first().row(), 0)->data(Qt::UserRole).toString();
}
const HistoryEntry *HistoryPanel::selectedEntry() const {
    const auto path = selectedDirectory();
    for (const auto &e : entries)
        if (e.directory == path)
            return &e;
    return nullptr;
}
QString HistoryPanel::activeDirectory() const {
    const auto &run = window->execution->currentRun();
    return run.active() ? run.plan().directory : QString();
}
void HistoryPanel::populate(bool reloadDetails) {
    const auto selected = pendingSelection.isEmpty() ? selectedDirectory() : pendingSelection;
    pendingSelection.clear();
    const auto query = search->text().trimmed();
    const auto state = filter->currentData().toString(), active = activeDirectory();
    {
        QSignalBlocker block(table);
        table->setSortingEnabled(false);
        table->setRowCount(0);
        for (const auto &e : entries) {
            const bool owned = e.directory == active;
            const bool uncertain =
                !owned && QStringList{"starting", "running", "stopping"}.contains(e.status);
            if (!state.isEmpty() && !(state == "running"       ? owned
                                      : state == "unconfirmed" ? uncertain
                                                               : state == e.status))
                continue;
            if (!(e.name + " " + e.method + " " + e.basis + " " + e.task + " " + e.directory)
                     .contains(query, Qt::CaseInsensitive))
                continue;
            const int row = table->rowCount();
            table->insertRow(row);
            const QStringList labels{
                e.created.isValid() ? e.created.toLocalTime().toString("yyyy-MM-dd HH:mm:ss") : "—",
                e.name,
                e.method + " / " + e.basis,
                e.task,
                historyStatus(e.status, owned),
                e.energy ? QString::number(*e.energy, 'f', 10) + " (" + e.energyMethod + ")" : "—",
                e.seconds < 0 ? "—" : QString::number(e.seconds)};
            for (int col = 0; col < labels.size(); ++col) {
                QVariant key = labels[col];
                if (col == 0)
                    key = e.created.toMSecsSinceEpoch();
                if (col == 5)
                    key = e.energy.value_or(-std::numeric_limits<double>::infinity());
                if (col == 6)
                    key = e.seconds;
                auto *item = new SortItem(labels[col], key);
                item->setData(Qt::UserRole, e.directory);
                item->setToolTip(e.directory + (e.warnings.isEmpty() ? "" : "\n" + e.warnings.join('\n')));
                table->setItem(row, col, item);
            }
        }
        table->setSortingEnabled(true);
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0)->data(Qt::UserRole).toString() == selected)
                table->selectRow(row);
    }
    status->setText(
        QString("%1 / %2 件　·　終了コード0は収束の保証ではありません。Eはログ末尾の最後の全エネルギーです。")
            .arg(table->rowCount())
            .arg(entries.size()));
    if (reloadDetails || selectedDirectory() != selected)
        showDetails();
    else
        updateActions();
}
void HistoryPanel::showDetails() {
    const auto *e = selectedEntry();
    try {
        if (e)
            window->checkMcpPath(e->directory);
    } catch (const std::exception &) {
        e = nullptr; // An MCP selection does not read jobs outside --mcp-allow-dir.
    }
    if (!e) {
        for (auto *p : {summary, input, output, errors, record})
            p->clear();
        updateActions();
        return;
    }
    QStringList lines{e->name,
                      e->directory,
                      "状態: " + historyStatus(e->status, e->directory == activeDirectory()),
                      "手法 / 基底: " + e->method + " / " + e->basis,
                      "目的: " + e->task,
                      "開始: " + e->manifest["started_at"].toString() +
                          "　終了: " + e->manifest["finished_at"].toString(),
                      "終了コード: " + (e->manifest.contains("exit_code")
                                            ? QString::number(e->manifest["exit_code"].toInt())
                                            : "未記録"),
                      "入力照合: " + historyIntegrity(*e)};
    lines << e->warnings;
    if (!e->manifest["error"].toString().isEmpty())
        lines << "実行エラー: " + e->manifest["error"].toString();
    lines << "\nエネルギーは末尾128 KiB内の最後のSCF/DFT/MP2/CCSD/CCSD(T)値です。収束値とは限りません。"
             "異なる手法・基底・組成の値をそのまま比較しないでください。"
             "\n再利用は保存した分子と条件を編集画面へ取り込みます。計算の開始は「5 実行設定」で行います。";
    text(summary, lines.join('\n'));
    for (auto pair : {qMakePair(input, "input.nw"), qMakePair(record, "run.json")}) {
        try {
            text(pair.first, historyText(e->directory, pair.second));
        } catch (const std::exception &error) {
            text(pair.first, QString::fromUtf8(error.what()));
        }
    }
    updateLogs();
    updateActions();
}
void HistoryPanel::updateLogs() {
    const auto directory = selectedDirectory();
    if (directory.isEmpty())
        return;
    for (auto pair : {qMakePair(output, "output.out"), qMakePair(errors, "stderr.err")}) {
        try {
            text(pair.first, historyText(directory, pair.second, true), follow->isChecked());
        } catch (const std::exception &error) {
            text(pair.first, QString::fromUtf8(error.what()));
        }
    }
}
void HistoryPanel::updateActions() {
    const auto *e = selectedEntry();
    const bool exists = e && QFileInfo(e->directory).isDir();
    viewer->setEnabled(exists && !resultFiles(e->directory).isEmpty());
    folder->setEnabled(exists);
    reuse->setEnabled(exists && e->valid && !window->hasCandidate() && !window->worker);
    stop->setEnabled(exists && e->directory == activeDirectory() &&
                     window->execution->currentRun().state() != "stopping");
    trash->setEnabled(exists && e->valid && e->directory != activeDirectory() &&
                      !QStringList{"starting", "running", "stopping"}.contains(e->status));
    csv->setEnabled(table->rowCount() > 0);
}
void HistoryPanel::notifyJob(const QString &path) {
    if (!isVisible() || QFileInfo(path).absolutePath() != root)
        return;
    if (scan.active()) {
        changedJobs.insert(path);
        return;
    }
    auto next = readHistoryEntry(path);
    if (path == activeDirectory()) {
        next.status = window->execution->currentRun().state();
        next.seconds = window->execution->currentRun().elapsedSeconds();
    }
    for (auto &e : entries)
        if (e.directory == path) {
            const bool transition = e.status != next.status;
            e = next;
            populate(transition);
            return;
        }
    entries << next;
    populate();
}
void HistoryPanel::reuseSelected() {
    const auto directory = selectedDirectory();
    if (directory.isEmpty())
        return;
    window->checkMcpPath(directory);
    if (window->hasCandidate() || window->worker)
        fail("候補を確定・取消し、整形の終了後に再利用してください。");
    const auto next = historyDocument(directory);
    if (!window->confirmDiscard())
        return;
    window->path.clear();
    window->loadDocument(next);
    window->history()->resetClean();
    window->refresh();
    window->workspaceTabs->setCurrentIndex(2);
    window->statusBar()->showMessage(
        "履歴から新しい未保存プロジェクトを作成しました。実行前に条件・参照ファイルを確認してください。",
        15000);
}
void HistoryPanel::openResult() {
    const auto directory = selectedDirectory();
    if (directory.isEmpty())
        return;
    window->checkMcpPath(directory);
    if (resultFiles(directory).isEmpty())
        return;
    window->workspaceTabs->setCurrentWidget(window->results);
    window->results->openDirectory(directory);
}
void HistoryPanel::trashSelected() {
    const auto directory = selectedDirectory();
    if (directory.isEmpty())
        return;
    window->checkMcpPath(directory);
    if (QMessageBox::question(
            this, "ジョブをごみ箱へ",
            "入力・プロジェクト・結果を含む次のフォルダーをごみ箱へ移動します。\n" + directory,
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    trashHistoryEntry(directory, root, activeDirectory());
    refresh();
}
void HistoryPanel::exportCsv(const QString &path) const {
    QVector<HistoryEntry> visible;
    for (int row = 0; row < table->rowCount(); ++row)
        for (const auto &e : entries)
            if (e.directory == table->item(row, 0)->data(Qt::UserRole).toString()) {
                visible << e;
                break;
            }
    writeFile(path, historyCsv(visible).toUtf8());
}
} // namespace mf
