// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/execution_panel.h"
#include "nwchem/nwchem.h"
#include "ui/calculation_panel.h"
#include "ui/editor_chrome.h"
#include "ui/history_panel.h"
#include "ui/input_preview_panel.h"
#include "ui/main_window.h"
#include "ui/results_panel.h"
#include "ui/run_progress.h"
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
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // The calculation, saving it as a job and what to do when a run ends. Runs start from the
    // header's "計算を実行" (F5) and stop from its "停止", whatever the tab.
    toolbar = new QFrame;
    toolbar->setObjectName("executionToolbar");
    toolbar->setProperty("toolbar", true);
    auto *tools = new FlowLayout(toolbar, 10, 6);
    tools->setContentsMargins(12, 8, 12, 8);
    outer->addWidget(toolbar);
    auto *target = new QWidget;
    auto *targetLayout = new QHBoxLayout(target);
    targetLayout->setContentsMargins(0, 0, 0, 0);
    targetLayout->addWidget(caption(tr("対象の計算")));
    context = new QLabel;
    context->setObjectName("executionContext");
    context->setTextFormat(Qt::PlainText);
    targetLayout->addWidget(context);
    tools->addWidget(target);
    auto *toolGap = new QWidget;
    toolGap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tools->addWidget(toolGap);
    openResults = new QCheckBox(tr("終了したら結果ビューアに読み込む"));
    openResults->setObjectName("executionOpenResults");
    openResults->setToolTip(
        tr("計算が終わると、ジョブフォルダーを「結果ビューア」に読み込みます（タブは切り替えません）。"
           "終了はステータスバーとデスクトップ通知でも知らせます。"));
    openResults->setChecked(window->settings.value("execution/open_results", true).toBool());
    connect(openResults, &QCheckBox::toggled, this,
            [this](bool on) { window->settings.setValue("execution/open_results", on); });
    tools->addWidget(openResults);
    prepare = new QPushButton(tr("ジョブとして保存"));
    prepare->setObjectName("executionPrepare");
    prepare->setIcon(lineIcon("apply"));
    prepare->setToolTip(tr("NWChemを起動せず、入力とプロジェクトを新しいジョブフォルダーに保存します"
                           "（別のPCで実行するときなど）。"));
    tools->addWidget(prepare);
    connect(prepare, &QPushButton::clicked, this, [this] { window->safe([this] { saveBundle(); }); });
    auto *split = new QSplitter;
    split->setChildrenCollapsible(false);
    outer->addWidget(split, 1);

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
    runningNote =
        new QLabel(tr("計算の実行中も分子や条件を編集できます。起動済みの計算には反映されません。"));
    runningNote->setObjectName("executionRunningNote");
    runningNote->setWordWrap(true);
    runningNote->hide();
    forms->addWidget(runningNote);
    auto *files = new SettingsCard(tr("実行ファイル・保存先"));
    files->setObjectName("executionFiles");
    files->header()->insertWidget(1, note(tr("実行ごとに専用のジョブフォルダーを作ります")), 1);
    forms->addWidget(files);
    auto *paths = new QGridLayout;
    paths->setHorizontalSpacing(12);
    paths->setVerticalSpacing(6);
    paths->setColumnStretch(1, 1);
    files->body()->addLayout(paths);
    pathField(paths, tr("NWChem実行ファイル"), "executable", false);
    mode = new QComboBox;
    mode->setObjectName("execution_mode");
    mode->addItem(tr("MPIで起動"), "mpi");
    mode->addItem(tr("直接実行（対応する逐次版等）"), "direct");
    fields["mode"] = mode;
    auto *modes =
        new ChoiceButtons(mode, ChoiceButtons::Segments, {tr("MPIで起動"), tr("直接実行（逐次版など）")});
    choices << modes;
    {
        const int row = paths->rowCount();
        paths->addWidget(new QLabel(tr("起動方式")), row, 0);
        paths->addWidget(modes, row, 1, 1, 2, Qt::AlignLeft);
    }
    connect(mode, &QComboBox::currentIndexChanged, this,
            [this] { change("mode", mode->currentData().toString()); });
    pathField(paths, tr("MPIランチャー"), "launcher", false);
    pathField(paths, tr("ジョブ保存先"), "job_root", true);
    pathField(paths, tr("参照ファイルの基準"), "reference_directory", true);
    qobject_cast<QLineEdit *>(fields["reference_directory"])
        ->setPlaceholderText(tr("空欄: プロジェクトの場所（未保存時は起動時の場所）"));
    auto *pathsHelp = new Disclosure(tr("パスの扱いについて"));
    pathsHelp->button()->setObjectName("executionPathHelp");
    files->body()->addWidget(pathsHelp, 0, Qt::AlignLeft);
    auto *pathHint =
        muted(tr("実行ファイルは名前またはパスで指定します。MPIはmpirun/mpiexecの -np を使用します。"
                 "入力movecsの相対パスは参照基準から解決します。scratch・permanentは新しいジョブ"
                 "フォルダーが基準です。出力movecs・CUBE・MOLDENはpermanent（未指定ならジョブ）に保存します。"
                 "出力movecsを./で始めるとジョブ内、絶対パスなら指定先に保存します。"));
    pathHint->hide();
    files->body()->addWidget(pathHint);
    connect(pathsHelp->button(), &QToolButton::toggled, pathHint, &QWidget::setVisible);

    auto *parallel = new SettingsCard(tr("計算資源"));
    parallel->setObjectName("executionResourcesCard");
    forms->addWidget(parallel);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    parallel->body()->addLayout(grid);
    grid->addWidget(integer(tr("MPI起動プロセス数"), "mpi_ranks", 1, 4096), 0, 0);
    // Short names over the narrow columns; the full ones in their tooltips.
    auto *omp = integer(tr("OpenMP / プロセス"), "omp_threads", 1, 1024);
    omp->setToolTip(tr("1プロセスあたりのOpenMPスレッド数（OMP_NUM_THREADS）"));
    grid->addWidget(omp, 0, 1);
    auto *blas = integer(tr("BLAS / プロセス"), "blas_threads", 1, 1024);
    blas->setToolTip(tr("1プロセスあたりのBLASスレッド数（OPENBLAS_NUM_THREADS・MKL_NUM_THREADS など）"));
    grid->addWidget(blas, 0, 2);
    auto *stackCell = new QWidget;
    auto *stackLayout = new QVBoxLayout(stackCell);
    stackLayout->setContentsMargins(0, 0, 0, 0);
    stackLayout->setSpacing(4);
    stackLayout->addWidget(caption(tr("OpenMPスタック")));
    auto *stack = new QLineEdit;
    fields["omp_stacksize"] = stack;
    stack->setObjectName("execution_omp_stacksize");
    stack->setPlaceholderText(tr("空欄: 継承（例: 64M）"));
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
    memoryLayout->addWidget(caption(tr("メモリ / MPIプロセス")));
    memory = new QSpinBox;
    memory->setObjectName("execution_memory");
    memory->setRange(64, 1048576);
    memory->setSuffix(" MB");
    memory->setKeyboardTracking(false);
    memory->setToolTip(tr("NWChemの memory 指定です（「2 計算条件」と「4 入力プレビュー」の要約にも表示）。"
                          "実際の使用量の上限ではありません。"));
    memoryLayout->addWidget(memory);
    grid->addWidget(memoryCell, 1, 1);
    connect(memory, &QSpinBox::valueChanged, this, [this](int n) {
        if (busy || window->hasCandidate() || window->worker)
            return;
        window->safe([&] {
            window->edit(tr("NWChemメモリを変更"), [&](Document &d) {
                auto s = d.data.value("nwchem").toObject();
                s["memory_mb"] = n;
                d.data["nwchem"] = s;
            });
        });
    });
    grid->addWidget(integer(tr("制限時間（0: なし）"), "timeout_seconds", 0, 604800, tr(" 秒")), 1, 2);
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
    parallel->body()->addWidget(caption(tr("MPI-PR版では補助プロセスが必要です（1計算プロセスでも起動数2）。"
                                           "起動数は自動補正しません。OpenMP/BLASの既定は各1です。")));

    auto *environmentCard = new SettingsCard(tr("追加の環境変数"));
    environmentCard->setObjectName("executionEnvironmentCard");
    forms->addWidget(environmentCard);
    auto *add = new QPushButton(tr("追加"));
    add->setIcon(lineIcon("plus"));
    removeEnvironment = new QPushButton(tr("選択行を削除"));
    add->setObjectName("executionAddEnvironment");
    removeEnvironment->setObjectName("executionRemoveEnvironment");
    add->setProperty("mcpExecutionSetting", true);
    removeEnvironment->setProperty("mcpExecutionSetting", true);
    environmentCard->header()->addWidget(removeEnvironment);
    environmentCard->header()->addWidget(add);
    environment = new QTableWidget(0, 3);
    environment->setObjectName("executionEnvironment");
    environment->setProperty("mcpExecutionSetting", true);
    environment->setHorizontalHeaderLabels({tr("有効"), tr("名前"), tr("値")});
    environment->verticalHeader()->hide();
    environment->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    environment->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    environment->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    environment->setMinimumHeight(110);
    environment->setMaximumHeight(200);
    environmentCard->body()->addWidget(environment);
    environmentEmpty = muted(tr("なし。有効な行だけを起動環境へ上書きします（例: NWCHEM_BASIS_LIBRARY）。"));
    environmentEmpty->setObjectName("executionEnvironmentEmpty");
    environmentCard->body()->addWidget(environmentEmpty);
    environmentCard->body()->addWidget(
        caption(tr("スレッド数は「計算資源」で指定してください。値の環境変数展開は行いません。")));
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
    checkCard = new StatusCard;
    checkCard->setObjectName("executionCheckCard");
    checkCard->headline()->setObjectName("executionCheckStatus");
    auto *check = checkCard->addRecheck("executionCheck", tr("実行ファイル・保存先などを確認し直します。"));
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
    startHint = caption(tr("ヘッダーの「計算を実行」（F5）で、入力一式を新しいジョブフォルダーに保存して"
                           "NWChemを起動します。"));
    startHint->setObjectName("executionStartHint");
    checkCard->body()->addWidget(startHint);
    output->addWidget(checkCard);

    commandCard = new SettingsCard(tr("起動コマンド"));
    commandCard->setObjectName("executionCommandCard");
    commandCard->header()->insertWidget(1, note(tr("ジョブ名の日時・IDは保存時に決まります")), 1);
    auto *copyCommand = copyButton("executionCopyCommand", tr("起動コマンドをコピー"));
    commandCard->header()->addWidget(copyCommand);
    command = logView("executionCommand");
    command->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    command->setFixedHeight(command->fontMetrics().lineSpacing() * 5 + 14);
    commandCard->body()->addWidget(command);
    connect(copyCommand, &QToolButton::clicked, this, [this] {
        QApplication::clipboard()->setText(command->toPlainText());
        window->statusBar()->showMessage(tr("起動コマンドをコピーしました。"), 5000);
    });
    output->addWidget(commandCard);
    // As tall as their contents; the room left is below them.
    for (QWidget *card : {static_cast<QWidget *>(checkCard), static_cast<QWidget *>(commandCard)})
        card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    // The current or last run is in the run panel, which any tab can open.
    showRunLink = linkButton(tr("実行状況とログを表示"), "executionShowRun");
    showRunLink->setToolTip(tr("最後に保存・実行したジョブの状態・進み具合・ログを表示します。"));
    connect(showRunLink, &QPushButton::clicked, this, [this] { showRunPanel(); });
    auto *showRunRow = new QHBoxLayout;
    showRunRow->addStretch();
    showRunRow->addWidget(showRunLink);
    output->addLayout(showRunRow);
    output->addStretch();
    buildRunPanel();
    split->addWidget(rightScroll);
    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 6);
    split->setSizes({600, 720});

    runStatus = new QToolButton;
    runStatus->setObjectName("runStatusButton");
    runStatus->setAutoRaise(true);
    runStatus->setToolTip(tr("実行状況とログを表示します。"));
    runStatus->hide();
    window->statusBar()->addPermanentWidget(runStatus);
    connect(runStatus, &QToolButton::clicked, this, [this] { showRunPanel(); });
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
void ExecutionPanel::buildRunPanel() {
    // Over the tabs at the right of the window, whatever the tab: its title and close, the state of
    // the run with its folder and progress, and its log.
    runPanel = new QFrame(window);
    runPanel->setObjectName("runPanel");
    runPanel->setAttribute(Qt::WA_StyledBackground);
    runPanel->hide();
    auto *panel = new QVBoxLayout(runPanel);
    panel->setContentsMargins(14, 12, 14, 14);
    panel->setSpacing(12);
    auto *head = new QHBoxLayout;
    auto *title = new QLabel(tr("実行状況とログ")); // The name of every way to open it.
    title->setObjectName("runPanelTitle");
    title->setProperty("cardTitle", true);
    head->addWidget(title);
    head->addStretch();
    auto *close = new QToolButton;
    close->setObjectName("runPanelClose");
    close->setIcon(lineIcon("close"));
    close->setProperty("flat", true);
    close->setToolTip(tr("閉じる（実行は続きます）"));
    close->setAccessibleName(tr("実行状況を閉じる"));
    head->addWidget(close);
    panel->addLayout(head);
    connect(close, &QToolButton::clicked, this, [this] { showRunPanel(false); });
    runCard = new SettingsCard(tr("実行状況"));
    runCard->setObjectName("executionRunCard");
    statePill = new QLabel;
    statePill->setObjectName("executionStatePill");
    runCard->header()->insertWidget(1, statePill);
    timing = muted();
    timing->setObjectName("executionTiming");
    timing->setWordWrap(false);
    runCard->header()->insertWidget(2, timing);
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
    progress = new RunProgress("execution", 2);
    runCard->body()->addWidget(progress);
    auto *actions = new FlowLayout(nullptr, 8, 6); // Wraps where the panel is narrow (over "1 分子").
    openFolder = new QPushButton(tr("保存先を開く"));
    openFolder->setObjectName("executionOpenFolder");
    openFolder->setIcon(lineIcon("folder"));
    viewResults = new QPushButton(tr("結果ビューアへ"));
    viewResults->setObjectName("executionResults");
    auto *history = new QPushButton(tr("履歴で見る"));
    history->setObjectName("executionHistory");
    // While it runs, the same stop as the header's (asking first).
    stopRun = new QPushButton(lineIcon("stop"), tr("停止…"));
    stopRun->setObjectName("executionStopRun");
    stopRun->setProperty("danger", true);
    stopRun->setToolTip(tr("実行中のNWChemを停止します（確認あり）。"));
    stopRun->hide();
    connect(stopRun, &QPushButton::clicked, this, &ExecutionPanel::confirmStop);
    for (auto *b : {openFolder, viewResults, history, stopRun})
        actions->addWidget(b);
    runCard->body()->addLayout(actions);
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
    panel->addWidget(runCard);

    auto *logCard = new SettingsCard(tr("ログ"));
    logCard->setObjectName("executionLogCard");
    logChannel = new QComboBox;
    logChannel->setObjectName("executionLogChannel");
    logChannel->addItem(tr("標準出力（末尾128 KiB）"), "output.out");
    logChannel->addItem(tr("標準エラー（末尾128 KiB）"), "stderr.err");
    auto *channels =
        new ChoiceButtons(logChannel, ChoiceButtons::Segments, {tr("標準出力"), tr("標準エラー")});
    choices << channels;
    // As the files of "履歴": the file and how much of it, following its end, and copy. The
    // channels head the body, so that the narrow panel over "1 分子" keeps their names.
    logCard->body()->addWidget(channels, 0, Qt::AlignLeft);
    logName = caption({});
    logName->setObjectName("executionLogName");
    logName->setWordWrap(false);
    logName->setProperty("mono", true);
    logCard->body()->addWidget(logName);
    follow = new QCheckBox(tr("追尾"));
    follow->setObjectName("executionFollow");
    follow->setChecked(true);
    follow->setToolTip(tr("ログが増えるたびに最後の行を表示します。"));
    logCard->header()->addWidget(follow);
    auto *copyLog = copyButton("executionCopyLog", tr("表示中のログをコピー"));
    logCard->header()->addWidget(copyLog);
    connect(logChannel, &QComboBox::currentIndexChanged, this, &ExecutionPanel::updateRun);
    log = logView("executionLog");
    log->setPlaceholderText(tr("まだ実行していません。計算を実行すると、ここにNWChemの出力を表示します。"));
    connect(follow, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            log->verticalScrollBar()->setValue(log->verticalScrollBar()->maximum());
    });
    // The log takes the height left; the state above keeps all of its lines and buttons, also in
    // the narrow panel over "1 分子" of a small window.
    log->setMinimumHeight(100);
    logCard->body()->addWidget(log, 1);
    connect(copyLog, &QToolButton::clicked, this,
            [this] { QApplication::clipboard()->setText(log->toPlainText()); });
    panel->addWidget(logCard, 1);
    // The panel follows the area of the tabs, over "1 分子" the pages at its right and over this tab
    // the settings below its bar (whose height changes as it wraps).
    window->workspaceTabs->installEventFilter(this);
    toolbar->installEventFilter(this);
    connect(window->workspaceTabs, &QTabWidget::currentChanged, this, [this] {
        if (runPanel->isVisible())
            placeRunPanel();
    });
}
void ExecutionPanel::showRunPanel(bool show) {
    if (show) {
        placeRunPanel();
        runPanel->show();
        runPanel->raise();
    } else
        runPanel->hide();
    if (window->runPanelAction)
        window->runPanelAction->setChecked(show);
}
bool ExecutionPanel::runPanelShown() const {
    return runPanel->isVisible();
}
void ExecutionPanel::placeRunPanel() {
    auto *tabs = window->workspaceTabs;
    const QRect area(tabs->mapTo(window, QPoint()), tabs->size());
    int left = area.right() + 1 - std::clamp(area.width() * 2 / 5, std::min(420, area.width()), 600);
    // Over "1 分子" it takes the place of the pages at the right: the view, its controls and the
    // options above it stay in sight.
    auto *column = window->findChild<QWidget *>("propertyColumn");
    if (column && tabs->currentWidget() == window->moleculePage && column->isVisible()) {
        if (!column->property("runPanelWatched").toBool()) { // Its width follows the splitter.
            column->setProperty("runPanelWatched", true);
            column->installEventFilter(this);
        }
        left = column->mapTo(window, QPoint()).x();
    }
    // Over "3 実行設定" it starts below the bar: the calculation, "終了したら結果ビューアに読み込む"
    // and "ジョブとして保存" stay in sight.
    int top = area.top();
    if (tabs->currentWidget() == this)
        top = toolbar->mapTo(window, QPoint(0, toolbar->height())).y();
    runPanel->setGeometry(left, top, area.right() + 1 - left, area.bottom() + 1 - top);
}
bool ExecutionPanel::eventFilter(QObject *watched, QEvent *event) {
    if ((watched == window->workspaceTabs || watched == toolbar ||
         watched->objectName() == "propertyColumn") &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move) && runPanel->isVisible())
        placeRunPanel();
    return QWidget::eventFilter(watched, event);
}
QString ExecutionPanel::baseDirectory() const {
    return window->path.isEmpty() ? QDir::currentPath() : QFileInfo(window->path).absolutePath();
}
void ExecutionPanel::pathField(QGridLayout *grid, const QString &label, const QString &key, bool directory) {
    const int row = grid->rowCount();
    auto *line = new QLineEdit;
    line->setObjectName("execution_" + key);
    fields[key] = line;
    auto *browse = new QPushButton(tr("参照…"));
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
        const auto path = directory
                              ? QFileDialog::getExistingDirectory(this, tr("ディレクトリ"), line->text())
                              : QFileDialog::getOpenFileName(this, tr("実行ファイル"), line->text());
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
        window->edit(tr("NWChem実行設定を変更"), [&](Document &d) { setExecutionSetting(d, key, value); });
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
        fail(tr("候補・整形・実行中は入力一式を保存できません。"));
    commitPendingEdits();
    if (window->mcp.policy && window->mcp.depth > 0)
        window->mcp.policy->checkRun(window->document(), baseDirectory());
    saved = saveExecutionBundle(window->document(), baseDirectory());
    updateRun();
    emit bundleChanged(saved.directory);
}
void ExecutionPanel::startCalculation() {
    if (window->hasCandidate() || window->worker || run.active())
        fail(tr("候補・整形・実行中は新しい計算を開始できません。"));
    commitPendingEdits();
    auto &mcp = window->mcp;
    if (mcp.policy) {
        const bool remote = mcp.depth > 0;
        if (remote)
            mcp.policy->checkRun(window->document(), baseDirectory());
        const auto settings = protectedExecution(window->document());
        if (!mcp.policy->allowExecutionSettings && settings != mcp.trustedExecution) {
            if (remote)
                fail(tr("NWChem実行ファイル・MPIランチャー・追加の環境変数・NWChemの追加入力が、"
                        "MolForma上で最後に設定した値と異なります（MCP経由の変更や"
                        "プロジェクトの読込による）。MCPからはこの設定で実行できません。"
                        "利用者が「2 計算条件」「3 実行設定」タブで設定を確認してください。"));
            // A person started the run: confirm in a dialog that MCP cannot see or press.
            QStringList environment;
            for (const auto &row : settings["environment"].toArray())
                if (row.toObject()["enabled"].toBool())
                    environment << row.toObject()["name"].toString() + "=" +
                                       row.toObject()["value"].toString();
            QMessageBox box(
                QMessageBox::Warning, tr("実行設定の確認"),
                tr("実行ファイル・ランチャー・環境変数・NWChemの追加入力が、MCP接続中に画面の外から"
                   "変更されています。この設定で実行しますか？"),
                QMessageBox::Yes | QMessageBox::No, window);
            const bool extra = !settings["extra_input"].toString().trimmed().isEmpty() ||
                               !settings["extra_tasks"].toString().trimmed().isEmpty();
            box.setInformativeText(tr("実行ファイル: %1\nランチャー: %2\n環境変数: %3\nNWChemの追加入力: %4")
                                       .arg(settings["executable"].toString(),
                                            settings["launcher"].toString(),
                                            environment.isEmpty() ? tr("なし") : environment.join(", "),
                                            extra ? tr("あり（「4 入力プレビュー」で確認）") : tr("なし")));
            box.setDefaultButton(QMessageBox::No);
            box.setProperty("mcpProtected", true);
            if (box.exec() != QMessageBox::Yes)
                return;
            mcp.trustedExecution = settings;
        }
    }
    run.start(window->document(), baseDirectory());
    showRunPanel();
}
void ExecutionPanel::confirmStop() {
    if (!run.active()) {
        window->statusBar()->showMessage(tr("実行中の計算はありません。"), 5000);
        return;
    }
    if (QMessageBox::question(this, tr("計算の停止"),
                              tr("実行中のNWChemを停止しますか？途中までのログと出力は保存先に残ります。"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        run.stop();
}
void ExecutionPanel::startFromMenu() {
    window->refreshPanel(this, true); // The tab may be stale while another one is shown.
    if (!startable) {
        window->workspaceTabs->setCurrentWidget(this);
        window->statusBar()->showMessage(
            run.active() ? tr("計算を実行中です。")
                         : tr("計算を開始できません。「3 実行設定」の「実行前の確認」を見てください。"),
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
                combo->addItem(tr("未対応: %1").arg(s[it.key()].toVariant().toString()), s[it.key()]);
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
    resources->setText(tr("CPU  %1 / %2 論理CPU（起動数 %3 × OpenMP %4 / BLAS %5）")
                           .arg(ranks * threads)
                           .arg(cpus)
                           .arg(ranks)
                           .arg(s["omp_threads"].toInt())
                           .arg(s["blas_threads"].toInt()));
    resources->setProperty("bad", ranks * threads > cpus);
    repolish(resources);
    memoryTotal->setText(tr("NWChemメモリ合計の目安  %1 MB（%2 × %3 MB）")
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
    // Under a field only what it does not say itself: where a name was found in PATH, or why it
    // was not found. A path stays in the checks at the right.
    for (const auto &key : {"executable", "launcher"})
        for (const auto &c : report.checks)
            if (c.label == (QString(key) == "executable" ? tr("NWChem実行ファイル") : tr("MPIランチャー"))) {
                const auto value = s[key].toString();
                const bool named = !value.contains('/') && !value.contains('\\');
                found[key]->setText(!c.ok   ? c.detail
                                    : named ? tr("✓ PATHから: %1").arg(c.detail)
                                            : QString());
                found[key]->setProperty("good", c.ok);
                found[key]->setProperty("bad", !c.ok);
                repolish(found[key]);
            }
    found["executable"]->setVisible(!found["executable"]->text().isEmpty());
    found["launcher"]->setVisible(s["mode"] == "mpi" && !found["launcher"]->text().isEmpty());
    QStringList messages;
    if (!editable)
        messages << tr("候補を確定・取消し、整形の終了後に実行できます。");
    for (const auto &e : report.errors)
        if (!covered.contains(e))
            messages << tr("エラー: %1").arg(e);
    for (const auto &w : report.warnings)
        messages << tr("確認: %1").arg(w);
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
                messages << tr("実行前の確認: %1").arg(QString::fromUtf8(e.what()));
                runnable = false;
            }
        }
    } catch (const std::exception &e) {
        command->setPlainText(tr("起動設定を作成できません:\n%1").arg(QString::fromUtf8(e.what())));
    }
    diagnostics->setText(messages.join('\n'));
    diagnostics->setVisible(!messages.isEmpty());
    diagnostics->setProperty("bad", !runnable || !editable);
    repolish(diagnostics);
    prepare->setEnabled(editable && !run.active() && portable);
    startable = editable && !run.active() && runnable && portable;
    const int problems = report.errors.size() + int(!editable) + int(runnable != report.errors.isEmpty());
    checkCard->setState(run.active() ? StatusCard::Waiting
                        : startable  ? (report.warnings.isEmpty() ? StatusCard::Ok : StatusCard::Warning)
                        : !editable  ? StatusCard::Waiting
                                     : StatusCard::Error,
                        run.active() ? tr("実行中です")
                        : startable  ? (report.warnings.isEmpty() ? tr("実行できます")
                                                                  : tr("実行できます（確認 %n 件）", nullptr,
                                                                       int(report.warnings.size())))
                        : !editable  ? tr("候補・整形の完了待ち")
                                     : tr("実行できません（%n 件）", nullptr, std::max(1, problems)));
    startHint->setVisible(startable);
    context->setText(calculationBrief(d));
    updateRun();
}
void ExecutionPanel::updateRun() {
    if (run.active() || (!run.plan().directory.isEmpty() && saved.directory == run.plan().directory))
        saved = run.plan();
    else if (!run.plan().directory.isEmpty() && saved.directory.isEmpty())
        saved = run.plan();
    const bool showingRun = saved.directory == run.plan().directory && !saved.directory.isEmpty();
    // The pill names the state: the text adds what it does not say.
    const QMap<QString, QString> details{
        {"cancelled", tr("途中までのログと出力は保存先に残っています。")},
        {"timed_out", tr("制限時間に達しました。途中までのログと出力は保存先に残っています。")},
        {"finished", tr("終了コード0")}};
    QStringList lines;
    if (showingRun) {
        lines << details.value(run.state());
        if (run.state() == "finished")
            lines << (!resultSummary.isEmpty()
                          ? tr("結果: %1（「結果ビューア」に読み込みました）").arg(resultSummary)
                          : tr("結果は「結果ビューアへ」で確認できます。"));
        lines << run.message();
        lines.removeAll(QString());
    }
    const auto stateText = showingRun ? lines.join('\n')
                           : saved.directory.isEmpty()
                               ? tr("未実行")
                               : tr("入力一式を保存しました（NWChemは起動していません）。");
    if (state->text() != stateText)
        state->setText(stateText);
    location->setText(displayPath(saved.directory));
    location->setToolTip(saved.directory);
    location->setVisible(!saved.directory.isEmpty());
    showRunLink->setVisible(!saved.directory.isEmpty());
    const QMap<QString, QString> pills{
        {"starting", tr("起動中")},           {"running", tr("実行中")},
        {"stopping", tr("停止処理中")},       {"cancelled", tr("停止", "run state")},
        {"timed_out", tr("時間制限")},        {"failed", tr("失敗")},
        {"finished", tr("終了", "run state")}};
    const auto kind = !showingRun                 ? QString("saved")
                      : run.active()              ? QString("running")
                      : run.state() == "finished" ? QString("finished")
                                                  : QString("failed");
    statePill->setText(showingRun                  ? pills.value(run.state(), run.state())
                       : saved.directory.isEmpty() ? QString()
                                                   : tr("保存のみ", "run state"));
    statePill->setVisible(!statePill->text().isEmpty());
    if (statePill->property("state").toString() != kind) {
        statePill->setProperty("state", kind);
        repolish(statePill);
    }
    const qint64 seconds = run.elapsedSeconds();
    timing->setText(!showingRun ? QString()
                    : run.active()
                        ? tr("経過 %1 · %2 開始")
                              .arg(elapsedClock(seconds))
                              .arg(QDateTime::currentDateTime().addSecs(-seconds).toString("HH:mm"))
                        : tr("所要 %1").arg(elapsedClock(seconds)));
    openFolder->setEnabled(!saved.directory.isEmpty());
    viewResults->setEnabled(!saved.directory.isEmpty() && !resultFiles(saved.directory).isEmpty());
    const bool stoppable = run.active() && run.state() != "stopping";
    stopRun->setVisible(run.active());
    stopRun->setEnabled(stoppable);
    prepare->setVisible(!run.active());
    runningNote->setVisible(run.active());
    state->setVisible(!run.active() && !state->text().isEmpty());
    if (window->stopAction)
        window->stopAction->setEnabled(stoppable);
    if (run.active()) {
        startable = false;
        prepare->setEnabled(false);
    }
    window->showRun(run.active(), tr("実行中 %1").arg(elapsedClock(seconds)), stoppable);
    // Always visible in the status bar, whatever the tab.
    if (!run.plan().directory.isEmpty()) {
        const QMap<QString, QString> brief{{"starting", tr("NWChemを起動中")},
                                           {"running", tr("NWChem実行中")},
                                           {"stopping", tr("NWChemを停止中")},
                                           {"cancelled", tr("最後の計算: 停止")},
                                           {"timed_out", tr("最後の計算: 時間制限")},
                                           {"failed", tr("最後の計算: 失敗")},
                                           {"finished", tr("最後の計算: 終了（コード0）")}};
        runStatus->setText(
            QString("%1%2 · %3")
                .arg(run.active() ? "▶ " : "", brief.value(run.state(), run.state()))
                .arg(run.active() ? elapsedClock(seconds) : QFileInfo(run.plan().directory).fileName()));
        runStatus->show();
    }
    if (showingRun)
        progress->showRun(saved.directory, run.state(), run.active());
    else
        progress->clear();
    appendLog();
}
void ExecutionPanel::appendLog() {
    constexpr qint64 tail = 128 * 1024;
    const QString path = saved.directory.isEmpty()
                             ? QString()
                             : QDir(saved.directory).filePath(logChannel->currentData().toString());
    logName->setText(saved.directory.isEmpty()
                         ? QString()
                         : tr("%1 · 末尾 128 KiB").arg(logChannel->currentData().toString()));
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
    const bool atEnd = follow->isChecked() || bar->value() == bar->maximum();
    QString added;
    if (logOffset < 0) {
        const bool truncated = file.size() > tail;
        if (truncated)
            file.seek(file.size() - tail);
        added = (truncated ? tr("…ログの末尾を表示しています。全文は保存先にあります。\n") : QString()) +
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
    const QMap<QString, QString> names{{"finished", tr("計算が終了しました（終了コード0）")},
                                       {"failed", tr("計算が失敗しました")},
                                       {"cancelled", tr("計算を停止しました")},
                                       {"timed_out", tr("計算が制限時間で停止しました")}};
    const auto directory = run.plan().directory;
    const auto message = QString("%1: %2").arg(names.value(run.state(), tr("計算が終了しました")),
                                               QFileInfo(directory).fileName());
    // The run status at the right of the status bar says how it ended; a message at the left only
    // adds what the log says once it is read (showResultSummary).
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
        window->statusBar()->showMessage(tr("計算が終了しました: %1").arg(resultSummary), 60000);
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
    const QString rules = settingsRules() + R"(
        #executionTab { background: @window; }
        #runPanel { background: @window; border: none; border-left: 1px solid @line; }
        #executionUsage { background: @window; border: 1px solid @line; border-radius: 8px; }
        QLabel#executionStatePill { border-radius: 10px; padding: 2px 9px; font-weight: bold; }
        QLabel#executionStatePill[state="running"] { background: @tint; color: @accentText; }
        QLabel#executionStatePill[state="finished"] { background: @goodTint; color: @good; }
        QLabel#executionStatePill[state="failed"] { background: @badTint; color: @error; }
        QLabel#executionStatePill[state="saved"] { background: @segment; color: @muted; }
        QLabel#executionRunningNote { background: @tint; color: @accentText; border-radius: 8px; padding: 8px 12px; }
        QLabel[tile="true"] { background: @window; border: 1px solid @line; border-radius: 8px; padding: 6px 9px; }
    )";
    setStyleSheet(chromeRules(rules, pal));
    runPanel->setStyleSheet(chromeRules(rules, pal));
}
} // namespace mf
