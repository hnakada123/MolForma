// SPDX-License-Identifier: Apache-2.0
// Native NWChem execution UI; path/resource/environment workflow adapted from KAGAMI app.py.
#include "ui/execution_panel.h"
#include "nwchem/nwchem.h"
#include "ui/advanced_panel.h"
#include "ui/calculation_panel.h"
#include "ui/history_panel.h"
#include "ui/input_preview_panel.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include <QtWidgets>

namespace mf {
ExecutionPanel::ExecutionPanel(MainWindow *owner) : QWidget(owner), window(owner), run(this) {
    setObjectName("executionTab");
    run.setObjectName("nwchemRun");
    auto *layout = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout;
    layout->addLayout(toolbar);
    auto *basic = new QPushButton("3 計算条件へ");
    auto *advanced = new QPushButton("4 詳細設定へ");
    undo = new QPushButton("元に戻す");
    redo = new QPushButton("やり直す");
    undo->setObjectName("executionUndo");
    redo->setObjectName("executionRedo");
    for (auto *button : {basic, advanced, undo, redo})
        toolbar->addWidget(button);
    toolbar->addStretch();
    auto *preview = new QPushButton("6 入力プレビューへ");
    preview->setObjectName("executionPreview");
    toolbar->addWidget(preview);
    viewResults = new QPushButton("結果ビューアへ");
    viewResults->setObjectName("executionResults");
    toolbar->addWidget(viewResults);
    auto *history = new QPushButton("履歴へ");
    history->setObjectName("executionHistory");
    toolbar->addWidget(history);
    connect(history, &QPushButton::clicked, this, [this] {
        window->workspaceTabs->setCurrentWidget(window->calculationHistory);
        if (!saved.directory.isEmpty())
            window->calculationHistory->selectDirectory(saved.directory);
    });
    connect(viewResults, &QPushButton::clicked, this, [this] {
        window->workspaceTabs->setCurrentWidget(window->results);
        window->results->openDirectory(saved.directory);
    });
    connect(preview, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->inputPreview); });
    toolbar->addWidget(new QLabel("NWChem · ローカル実行"));
    connect(basic, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->calculation); });
    connect(advanced, &QPushButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(window->advanced); });
    connect(undo, &QPushButton::clicked, this, [this] { window->history()->undo(); });
    connect(redo, &QPushButton::clicked, this, [this] { window->history()->redo(); });
    auto *hint = new QLabel("実行ごとに専用フォルダーを作り、入力・プロジェクト・ログを保存します。"
                            "実行中に分子や条件を編集しても、起動済みの計算には反映しません。");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *split = new QSplitter;
    layout->addWidget(split, 1);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    form = new QWidget;
    scroll->setWidget(form);
    auto *forms = new QVBoxLayout(form);
    auto group = [&](const QString &name) {
        auto *box = new QGroupBox(name);
        forms->addWidget(box);
        auto *f = new QFormLayout(box);
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        f->setRowWrapPolicy(QFormLayout::WrapLongRows);
        return f;
    };
    auto *paths = group("実行ファイル・保存先");
    pathField(paths, "NWChem実行ファイル", "executable", false);
    mode = new QComboBox;
    mode->setObjectName("execution_mode");
    mode->addItem("MPIで起動", "mpi");
    mode->addItem("直接実行（対応する逐次版等）", "direct");
    fields["mode"] = mode;
    paths->addRow("起動方式", mode);
    connect(mode, &QComboBox::currentIndexChanged, this,
            [this] { change("mode", mode->currentData().toString()); });
    pathField(paths, "MPIランチャー", "launcher", false);
    pathField(paths, "ジョブ保存先", "job_root", true);
    pathField(paths, "参照ファイルの基準", "reference_directory", true);
    qobject_cast<QLineEdit *>(fields["reference_directory"])
        ->setPlaceholderText("空欄: プロジェクトの場所（未保存時は起動時の場所）");
    auto *pathHint = new QLabel(
        "実行ファイルは名前またはパスで指定します。MPIはmpirun/mpiexecの -np を使用します。"
        "入力movecsの相対パスは参照基準から解決します。scratch・permanentは新しいジョブ"
        "フォルダーが基準です。出力movecs・CUBE・MOLDENはpermanent（未指定ならジョブ）に保存します。"
        "出力movecsを./で始めるとジョブ内、絶対パスなら指定先に保存します。");
    pathHint->setWordWrap(true);
    paths->addRow(pathHint);
    auto *parallel = group("計算資源");
    integer(parallel, "MPI起動プロセス数", "mpi_ranks", 1, 4096);
    integer(parallel, "OpenMPスレッド数 / プロセス", "omp_threads", 1, 1024);
    integer(parallel, "BLASスレッド数 / プロセス", "blas_threads", 1, 1024);
    auto *stack = new QLineEdit;
    fields["omp_stacksize"] = stack;
    stack->setObjectName("execution_omp_stacksize");
    stack->setPlaceholderText("空欄: 起動環境を継承（例: 64M）");
    parallel->addRow("OpenMPスタック", stack);
    connect(stack, &QLineEdit::editingFinished, this, [this, stack] {
        if (stack->isModified())
            change("omp_stacksize", stack->text().trimmed());
        stack->setModified(false);
    });
    memory = new QSpinBox;
    memory->setObjectName("execution_memory");
    memory->setRange(64, 1048576);
    memory->setKeyboardTracking(false);
    parallel->addRow("メモリ / MPIプロセス（MB）", memory);
    connect(memory, &QSpinBox::valueChanged, this, [this](int n) {
        if (busy || window->hasCandidate() || window->worker)
            return;
        window->safe([&] {
            window->edit("NWChemメモリを変更", [&](Document &d) {
                auto s = d.data.value("nwchem").toObject();
                s["memory_mb"] = n;
                d.data["nwchem"] = s;
            });
        });
    });
    integer(parallel, "制限時間 / 秒（0: 制限なし）", "timeout_seconds", 0, 604800);
    resources = new QLabel;
    resources->setObjectName("executionResources");
    resources->setWordWrap(true);
    parallel->addRow(resources);
    auto *mpiHint = new QLabel("MPI-PR版では補助プロセスが必要です（1計算プロセスでも起動数2）。"
                               "起動数は自動補正しません。OpenMP/BLASの既定は各1です。"
                               "メモリは「3 計算条件」と共有し、実際の使用量の上限ではありません。");
    mpiHint->setWordWrap(true);
    parallel->addRow(mpiHint);
    auto *envForm = group("追加の環境変数");
    environment = new QTableWidget(0, 3);
    environment->setObjectName("executionEnvironment");
    environment->setHorizontalHeaderLabels({"有効", "名前", "値"});
    environment->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    environment->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    environment->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    environment->setMinimumHeight(130);
    environment->setMaximumHeight(200);
    envForm->addRow(environment);
    auto *envHint = new QLabel("有効な行だけを起動環境へ上書きします。例: NWCHEM_BASIS_LIBRARY。"
                               "スレッド数は上の専用欄で指定してください。値の環境変数展開は行いません。");
    envHint->setWordWrap(true);
    envForm->addRow(envHint);
    auto *buttons = new QHBoxLayout;
    auto *add = new QPushButton("追加");
    auto *remove = new QPushButton("選択行を削除");
    add->setObjectName("executionAddEnvironment");
    remove->setObjectName("executionRemoveEnvironment");
    buttons->addWidget(add);
    buttons->addWidget(remove);
    envForm->addRow(buttons);
    connect(add, &QPushButton::clicked, this, [this] {
        auto rows = executionSettings(window->document())["environment"].toArray();
        rows.append(QJsonObject{{"enabled", false}, {"name", ""}, {"value", ""}});
        change("environment", rows);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = environment->currentRow();
        auto rows = executionSettings(window->document())["environment"].toArray();
        if (row >= 0 && row < rows.size()) {
            rows.removeAt(row);
            change("environment", rows);
        }
    });
    // Defer table refresh until the delegate's current edit has completed.
    connect(environment, &QTableWidget::itemChanged, this, [this] {
        if (!busy) {
            environmentDirty = true;
            QTimer::singleShot(0, this, &ExecutionPanel::editEnvironment);
        }
    });
    forms->addStretch();
    split->addWidget(scroll);
    auto *right = new QWidget;
    auto *output = new QVBoxLayout(right);
    output->setContentsMargins(4, 0, 0, 0);
    diagnostics = new QPlainTextEdit;
    diagnostics->setObjectName("executionValidation");
    diagnostics->setReadOnly(true);
    diagnostics->setMaximumHeight(150);
    output->addWidget(diagnostics);
    auto *check = new QPushButton("実行環境を再確認");
    check->setObjectName("executionCheck");
    output->addWidget(check, 0, Qt::AlignRight);
    connect(check, &QPushButton::clicked, this, &ExecutionPanel::refresh);
    output->addWidget(new QLabel("起動コマンド例（ジョブ名の日時・IDは保存時に決定）"));
    command = new QPlainTextEdit;
    command->setObjectName("executionCommand");
    command->setReadOnly(true);
    command->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    command->setMaximumHeight(145);
    output->addWidget(command);
    auto *actions = new QHBoxLayout;
    prepare = new QPushButton("入力一式を保存");
    start = new QPushButton("計算を実行");
    stop = new QPushButton("停止");
    prepare->setObjectName("executionPrepare");
    start->setObjectName("executionStart");
    stop->setObjectName("executionStop");
    actions->addWidget(prepare);
    actions->addWidget(start);
    actions->addWidget(stop);
    output->addLayout(actions);
    connect(prepare, &QPushButton::clicked, this, [this] { window->safe([this] { saveBundle(); }); });
    connect(start, &QPushButton::clicked, this, [this] { window->safe([this] { startCalculation(); }); });
    connect(stop, &QPushButton::clicked, &run, &NwchemRun::stop);
    state = new QLabel;
    state->setObjectName("executionState");
    state->setWordWrap(true);
    state->setTextFormat(Qt::PlainText);
    location = new QLabel;
    location->setObjectName("executionLocation");
    location->setWordWrap(true);
    location->setTextFormat(Qt::PlainText);
    location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    output->addWidget(state);
    output->addWidget(location);
    auto *logs = new QHBoxLayout;
    logChannel = new QComboBox;
    logChannel->addItem("標準出力（末尾128 KiB）", "output.out");
    logChannel->addItem("標準エラー（末尾128 KiB）", "stderr.err");
    logs->addWidget(logChannel, 1);
    openFolder = new QPushButton("保存先を開く");
    openFolder->setObjectName("executionOpenFolder");
    logs->addWidget(openFolder);
    connect(openFolder, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(saved.directory)); });
    connect(logChannel, &QComboBox::currentIndexChanged, this, &ExecutionPanel::updateRun);
    output->addLayout(logs);
    log = new QPlainTextEdit;
    log->setObjectName("executionLog");
    log->setReadOnly(true);
    log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log->setLineWrapMode(QPlainTextEdit::NoWrap);
    output->addWidget(log, 1);
    split->addWidget(right);
    split->setSizes({620, 700});
    connect(&run, &NwchemRun::changed, this, [this] {
        if (run.state() != previousRunState) {
            previousRunState = run.state();
            refresh();
            emit bundleChanged(run.plan().directory);
        } else
            updateRun();
    });
    connect(window->history(), &QUndoStack::indexChanged, this, &ExecutionPanel::refresh);
}
QString ExecutionPanel::baseDirectory() const {
    return window->path.isEmpty() ? QDir::currentPath() : QFileInfo(window->path).absolutePath();
}
void ExecutionPanel::pathField(QFormLayout *layout, const QString &label, const QString &key,
                               bool directory) {
    auto *row = new QHBoxLayout;
    auto *line = new QLineEdit;
    line->setObjectName("execution_" + key);
    fields[key] = line;
    auto *browse = new QPushButton("参照…");
    row->addWidget(line, 1);
    row->addWidget(browse);
    layout->addRow(label, row);
    connect(line, &QLineEdit::editingFinished, this, [this, line, key] {
        if (line->isModified())
            change(key, line->text().trimmed());
        line->setModified(false);
    });
    connect(browse, &QPushButton::clicked, this, [this, line, key, directory] {
        const auto path = directory ? QFileDialog::getExistingDirectory(this, "ディレクトリ", line->text())
                                    : QFileDialog::getOpenFileName(this, "実行ファイル", line->text());
        if (!path.isEmpty())
            change(key, path);
    });
}
void ExecutionPanel::integer(QFormLayout *layout, const QString &label, const QString &key, int lo, int hi) {
    auto *spin = new QSpinBox;
    spin->setObjectName("execution_" + key);
    spin->setRange(lo, hi);
    spin->setKeyboardTracking(false);
    fields[key] = spin;
    layout->addRow(label, spin);
    connect(spin, &QSpinBox::valueChanged, this, [this, key](int value) { change(key, value); });
}
void ExecutionPanel::change(const QString &key, const QJsonValue &value) {
    if (busy || window->hasCandidate() || window->worker ||
        executionSettings(window->document()).value(key) == value)
        return;
    window->safe([&] {
        window->edit("NWChem実行設定を変更", [&](Document &d) { setExecutionSetting(d, key, value); });
    });
}
void ExecutionPanel::editEnvironment() {
    if (busy || !environmentDirty)
        return;
    environmentDirty = false;
    auto rows = executionSettings(window->document())["environment"].toArray();
    if (rows.size() != environment->rowCount())
        return;
    for (int row = 0; row < rows.size(); ++row) {
        auto item = rows[row].toObject(); // Preserve unknown per-row fields.
        item["enabled"] = environment->item(row, 0)->checkState() == Qt::Checked;
        item["name"] = environment->item(row, 1)->text().trimmed();
        item["value"] = environment->item(row, 2)->text();
        rows[row] = item;
    }
    change("environment", rows);
}
void ExecutionPanel::commitPendingEdits() {
    if (auto *focused = QApplication::focusWidget(); focused && isAncestorOf(focused))
        focused->clearFocus();
    editEnvironment();
}
void ExecutionPanel::saveBundle() {
    if (window->hasCandidate() || window->worker || run.active())
        fail("候補・整形・実行中は入力一式を保存できません。");
    commitPendingEdits();
    saved = saveExecutionBundle(window->document(), baseDirectory());
    updateRun();
    emit bundleChanged(saved.directory);
}
void ExecutionPanel::startCalculation() {
    if (window->hasCandidate() || window->worker || run.active())
        fail("候補・整形・実行中は新しい計算を開始できません。");
    commitPendingEdits();
    run.start(window->document(), baseDirectory());
}
void ExecutionPanel::refresh() {
    if (busy)
        return;
    QScopedValueRollback guard(busy, true);
    const auto &d = window->displayDocument();
    const auto s = executionSettings(d);
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const QSignalBlocker blocker(it.value());
        if (auto *line = qobject_cast<QLineEdit *>(it.value())) {
            if (line->text() != s[it.key()].toString())
                line->setText(s[it.key()].toString());
        } else if (auto *spin = qobject_cast<QSpinBox *>(it.value()))
            spin->setValue(s[it.key()].toInt());
        else if (auto *combo = qobject_cast<QComboBox *>(it.value())) {
            while (combo->count() > 2)
                combo->removeItem(2);
            int index = combo->findData(s[it.key()].toString());
            if (index < 0) {
                combo->addItem("未対応: " + s[it.key()].toVariant().toString(), s[it.key()]);
                index = 2;
            }
            combo->setCurrentIndex(index);
        }
    }
    {
        const QSignalBlocker blocker(memory);
        memory->setValue(nwchemSettings(d)["memory_mb"].toInt());
    }
    {
        const QSignalBlocker blocker(environment);
        environmentDirty = false;
        const auto rows = s["environment"].toArray();
        environment->setRowCount(rows.size());
        for (int row = 0; row < rows.size(); ++row) {
            const auto entry = rows[row].toObject();
            for (int column = 0; column < 3; ++column) {
                auto *item = environment->item(row, column);
                if (!item) {
                    item = new QTableWidgetItem;
                    environment->setItem(row, column, item);
                }
                if (column == 0) {
                    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
                    item->setCheckState(entry["enabled"].toBool() ? Qt::Checked : Qt::Unchecked);
                } else
                    item->setText(entry[column == 1 ? "name" : "value"].toString());
            }
        }
    }
    const bool editable = !window->hasCandidate() && !window->worker;
    form->setEnabled(editable);
    fields["launcher"]->setEnabled(s["mode"] == "mpi");
    fields["mpi_ranks"]->setEnabled(s["mode"] == "mpi");
    undo->setEnabled(editable && window->history()->canUndo());
    redo->setEnabled(editable && window->history()->canRedo());
    const int ranks = s["mode"] == "mpi" ? s["mpi_ranks"].toInt() : 1;
    resources->setText(QString("起動数 %1 × OpenMP %2 / BLAS %3　·　このPC: %4論理CPU\n"
                               "NWChemメモリ指定の合計目安: %5 MB（起動数 × 1プロセス分）")
                           .arg(ranks)
                           .arg(s["omp_threads"].toInt())
                           .arg(s["blas_threads"].toInt())
                           .arg(QThread::idealThreadCount())
                           .arg(qint64(ranks) * memory->value()));
    const auto report = validateExecution(d, baseDirectory(), true);
    QStringList messages;
    if (!editable)
        messages << "候補を確定・取消し、整形の終了後に実行できます。";
    for (const auto &e : report.errors)
        messages << "エラー: " + e;
    for (const auto &w : report.warnings)
        messages << "確認: " + w;
    if (report.errors.isEmpty())
        messages.prepend("✓ 実行ファイル・設定を確認しました。");
    bool portable = false;
    bool runnable = report.errors.isEmpty();
    try {
        const auto preview = executionPreviewPlan(d, baseDirectory());
        command->setPlainText(executionCommand(preview));
        portable = true;
        if (runnable) {
            try {
                executionPreviewPlan(d, baseDirectory(), true);
            } catch (const std::exception &e) {
                messages << "実行前の確認: " + QString::fromUtf8(e.what());
                runnable = false;
            }
        }
    } catch (const std::exception &e) {
        command->setPlainText("起動設定を作成できません:\n" + QString::fromUtf8(e.what()));
    }
    prepare->setEnabled(editable && !run.active() && portable);
    diagnostics->setPlainText(messages.join('\n'));
    start->setEnabled(editable && !run.active() && runnable && portable);
    stop->setEnabled(run.active() && run.state() != "stopping");
    updateRun();
}
void ExecutionPanel::updateRun() {
    if (run.active() || (!run.plan().directory.isEmpty() && saved.directory == run.plan().directory))
        saved = run.plan();
    else if (!run.plan().directory.isEmpty() && saved.directory.isEmpty())
        saved = run.plan();
    const bool showingRun = saved.directory == run.plan().directory && !saved.directory.isEmpty();
    const QMap<QString, QString> names{
        {"starting", "起動中"},
        {"running", "実行中"},
        {"stopping", "停止処理中"},
        {"cancelled", "停止しました"},
        {"timed_out", "制限時間で停止しました"},
        {"failed", "実行に失敗しました"},
        {"finished", "終了しました（終了コード0）。計算結果はログを確認してください。"}};
    state->setText(showingRun ? names.value(run.state()) + QString("　経過 %1 秒").arg(run.elapsedSeconds()) +
                                    (run.message().isEmpty() ? "" : "\n" + run.message())
                   : saved.directory.isEmpty() ? "未実行"
                                               : "入力一式を保存しました。");
    location->setText(saved.directory);
    openFolder->setEnabled(!saved.directory.isEmpty());
    viewResults->setEnabled(!saved.directory.isEmpty() && !resultFiles(saved.directory).isEmpty());
    stop->setEnabled(run.active() && run.state() != "stopping");
    if (run.active()) {
        start->setEnabled(false);
        prepare->setEnabled(false);
    }
    QString text;
    if (!saved.directory.isEmpty()) {
        QFile file(QDir(saved.directory).filePath(logChannel->currentData().toString()));
        if (file.open(QIODevice::ReadOnly)) {
            const bool truncated = file.size() > 128 * 1024;
            if (truncated)
                file.seek(file.size() - 128 * 1024);
            text = (truncated ? "…ログの末尾を表示しています。全文は保存先にあります。\n" : "") +
                   QString::fromUtf8(file.read(128 * 1024));
        }
    }
    if (text != displayedLog) {
        const bool atEnd = log->verticalScrollBar()->value() == log->verticalScrollBar()->maximum();
        const int position = log->verticalScrollBar()->value();
        displayedLog = text;
        log->setPlainText(text);
        log->verticalScrollBar()->setValue(atEnd ? log->verticalScrollBar()->maximum() : position);
    }
}
void ExecutionPanel::stopEditing() {
    busy = true;
    run.disconnect(this);
}
void ExecutionPanel::shutdown() {
    run.shutdown();
}
} // namespace mf
