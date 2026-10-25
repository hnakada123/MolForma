// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/execution_panel.h"
#include "nwchem/nwchem.h"
#include "ui/advanced_panel.h"
#include "ui/calculation_panel.h"
#include "ui/editor_chrome.h"
#include "ui/history_panel.h"
#include "ui/input_preview_panel.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include "ui/theme.h"
#include <QtWidgets>

namespace mf {
namespace {
QLabel *caption(const QString &text) {
    auto *label = new QLabel(text);
    label->setProperty("caption", true);
    label->setWordWrap(true);
    return label;
}
QLabel *muted(const QString &text = {}) {
    auto *label = new QLabel(text);
    label->setProperty("muted", true);
    label->setWordWrap(true);
    return label;
}
// A note beside a card title, shortened with "…" when the card is narrow.
QLabel *note(const QString &text) {
    auto *label = new ElidedLabel(text);
    label->setProperty("muted", true);
    label->setToolTip(text);
    return label;
}
void repolish(QWidget *w) {
    w->style()->unpolish(w);
    w->style()->polish(w);
}
// "18:31", or "1:02:03" past an hour.
QString clock(qint64 seconds) {
    return seconds >= 3600 ? QString("%1:%2:%3")
                                 .arg(seconds / 3600)
                                 .arg(seconds / 60 % 60, 2, 10, QChar('0'))
                                 .arg(seconds % 60, 2, 10, QChar('0'))
                           : QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}
// The logical CPUs of this PC as cells, filled for those the run would use (all in the error color
// when it would use more than there are).
class CpuMeter : public QWidget {
  public:
    int used = 0;
    QSize sizeHint() const override {
        return {240, 12};
    }
    QSize minimumSizeHint() const override {
        return {60, 12};
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        const int cpus = std::max(1, QThread::idealThreadCount());
        const int cells = std::min(cpus, 64); // Many CPUs: cells stand for shares of them.
        const ChromeColors c(palette());
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double gap = 3, w = (width() - gap * (cells - 1)) / cells;
        const int filled = used > cpus ? cells : int(std::ceil(double(used) * cells / cpus));
        for (int i = 0; i < cells; ++i) {
            p.setPen(Qt::NoPen);
            p.setBrush(i < filled ? (used > cpus ? c.bad : c.accent) : c.segment);
            p.drawRoundedRect(QRectF(i * (w + gap), 0, w, height()), 2, 2);
        }
    }
};
} // namespace

ExecutionPanel::ExecutionPanel(MainWindow *owner) : QWidget(owner), window(owner), run(this) {
    setObjectName("executionTab");
    setAttribute(Qt::WA_StyledBackground);
    run.setObjectName("nwchemRun");
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *split = new QSplitter;
    split->setChildrenCollapsible(false);
    outer->addWidget(split);

    // The settings of a run.
    auto *scroll = new QScrollArea;
    scroll->setObjectName("executionSettings");
    scroll->setProperty("canvas", true);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(420);
    form = new QWidget;
    form->setProperty("canvas", true);
    scroll->setWidget(form);
    auto *forms = new QVBoxLayout(form);
    forms->setContentsMargins(16, 16, 8, 16);
    forms->setSpacing(12);
    runningNote = new QLabel("計算の実行中も分子や条件を編集できます。起動済みの計算には反映されません。");
    runningNote->setObjectName("executionRunningNote");
    runningNote->setWordWrap(true);
    runningNote->hide();
    forms->addWidget(runningNote);
    auto *files = new SettingsCard("実行ファイル・保存先");
    files->setObjectName("executionFiles");
    files->header()->insertWidget(1, note("実行ごとに専用のジョブフォルダーを作ります"), 1);
    forms->addWidget(files);
    auto *paths = new QGridLayout;
    paths->setHorizontalSpacing(12);
    paths->setVerticalSpacing(6);
    paths->setColumnStretch(1, 1);
    files->body()->addLayout(paths);
    pathField(paths, "NWChem実行ファイル", "executable", false);
    mode = new QComboBox;
    mode->setObjectName("execution_mode");
    mode->addItem("MPIで起動", "mpi");
    mode->addItem("直接実行（対応する逐次版等）", "direct");
    fields["mode"] = mode;
    auto *modes = new ChoiceButtons(mode, ChoiceButtons::Segments, {"MPIで起動", "直接実行（逐次版など）"});
    choices << modes;
    {
        const int row = paths->rowCount();
        paths->addWidget(new QLabel("起動方式"), row, 0);
        paths->addWidget(modes, row, 1, 1, 2, Qt::AlignLeft);
    }
    connect(mode, &QComboBox::currentIndexChanged, this,
            [this] { change("mode", mode->currentData().toString()); });
    pathField(paths, "MPIランチャー", "launcher", false);
    pathField(paths, "ジョブ保存先", "job_root", true);
    pathField(paths, "参照ファイルの基準", "reference_directory", true);
    qobject_cast<QLineEdit *>(fields["reference_directory"])
        ->setPlaceholderText("空欄: プロジェクトの場所（未保存時は起動時の場所）");
    auto *pathsHelp = new QToolButton;
    pathsHelp->setObjectName("executionPathHelp");
    pathsHelp->setText("パスの扱いについて");
    pathsHelp->setCheckable(true);
    pathsHelp->setProperty("flat", true);
    pathsHelp->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pathsHelp->setArrowType(Qt::RightArrow);
    files->body()->addWidget(pathsHelp, 0, Qt::AlignLeft);
    auto *pathHint =
        muted("実行ファイルは名前またはパスで指定します。MPIはmpirun/mpiexecの -np を使用します。"
              "入力movecsの相対パスは参照基準から解決します。scratch・permanentは新しいジョブ"
              "フォルダーが基準です。出力movecs・CUBE・MOLDENはpermanent（未指定ならジョブ）に保存します。"
              "出力movecsを./で始めるとジョブ内、絶対パスなら指定先に保存します。");
    pathHint->hide();
    files->body()->addWidget(pathHint);
    connect(pathsHelp, &QToolButton::toggled, this, [pathsHelp, pathHint](bool on) {
        pathHint->setVisible(on);
        pathsHelp->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });

    auto *parallel = new SettingsCard("計算資源");
    parallel->setObjectName("executionResourcesCard");
    forms->addWidget(parallel);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    parallel->body()->addLayout(grid);
    grid->addWidget(integer("MPI起動プロセス数", "mpi_ranks", 1, 4096), 0, 0);
    grid->addWidget(integer("OpenMPスレッド / プロセス", "omp_threads", 1, 1024), 0, 1);
    grid->addWidget(integer("BLASスレッド / プロセス", "blas_threads", 1, 1024), 0, 2);
    auto *stackCell = new QWidget;
    auto *stackLayout = new QVBoxLayout(stackCell);
    stackLayout->setContentsMargins(0, 0, 0, 0);
    stackLayout->setSpacing(4);
    stackLayout->addWidget(caption("OpenMPスタック"));
    auto *stack = new QLineEdit;
    fields["omp_stacksize"] = stack;
    stack->setObjectName("execution_omp_stacksize");
    stack->setPlaceholderText("空欄: 継承（例: 64M）");
    stackLayout->addWidget(stack);
    grid->addWidget(stackCell, 1, 0);
    connect(stack, &QLineEdit::editingFinished, this, [this, stack] {
        if (stack->isModified())
            change("omp_stacksize", stack->text().trimmed());
        stack->setModified(false);
    });
    auto *memoryCell = new QWidget;
    auto *memoryLayout = new QVBoxLayout(memoryCell);
    memoryLayout->setContentsMargins(0, 0, 0, 0);
    memoryLayout->setSpacing(4);
    memoryLayout->addWidget(caption("メモリ / MPIプロセス"));
    memory = new QSpinBox;
    memory->setObjectName("execution_memory");
    memory->setRange(64, 1048576);
    memory->setSuffix(" MB");
    memory->setKeyboardTracking(false);
    memory->setToolTip("「3 計算条件」のメモリと同じ設定です。実際の使用量の上限ではありません。");
    memoryLayout->addWidget(memory);
    grid->addWidget(memoryCell, 1, 1);
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
    grid->addWidget(integer("制限時間（0: なし）", "timeout_seconds", 0, 604800, " 秒"), 1, 2);
    for (int column = 0; column < 3; ++column)
        grid->setColumnStretch(column, 1);
    // What the run takes of this PC.
    auto *usage = new QFrame;
    usage->setObjectName("executionUsage");
    auto *usageLayout = new QVBoxLayout(usage);
    usageLayout->setContentsMargins(12, 10, 12, 10);
    usageLayout->setSpacing(6);
    resources = new QLabel;
    resources->setObjectName("executionResources");
    resources->setWordWrap(true);
    usageLayout->addWidget(resources);
    auto *meter = new CpuMeter;
    meter->setObjectName("executionCpuMeter");
    cpuMeter = meter;
    usageLayout->addWidget(meter);
    memoryTotal = new QLabel;
    memoryTotal->setObjectName("executionMemoryTotal");
    memoryTotal->setWordWrap(true);
    usageLayout->addWidget(memoryTotal);
    parallel->body()->addWidget(usage);
    parallel->body()->addWidget(caption("MPI-PR版では補助プロセスが必要です（1計算プロセスでも起動数2）。"
                                        "起動数は自動補正しません。OpenMP/BLASの既定は各1です。"));

    auto *environmentCard = new SettingsCard("追加の環境変数");
    environmentCard->setObjectName("executionEnvironmentCard");
    forms->addWidget(environmentCard);
    auto *add = new QPushButton("追加");
    add->setIcon(lineIcon("plus"));
    removeEnvironment = new QPushButton("選択行を削除");
    add->setObjectName("executionAddEnvironment");
    removeEnvironment->setObjectName("executionRemoveEnvironment");
    add->setProperty("mcpExecutionSetting", true);
    removeEnvironment->setProperty("mcpExecutionSetting", true);
    environmentCard->header()->addWidget(removeEnvironment);
    environmentCard->header()->addWidget(add);
    environment = new QTableWidget(0, 3);
    environment->setObjectName("executionEnvironment");
    environment->setProperty("mcpExecutionSetting", true);
    environment->setHorizontalHeaderLabels({"有効", "名前", "値"});
    environment->verticalHeader()->hide();
    environment->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    environment->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    environment->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    environment->setMinimumHeight(110);
    environment->setMaximumHeight(200);
    environmentCard->body()->addWidget(environment);
    environmentEmpty = muted("なし。有効な行だけを起動環境へ上書きします（例: NWCHEM_BASIS_LIBRARY）。");
    environmentEmpty->setObjectName("executionEnvironmentEmpty");
    environmentCard->body()->addWidget(environmentEmpty);
    environmentCard->body()->addWidget(
        caption("スレッド数は「計算資源」で指定してください。値の環境変数展開は行いません。"));
    connect(add, &QPushButton::clicked, this, [this] {
        auto rows = executionSettings(window->document())["environment"].toArray();
        rows.append(QJsonObject{{"enabled", false}, {"name", ""}, {"value", ""}});
        change("environment", rows);
    });
    connect(removeEnvironment, &QPushButton::clicked, this, [this] {
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

    // The run, from its checks to its log.
    auto *rightScroll = new QScrollArea;
    rightScroll->setObjectName("executionRunScroll");
    rightScroll->setProperty("canvas", true);
    rightScroll->setWidgetResizable(true);
    rightScroll->setFrameShape(QFrame::NoFrame);
    auto *right = new QWidget;
    right->setObjectName("executionRun");
    right->setProperty("canvas", true);
    rightScroll->setWidget(right);
    auto *output = new QVBoxLayout(right);
    output->setContentsMargins(8, 16, 16, 16);
    output->setSpacing(12);
    checkCard = new SettingsCard("実行前の確認");
    checkCard->setObjectName("executionCheckCard");
    auto *check = new QPushButton("再確認");
    check->setObjectName("executionCheck");
    check->setIcon(lineIcon("refresh"));
    check->setToolTip("実行ファイル・保存先などを確認し直します。");
    checkCard->header()->addWidget(check);
    connect(check, &QPushButton::clicked, this, &ExecutionPanel::refresh);
    checkList = new QVBoxLayout;
    checkList->setSpacing(4);
    checkCard->body()->addLayout(checkList);
    diagnostics = new QLabel;
    diagnostics->setObjectName("executionValidation");
    diagnostics->setTextFormat(Qt::PlainText);
    diagnostics->setWordWrap(true);
    diagnostics->setTextInteractionFlags(Qt::TextSelectableByMouse);
    checkCard->body()->addWidget(diagnostics);
    output->addWidget(checkCard);

    commandCard = new SettingsCard("起動コマンド");
    commandCard->setObjectName("executionCommandCard");
    commandCard->header()->insertWidget(1, note("ジョブ名の日時・IDは保存時に決まります"), 1);
    auto *copyCommand = new QToolButton;
    copyCommand->setObjectName("executionCopyCommand");
    copyCommand->setIcon(lineIcon("copy"));
    copyCommand->setProperty("flat", true);
    copyCommand->setToolTip("起動コマンドをコピー");
    commandCard->header()->addWidget(copyCommand);
    command = new QPlainTextEdit;
    command->setObjectName("executionCommand");
    command->setReadOnly(true);
    command->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    command->setFixedHeight(command->fontMetrics().lineSpacing() * 5 + 14);
    commandCard->body()->addWidget(command);
    connect(copyCommand, &QToolButton::clicked, this, [this] {
        QApplication::clipboard()->setText(command->toPlainText());
        window->statusBar()->showMessage("起動コマンドをコピーしました。", 5000);
    });
    output->addWidget(commandCard);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    start = new QPushButton("計算を実行");
    prepare = new QPushButton("入力一式を保存");
    stop = new QPushButton("停止");
    start->setObjectName("executionStart");
    prepare->setObjectName("executionPrepare");
    stop->setObjectName("executionStop");
    start->setProperty("primary", true);
    stop->setProperty("danger", true);
    actions->addWidget(start);
    actions->addWidget(stop);
    actions->addWidget(prepare);
    actions->addStretch();
    output->addLayout(actions);
    connect(prepare, &QPushButton::clicked, this, [this] { window->safe([this] { saveBundle(); }); });
    connect(start, &QPushButton::clicked, this, [this] { window->safe([this] { startCalculation(); }); });
    connect(stop, &QPushButton::clicked, this, &ExecutionPanel::confirmStop);
    start->setToolTip("入力一式を新しいジョブフォルダーに保存し、NWChemを起動します（F5）。");
    prepare->setToolTip(
        "NWChemを起動せず、入力・プロジェクトをジョブフォルダーに保存します（別のPCでの実行用など）。");
    openResults = new QCheckBox("終了したら結果ビューアに読み込む");
    openResults->setObjectName("executionOpenResults");
    openResults->setToolTip(
        "計算が終わると、ジョブフォルダーを「結果ビューア」に読み込みます（タブは切り替えません）。"
        "終了はステータスバーとデスクトップ通知でも知らせます。");
    openResults->setChecked(window->settings.value("execution/open_results", true).toBool());
    connect(openResults, &QCheckBox::toggled, this,
            [this](bool on) { window->settings.setValue("execution/open_results", on); });
    actions->addWidget(openResults);

    // The current or last run: its state and folder, its tasks and the optimization criteria.
    runCard = new SettingsCard("実行状況");
    runCard->setObjectName("executionRunCard");
    statePill = new QLabel;
    statePill->setObjectName("executionStatePill");
    runCard->header()->insertWidget(1, statePill);
    timing = muted();
    timing->setObjectName("executionTiming");
    timing->setWordWrap(false);
    runCard->header()->insertWidget(2, timing);
    openFolder = new QPushButton("保存先を開く");
    openFolder->setObjectName("executionOpenFolder");
    openFolder->setIcon(lineIcon("folder"));
    viewResults = new QPushButton("結果ビューアへ");
    viewResults->setObjectName("executionResults");
    auto *history = new QPushButton("履歴で見る");
    history->setObjectName("executionHistory");
    for (auto *b : {openFolder, viewResults, history})
        runCard->header()->addWidget(b);
    connect(openFolder, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(saved.directory)); });
    connect(history, &QPushButton::clicked, this, [this] {
        window->workspaceTabs->setCurrentWidget(window->calculationHistory);
        if (!saved.directory.isEmpty())
            window->calculationHistory->selectDirectory(saved.directory);
    });
    connect(viewResults, &QPushButton::clicked, this, [this] {
        window->workspaceTabs->setCurrentWidget(window->results);
        window->results->openDirectory(saved.directory);
    });
    location = new QLabel;
    location->setObjectName("executionLocation");
    location->setProperty("mono", true);
    location->setProperty("muted", true);
    location->setWordWrap(true);
    location->setTextFormat(Qt::PlainText);
    location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    runCard->body()->addWidget(location);
    state = new QLabel;
    state->setObjectName("executionState");
    state->setWordWrap(true);
    state->setTextFormat(Qt::PlainText);
    runCard->body()->addWidget(state);
    stageList = new QVBoxLayout;
    stageList->setSpacing(4);
    runCard->body()->addLayout(stageList);
    criteria = new QWidget;
    criteria->setObjectName("executionCriteria");
    auto *tiles = new QHBoxLayout(criteria);
    tiles->setContentsMargins(0, 0, 0, 0);
    tiles->setSpacing(8);
    for (int i = 0; i < 4; ++i) {
        auto *tile = new QLabel;
        tile->setObjectName(QString("executionCriterion%1").arg(i));
        tile->setProperty("tile", true);
        tile->setTextFormat(Qt::RichText);
        tiles->addWidget(tile, 1);
        criterionLabels << tile;
    }
    runCard->body()->addWidget(criteria);
    output->addWidget(runCard);

    auto *logCard = new SettingsCard("ログ");
    logCard->setObjectName("executionLogCard");
    logChannel = new QComboBox;
    logChannel->setObjectName("executionLogChannel");
    logChannel->addItem("標準出力（末尾128 KiB）", "output.out");
    logChannel->addItem("標準エラー（末尾128 KiB）", "stderr.err");
    auto *channels = new ChoiceButtons(logChannel, ChoiceButtons::Segments, {"標準出力", "標準エラー"});
    choices << channels;
    logCard->header()->insertWidget(1, channels);
    logCard->header()->insertWidget(2, muted("末尾 128 KiB"));
    auto *copyLog = new QToolButton;
    copyLog->setObjectName("executionCopyLog");
    copyLog->setIcon(lineIcon("copy"));
    copyLog->setProperty("flat", true);
    copyLog->setToolTip("表示中のログをコピー");
    logCard->header()->addWidget(copyLog);
    connect(logChannel, &QComboBox::currentIndexChanged, this, &ExecutionPanel::updateRun);
    log = new QPlainTextEdit;
    log->setObjectName("executionLog");
    log->setReadOnly(true);
    log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log->setLineWrapMode(QPlainTextEdit::NoWrap);
    log->setPlaceholderText("まだ実行していません。計算を実行すると、ここにNWChemの出力を表示します。");
    log->setMinimumHeight(220);
    logCard->body()->addWidget(log, 1);
    connect(copyLog, &QToolButton::clicked, this,
            [this] { QApplication::clipboard()->setText(log->toPlainText()); });
    output->addWidget(logCard, 1);
    split->addWidget(rightScroll);
    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 6);
    split->setSizes({600, 720});

    runStatus = new QToolButton;
    runStatus->setObjectName("runStatusButton");
    runStatus->setAutoRaise(true);
    runStatus->setToolTip("「5 実行設定」を表示します。");
    runStatus->hide();
    window->statusBar()->addPermanentWidget(runStatus);
    connect(runStatus, &QToolButton::clicked, this,
            [this] { window->workspaceTabs->setCurrentWidget(this); });
    connect(&run, &NwchemRun::changed, this, [this] {
        if (run.state() != previousRunState) {
            const bool ended =
                QStringList{"starting", "running", "stopping"}.contains(previousRunState) && !run.active();
            previousRunState = run.state();
            refresh();
            emit bundleChanged(run.plan().directory);
            if (ended)
                runEnded();
        } else
            updateRun();
    });
}
QString ExecutionPanel::baseDirectory() const {
    return window->path.isEmpty() ? QDir::currentPath() : QFileInfo(window->path).absolutePath();
}
void ExecutionPanel::pathField(QGridLayout *grid, const QString &label, const QString &key, bool directory) {
    const int row = grid->rowCount();
    auto *line = new QLineEdit;
    line->setObjectName("execution_" + key);
    fields[key] = line;
    auto *browse = new QPushButton("参照…");
    browse->setObjectName("executionBrowse_" + key);
    // The program to run is chosen by a person; McpGui refuses to operate these controls.
    if (isProtectedExecutionKey(key)) {
        line->setProperty("mcpExecutionSetting", true);
        browse->setProperty("mcpExecutionSetting", true);
    }
    auto *name = new QLabel(label);
    name->setBuddy(line);
    grid->addWidget(name, row, 0);
    grid->addWidget(line, row, 1);
    grid->addWidget(browse, row, 2);
    if (!directory) { // Where the program was found, under its field.
        auto *where = new QLabel;
        where->setObjectName("executionFound_" + key);
        where->setTextFormat(Qt::PlainText);
        where->setWordWrap(true);
        found[key] = where;
        grid->addWidget(where, row + 1, 1, 1, 2);
    }
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
QWidget *ExecutionPanel::integer(const QString &label, const QString &key, int lo, int hi,
                                 const QString &unit) {
    auto *cell = new QWidget;
    auto *layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(caption(label));
    auto *spin = new QSpinBox;
    spin->setObjectName("execution_" + key);
    spin->setRange(lo, hi);
    spin->setSuffix(unit);
    spin->setKeyboardTracking(false);
    fields[key] = spin;
    layout->addWidget(spin);
    connect(spin, &QSpinBox::valueChanged, this, [this, key](int value) { change(key, value); });
    return cell;
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
    // A stale tab first catches up, dropping typing that an eager refresh would have replaced.
    window->refreshPanel(this);
    if (auto *focused = QApplication::focusWidget(); focused && isAncestorOf(focused))
        focused->clearFocus();
    editEnvironment();
}
void ExecutionPanel::saveBundle() {
    if (window->hasCandidate() || window->worker || run.active())
        fail("候補・整形・実行中は入力一式を保存できません。");
    commitPendingEdits();
    if (window->mcp.policy && window->mcp.depth > 0)
        window->mcp.policy->checkRun(window->document(), baseDirectory());
    saved = saveExecutionBundle(window->document(), baseDirectory());
    updateRun();
    emit bundleChanged(saved.directory);
}
void ExecutionPanel::startCalculation() {
    if (window->hasCandidate() || window->worker || run.active())
        fail("候補・整形・実行中は新しい計算を開始できません。");
    commitPendingEdits();
    auto &mcp = window->mcp;
    if (mcp.policy) {
        const bool remote = mcp.depth > 0;
        if (remote)
            mcp.policy->checkRun(window->document(), baseDirectory());
        const auto settings = protectedExecution(window->document());
        if (!mcp.policy->allowExecutionSettings && settings != mcp.trustedExecution) {
            if (remote)
                fail("NWChem実行ファイル・MPIランチャー・追加の環境変数・NWChemの追加入力が、"
                     "MolForma上で最後に設定した値と異なります（MCP経由の変更や"
                     "プロジェクトの読込による）。MCPからはこの設定で実行できません。"
                     "利用者が「3 計算条件」「5 実行設定」タブで設定を確認してください。");
            // A person started the run: confirm in a dialog that MCP cannot see or press.
            QStringList environment;
            for (const auto &row : settings["environment"].toArray())
                if (row.toObject()["enabled"].toBool())
                    environment << row.toObject()["name"].toString() + "=" +
                                       row.toObject()["value"].toString();
            QMessageBox box(QMessageBox::Warning, "実行設定の確認",
                            "実行ファイル・ランチャー・環境変数・NWChemの追加入力が、MCP接続中に画面の外から"
                            "変更されています。この設定で実行しますか？",
                            QMessageBox::Yes | QMessageBox::No, window);
            const bool extra = !settings["extra_input"].toString().trimmed().isEmpty() ||
                               !settings["extra_tasks"].toString().trimmed().isEmpty();
            box.setInformativeText("実行ファイル: " + settings["executable"].toString() +
                                   "\nランチャー: " + settings["launcher"].toString() + "\n環境変数: " +
                                   (environment.isEmpty() ? QString("なし") : environment.join(", ")) +
                                   "\nNWChemの追加入力: " +
                                   (extra ? QString("あり（「6 入力プレビュー」で確認）") : QString("なし")));
            box.setDefaultButton(QMessageBox::No);
            box.setProperty("mcpProtected", true);
            if (box.exec() != QMessageBox::Yes)
                return;
            mcp.trustedExecution = settings;
        }
    }
    run.start(window->document(), baseDirectory());
}
void ExecutionPanel::confirmStop() {
    if (!run.active()) {
        window->statusBar()->showMessage("実行中の計算はありません。", 5000);
        return;
    }
    if (QMessageBox::question(this, "計算の停止",
                              "実行中のNWChemを停止しますか？途中までのログと出力は保存先に残ります。",
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        run.stop();
}
void ExecutionPanel::startFromMenu() {
    window->refreshPanel(this, true); // The tab may be stale while another one is shown.
    if (!start->isEnabled()) {
        window->workspaceTabs->setCurrentWidget(this);
        window->statusBar()->showMessage(
            run.active() ? QString("計算を実行中です。")
                         : QString("計算を開始できません。「5 実行設定」の「実行前の確認」を見てください。"),
            10000);
        return;
    }
    startCalculation();
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
    for (auto *group : std::as_const(choices))
        group->sync();
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
        environment->setVisible(rows.size());
        removeEnvironment->setVisible(rows.size());
        environmentEmpty->setVisible(rows.isEmpty());
    }
    const bool editable = !window->hasCandidate() && !window->worker;
    form->setEnabled(editable);
    fields["launcher"]->setEnabled(s["mode"] == "mpi");
    fields["mpi_ranks"]->setEnabled(s["mode"] == "mpi");
    const int ranks = s["mode"] == "mpi" ? s["mpi_ranks"].toInt() : 1;
    const int threads = std::max(s["omp_threads"].toInt(), s["blas_threads"].toInt());
    const int cpus = QThread::idealThreadCount();
    static_cast<CpuMeter *>(cpuMeter)->used = ranks * threads;
    cpuMeter->update();
    resources->setText(QString("CPU  %1 / %2 論理CPU（起動数 %3 × OpenMP %4 / BLAS %5）")
                           .arg(ranks * threads)
                           .arg(cpus)
                           .arg(ranks)
                           .arg(s["omp_threads"].toInt())
                           .arg(s["blas_threads"].toInt()));
    resources->setProperty("bad", ranks * threads > cpus);
    repolish(resources);
    memoryTotal->setText(QString("NWChemメモリ合計の目安  %1 MB（%2 × %3 MB）")
                             .arg(qint64(ranks) * memory->value())
                             .arg(ranks)
                             .arg(memory->value()));
    const auto report = validateExecution(d, baseDirectory(), true);
    // The checks one per row, then the messages they do not cover.
    while (auto *item = checkList->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    QStringList covered;
    const ChromeColors colors(palette());
    for (const auto &c : report.checks) {
        auto *row = new QWidget;
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 2, 0, 2);
        layout->setSpacing(8);
        auto *icon = new QLabel;
        icon->setPixmap(
            lineIcon(c.ok ? "check" : "error", c.ok ? colors.good : colors.bad).pixmap(QSize(16, 16)));
        layout->addWidget(icon);
        auto *label = new QLabel(c.label);
        label->setMinimumWidth(130);
        layout->addWidget(label);
        auto *detail = new ElidedLabel(c.detail);
        detail->setToolTip(c.detail);
        detail->setProperty(c.ok ? "muted" : "bad", true);
        layout->addWidget(detail, 1);
        checkList->addWidget(row);
        if (!c.error.isEmpty())
            covered << c.error;
    }
    for (const auto &key : {"executable", "launcher"})
        for (const auto &c : report.checks)
            if (c.label == (QString(key) == "executable" ? "NWChem実行ファイル" : "MPIランチャー")) {
                found[key]->setText(c.ok ? "✓ " + c.detail : c.detail);
                found[key]->setProperty("good", c.ok);
                found[key]->setProperty("bad", !c.ok);
                repolish(found[key]);
            }
    found["launcher"]->setVisible(s["mode"] == "mpi");
    QStringList messages;
    if (!editable)
        messages << "候補を確定・取消し、整形の終了後に実行できます。";
    for (const auto &e : report.errors)
        if (!covered.contains(e))
            messages << "エラー: " + e;
    for (const auto &w : report.warnings)
        messages << "確認: " + w;
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
    diagnostics->setText(messages.join('\n'));
    diagnostics->setVisible(!messages.isEmpty());
    diagnostics->setProperty("bad", !runnable || !editable);
    repolish(diagnostics);
    prepare->setEnabled(editable && !run.active() && portable);
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
    const QMap<QString, QString> names{{"starting", "起動中"},
                                       {"running", "実行中"},
                                       {"stopping", "停止処理中"},
                                       {"cancelled", "停止しました"},
                                       {"timed_out", "制限時間で停止しました"},
                                       {"failed", "実行に失敗しました"},
                                       {"finished", "終了しました（終了コード0）"}};
    const QString outcome = run.state() != "finished" ? QString()
                            : !resultSummary.isEmpty()
                                ? "\n結果: " + resultSummary + "（「結果ビューア」に読み込みました）"
                                : QString("\n結果は「結果ビューアへ」で確認できます。");
    const auto stateText =
        showingRun
            ? names.value(run.state()) + outcome + (run.message().isEmpty() ? "" : "\n" + run.message())
        : saved.directory.isEmpty() ? QString("未実行")
                                    : QString("入力一式を保存しました（NWChemは起動していません）。");
    if (state->text() != stateText)
        state->setText(stateText);
    location->setText(saved.directory);
    runCard->setVisible(!saved.directory.isEmpty());
    const QMap<QString, QString> pills{
        {"starting", "起動中"},    {"running", "実行中"}, {"stopping", "停止処理中"}, {"cancelled", "停止"},
        {"timed_out", "時間制限"}, {"failed", "失敗"},    {"finished", "終了"}};
    const auto kind = !showingRun                 ? QString("saved")
                      : run.active()              ? QString("running")
                      : run.state() == "finished" ? QString("finished")
                                                  : QString("failed");
    statePill->setText(showingRun ? pills.value(run.state(), run.state()) : QString("保存のみ"));
    if (statePill->property("state").toString() != kind) {
        statePill->setProperty("state", kind);
        repolish(statePill);
    }
    const qint64 seconds = run.elapsedSeconds();
    timing->setText(!showingRun ? QString()
                    : run.active()
                        ? QString("経過 %1 · %2 開始")
                              .arg(clock(seconds))
                              .arg(QDateTime::currentDateTime().addSecs(-seconds).toString("HH:mm"))
                        : QString("所要 %1").arg(clock(seconds)));
    openFolder->setEnabled(!saved.directory.isEmpty());
    viewResults->setEnabled(!saved.directory.isEmpty() && !resultFiles(saved.directory).isEmpty());
    stop->setEnabled(run.active() && run.state() != "stopping");
    stop->setVisible(run.active());
    prepare->setVisible(!run.active());
    // While NWChem runs, its progress and log take the room of the checks before a run.
    checkCard->setVisible(!run.active());
    commandCard->setVisible(!run.active());
    runningNote->setVisible(run.active());
    state->setVisible(!run.active());
    if (window->stopAction)
        window->stopAction->setEnabled(stop->isEnabled());
    if (run.active()) {
        start->setEnabled(false);
        prepare->setEnabled(false);
    }
    window->showRun(run.active(), "実行中 " + clock(seconds), stop->isEnabled());
    // Always visible in the status bar, whatever the tab.
    if (!run.plan().directory.isEmpty()) {
        const QMap<QString, QString> brief{{"starting", "NWChemを起動中"},
                                           {"running", "NWChem実行中"},
                                           {"stopping", "NWChemを停止中"},
                                           {"cancelled", "最後の計算: 停止"},
                                           {"timed_out", "最後の計算: 時間制限"},
                                           {"failed", "最後の計算: 失敗"},
                                           {"finished", "最後の計算: 終了（コード0）"}};
        runStatus->setText(
            QString("%1%2 · %3")
                .arg(run.active() ? "▶ " : "", brief.value(run.state(), run.state()))
                .arg(run.active() ? clock(seconds) : QFileInfo(run.plan().directory).fileName()));
        runStatus->show();
    }
    showProgress(showingRun);
    appendLog();
}
void ExecutionPanel::readProgress() {
    if (saved.directory != progressDirectory) {
        progress = NwchemProgress();
        progressDirectory = saved.directory;
        progressOffset = 0;
        progressRest.clear();
        QFile input(QDir(saved.directory).filePath("input.nw"));
        const auto text = input.open(QIODevice::ReadOnly) ? QString::fromUtf8(input.readAll()) : QString();
        plannedTasks = NwchemProgress::tasks(text);
        maxSteps = NwchemProgress::driverSteps(text);
    }
    if (saved.directory.isEmpty())
        return;
    QFile file(QDir(saved.directory).filePath("output.out"));
    if (!file.open(QIODevice::ReadOnly) || file.size() == progressOffset)
        return;
    if (file.size() < progressOffset) { // A rewritten log: read it again.
        progress = NwchemProgress();
        progressOffset = 0;
        progressRest.clear();
    }
    file.seek(progressOffset);
    const auto chunk = file.read(std::min<qint64>(file.size() - progressOffset, 8 << 20));
    progressOffset += chunk.size();
    progressRest += chunk;
    const int end = progressRest.lastIndexOf('\n');
    if (end < 0)
        return;
    for (const auto &line : progressRest.left(end).split('\n'))
        progress.add(QString::fromUtf8(line));
    progressRest.remove(0, end + 1);
}
void ExecutionPanel::showProgress(bool showingRun) {
    // The tasks of the run with their state, and the criteria of a running optimization.
    QStringList stages;
    const auto showStages = [&] {
        // Rows in place while their number stays (the view updates four times a second).
        if (stageList->count() != stages.size())
            while (auto *item = stageList->takeAt(0)) {
                delete item->widget();
                delete item;
            }
        for (int i = 0; i < stages.size(); ++i) {
            auto *row =
                stageList->itemAt(i) ? qobject_cast<QLabel *>(stageList->itemAt(i)->widget()) : nullptr;
            if (!row) {
                row = new QLabel;
                row->setObjectName(QString("executionStage%1").arg(i));
                row->setTextFormat(Qt::RichText);
                stageList->addWidget(row);
            }
            if (row->text() != stages[i])
                row->setText(stages[i]);
        }
    };
    if (!showingRun || run.state() == "starting") {
        criteria->hide();
        showStages();
        return;
    }
    readProgress();
    const ChromeColors c(palette());
    const bool finished = run.state() == "finished" && !run.active();
    const int current = std::clamp(progress.started - 1, 0, std::max(0, int(plannedTasks.size()) - 1));
    const auto number = [](double value, int digits) {
        return QString::number(value, 'f', digits).replace('-', "−");
    };
    for (int i = 0; i < plannedTasks.size(); ++i) {
        const bool done =
            finished || i < current || (i == current && progress.convergedTask == i && !run.active());
        const bool now = run.active() && i == current;
        const QString status = done ? "完了" : now ? "実行中" : run.active() ? "待機" : "未完了";
        const QColor color = done ? c.good : now ? c.accentText : c.muted;
        QString detail;
        if (progress.stepTask == i && !progress.steps.isEmpty()) {
            const auto &last = progress.steps.last();
            detail = QString("ステップ %1%2 · E = %3 Hartree · ΔE %4")
                         .arg(last.step)
                         .arg(maxSteps ? QString(" / 最大 %1").arg(maxSteps) : QString())
                         .arg(number(last.energy, 6))
                         .arg(QString::number(last.deltaE, 'e', 1).replace('-', "−"));
            if (progress.convergedTask == i)
                detail += " · 収束";
        } else if (now && std::isfinite(progress.energy))
            detail = QString("E = %1 Hartree").arg(number(progress.energy, 6));
        stages << QString("<b>%1　%2</b>　<span style=\"color:%3\">%4</span>%5")
                      .arg(i + 1)
                      .arg(plannedTasks[i].toHtmlEscaped(), color.name(), status)
                      .arg(detail.isEmpty() ? QString()
                                            : QString("<br><span style=\"color:%1\">%2</span>")
                                                  .arg(c.muted.name(), detail.toHtmlEscaped()));
    }
    showStages();
    // The four DRIVER criteria of the last step against their thresholds.
    const bool optimizing = progress.stepTask >= 0 && !progress.steps.isEmpty() &&
                            progress.stepTask == current && !progress.thresholds.isEmpty();
    criteria->setVisible(optimizing);
    if (!optimizing)
        return;
    const auto &last = progress.steps.last();
    const QList<std::tuple<QString, QString, double>> values{{"Gmax", "gmax", last.gmax},
                                                             {"Grms", "grms", last.grms},
                                                             {"Xmax", "xmax", last.xmax},
                                                             {"Xrms", "xrms", last.xrms}};
    for (int i = 0; i < 4; ++i) {
        const auto &[name, key, value] = values[i];
        const double target = progress.thresholds.value(key).toDouble();
        // At step 0 the displacements are not measured yet.
        const bool measured = key.startsWith('g') || last.step > 0;
        const bool met = measured && value <= target;
        criterionLabels[i]->setText(QString("<b>%1</b>　<span style=\"color:%2\">%3</span><br><span "
                                            "style=\"font-family:monospace\">%4</span>"
                                            "<br><span style=\"color:%5\">目標 %6</span>")
                                        .arg(name, met ? c.good.name() : c.muted.name(),
                                             met        ? "達成"
                                             : measured ? "未達"
                                                        : "—")
                                        .arg(measured ? QString::number(value, 'f', 5) : QString("—"),
                                             c.muted.name(), QString::number(target, 'f', 5)));
    }
}
void ExecutionPanel::appendLog() {
    constexpr qint64 tail = 128 * 1024;
    const QString path = saved.directory.isEmpty()
                             ? QString()
                             : QDir(saved.directory).filePath(logChannel->currentData().toString());
    QFile file(path);
    const bool readable = !path.isEmpty() && file.open(QIODevice::ReadOnly);
    if (path != logPath || !readable || file.size() < logOffset) {
        // Another job, channel or a rewritten file: start again from its last 128 KiB.
        logPath = path;
        logOffset = -1;
        logDecoder = QStringDecoder(QStringDecoder::Utf8);
        log->clear();
    }
    if (!readable)
        return;
    auto *bar = log->verticalScrollBar();
    const bool atEnd = bar->value() == bar->maximum();
    QString added;
    if (logOffset < 0) {
        const bool truncated = file.size() > tail;
        if (truncated)
            file.seek(file.size() - tail);
        added = (truncated ? "…ログの末尾を表示しています。全文は保存先にあります。\n" : "") +
                QString(logDecoder.decode(file.read(tail)));
        logOffset = file.size();
    } else if (file.size() > logOffset) {
        file.seek(logOffset);
        const auto chunk = file.read(std::min<qint64>(file.size() - logOffset, 4 * tail));
        logOffset += chunk.size();
        added = logDecoder.decode(chunk);
    }
    if (added.isEmpty())
        return;
    // Insert at the end without touching the user's selection, then keep at most 128 Ki characters.
    QTextCursor cursor(log->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(added);
    if (log->document()->characterCount() > tail) {
        QTextCursor head(log->document());
        head.setPosition(int(log->document()->characterCount() - tail));
        head.movePosition(QTextCursor::EndOfBlock);
        head.setPosition(0, QTextCursor::KeepAnchor);
        head.removeSelectedText();
    }
    if (atEnd)
        bar->setValue(bar->maximum());
}
void ExecutionPanel::runEnded() {
    const QMap<QString, QString> names{{"finished", "計算が終了しました（終了コード0）"},
                                       {"failed", "計算が失敗しました"},
                                       {"cancelled", "計算を停止しました"},
                                       {"timed_out", "計算が制限時間で停止しました"}};
    const auto directory = run.plan().directory;
    const auto message =
        names.value(run.state(), "計算が終了しました") + ": " + QFileInfo(directory).fileName();
    window->statusBar()->showMessage(message, 60000);
    QApplication::alert(window); // Taskbar attention while MolForma is in the background.
    if (!window->isActiveWindow() && QSystemTrayIcon::isSystemTrayAvailable() &&
        QSystemTrayIcon::supportsMessages()) {
        if (!tray) {
            const auto icon = QApplication::windowIcon();
            tray = new QSystemTrayIcon(icon.isNull() ? style()->standardIcon(QStyle::SP_ComputerIcon) : icon,
                                       this);
            connect(tray, &QSystemTrayIcon::messageClicked, window, [this] {
                window->showNormal();
                window->raise();
                window->activateWindow();
            });
        }
        tray->show();
        tray->showMessage("MolForma", message, QSystemTrayIcon::Information, 10000);
        QTimer::singleShot(15000, tray, &QSystemTrayIcon::hide);
    }
    resultSummary.clear();
    if (openResults->isChecked() && run.state() == "finished" && !resultFiles(directory).isEmpty() &&
        !window->results->loading()) {
        summaryDirectory = directory;
        connect(window->results, &ResultsPanel::loaded, this, &ExecutionPanel::showResultSummary,
                Qt::UniqueConnection);
        window->results->openDirectory(directory);
    }
}
void ExecutionPanel::showResultSummary() {
    // Only the log of the run that just ended, not files the user opens later.
    const auto source = window->results->result().source;
    if (summaryDirectory.isEmpty() ||
        QFileInfo(source).absolutePath() != QFileInfo(summaryDirectory).absoluteFilePath())
        return;
    summaryDirectory.clear();
    resultSummary = window->results->briefSummary();
    updateRun();
    if (!resultSummary.isEmpty())
        window->statusBar()->showMessage("計算が終了しました: " + resultSummary, 60000);
}
void ExecutionPanel::stopEditing() {
    busy = true;
    run.disconnect(this);
}
void ExecutionPanel::shutdown() {
    run.shutdown();
}
void ExecutionPanel::applyChromeStyle() {
    const auto pal = static_cast<QWidget *>(window)->palette();
    setStyleSheet(chromeRules(settingsRules() + R"(
        #executionTab { background: @window; }
        #executionUsage { background: @window; border: 1px solid @line; border-radius: 8px; }
        QPlainTextEdit#executionCommand, QPlainTextEdit#executionLog { background: @window; border: 1px solid @line;
                                                                        border-radius: 6px; }
        QLabel#executionStatePill { border-radius: 10px; padding: 2px 9px; font-weight: bold; }
        QLabel#executionStatePill[state="running"] { background: @tint; color: @accentText; }
        QLabel#executionStatePill[state="finished"] { background: @goodTint; color: @good; }
        QLabel#executionStatePill[state="failed"] { background: @badTint; color: @error; }
        QLabel#executionStatePill[state="saved"] { background: @segment; color: @muted; }
        QLabel#executionRunningNote { background: @tint; color: @accentText; border-radius: 8px; padding: 8px 12px; }
        QLabel[tile="true"] { background: @window; border: 1px solid @line; border-radius: 8px; padding: 6px 9px; }
    )",
                              pal));
}
} // namespace mf
