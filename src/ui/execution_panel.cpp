// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/execution_panel.h"
#include "editing/coordinates.h"
#include "model/i18n.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_restart.h"
#include "nwchem/run_diagnostics.h"
#include "ui/editor_chrome.h"
#include "ui/execution_form.h"
#include "ui/main_window.h"
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
// What the run panel adds to the state pill of an ended run.
QString endedDetail(RunState state) {
    switch (state) {
    case RunState::Cancelled:
        return tr("途中までのログと出力は保存先に残っています。");
    case RunState::TimedOut:
        return tr("制限時間に達しました。途中までのログと出力は保存先に残っています。");
    case RunState::Finished:
        return tr("終了コード0");
    default:
        return {};
    }
}
// The run in the status bar, whatever the tab.
QString statusBarText(RunState state) {
    switch (state) {
    case RunState::Starting:
        return tr("NWChemを起動中");
    case RunState::Running:
        return tr("NWChem実行中");
    case RunState::Stopping:
        return tr("NWChemを停止中");
    case RunState::Cancelled:
        return tr("最後の計算: 停止");
    case RunState::TimedOut:
        return tr("最後の計算: 時間制限");
    case RunState::Failed:
        return tr("最後の計算: 失敗");
    case RunState::Finished:
        return tr("最後の計算: 終了（コード0）");
    default:
        return runStateName(state);
    }
}
// How a run ended, in the message when it does.
QString endedMessage(RunState state) {
    switch (state) {
    case RunState::Finished:
        return tr("計算が終了しました（終了コード0）");
    case RunState::Failed:
        return tr("計算が失敗しました");
    case RunState::Cancelled:
        return tr("計算を停止しました");
    case RunState::TimedOut:
        return tr("計算が制限時間で停止しました");
    default:
        return tr("計算が終了しました");
    }
}
} // namespace

ExecutionPanel::ExecutionPanel(MainWindow *owner)
    : QWidget(owner), window(owner), run(this), queue(run, this) {
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
    openResults->setChecked(window->preference("execution/open_results", true).toBool());
    connect(openResults, &QCheckBox::toggled, this,
            [this](bool on) { window->setPreference("execution/open_results", on); });
    tools->addWidget(openResults);
    prepare = new QPushButton(tr("ジョブとして保存"));
    prepare->setObjectName("executionPrepare");
    prepare->setIcon(lineIcon("apply"));
    prepare->setToolTip(tr("NWChemを起動せず、入力とプロジェクトを新しいジョブフォルダーに保存します"
                           "（別のPCで実行するときなど）。"));
    tools->addWidget(prepare);
    connect(prepare, &QPushButton::clicked, this, [this] { window->safe([this] { saveBundle(); }); });
    enqueue = new QPushButton(tr("キューに追加"));
    enqueue->setObjectName("executionEnqueue");
    enqueue->setToolTip(tr("現在の分子と条件を保存し、順番に実行する計算へ登録します。"));
    tools->addWidget(enqueue);
    connect(enqueue, &QPushButton::clicked, this, [this] {
        window->safe([this] {
            if (!enqueueCalculation().isEmpty())
                showQueue();
        });
    });
    queueButton = new QPushButton(tr("計算キュー"));
    queueButton->setObjectName("executionQueue");
    tools->addWidget(queueButton);
    connect(queueButton, &QPushButton::clicked, this, &ExecutionPanel::showQueue);
    connect(&queue, &NwchemQueue::changed, this,
            [this] { queueButton->setText(tr("計算キュー（待機 %n 件）", nullptr, queue.pending())); });
    connect(&queue, &NwchemQueue::bundleSaved, this, &ExecutionPanel::bundleChanged);
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
    settingsForm = new ExecutionForm(ExecutionForm::Scope::Project, "execution");
    forms->addWidget(settingsForm);
    connect(settingsForm, &ExecutionForm::changed, this, &ExecutionPanel::change);
    auto *memoryCell = new QWidget;
    auto *memoryLayout = new QVBoxLayout(memoryCell);
    memoryLayout->setContentsMargins(0, 0, 0, 0);
    memoryLayout->setSpacing(4);
    memoryLayout->addWidget(caption(tr("メモリ / MPIプロセス")));
    memory = new QSpinBox;
    memory->setObjectName("execution_memory");
    const auto memorySpec = nwchemSettingSpec("memory_mb");
    memory->setRange(memorySpec.minimum, memorySpec.maximum);
    memory->setSuffix(" MB");
    memory->setKeyboardTracking(false);
    memory->setToolTip(tr("NWChemの memory 指定です（「2 計算条件」と「4 入力プレビュー」の要約にも表示）。"
                          "実際の使用量の上限ではありません。"));
    memoryLayout->addWidget(memory);
    settingsForm->resourcesGrid()->addWidget(memoryCell, 1, 1);
    connect(memory, &QSpinBox::valueChanged, this, [this](int n) {
        if (busy || window->hasCandidate() || window->chemistryRunning())
            return;
        window->safe([&] {
            window->edit(tr("NWChemメモリを変更"), [&](Document &d) { setNwchemSetting(d, "memory_mb", n); });
        });
    });
    // What the run takes of this PC, under the resources.
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
    settingsForm->resourcesCard()->body()->insertWidget(1, usage);
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
    runStatus->setToolTip(tr("実行状況とログの表示を切り替えます。"));
    runStatus->hide();
    window->statusBar()->addPermanentWidget(runStatus);
    connect(runStatus, &QToolButton::clicked, this, [this] { showRunPanel(!runPanelShown()); });
    connect(&run, &NwchemRun::changed, this, [this] {
        if (run.state() != previousRunState) {
            const bool ended = runStateActive(previousRunState) && !run.active();
            previousRunState = run.state();
            // The next session looks for a run this one may leave to its supervisor.
            if (run.supervised())
                window->setPreference("execution/running", run.active() ? run.plan().directory : QString());
            refresh();
            emit bundleChanged(run.plan().directory);
            if (ended)
                runEnded();
        } else
            updateRun();
    });
}
void ExecutionPanel::buildRunPanel() {
    // Below the tabs, whatever the tab, which give up its height so that it hides nothing: its title
    // with the state of the run, then the run at the left and its log, at the width of the window,
    // beside it.
    runPanel = new QFrame;
    runPanel->setObjectName("runPanel");
    runPanel->setAttribute(Qt::WA_StyledBackground);
    runPanel->hide();
    panelHeight = window->preference("execution/run_panel_height", 0).toInt();
    auto *panel = new QVBoxLayout(runPanel);
    panel->setContentsMargins(14, 4, 12, 12);
    panel->setSpacing(4);
    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    auto *title = new QLabel(tr("実行状況とログ")); // The name of every way to open it.
    title->setObjectName("runPanelTitle");
    title->setProperty("cardTitle", true);
    head->addWidget(title);
    statePill = new QLabel;
    statePill->setObjectName("executionStatePill");
    head->addWidget(statePill);
    timing = muted();
    timing->setObjectName("executionTiming");
    timing->setWordWrap(false);
    head->addWidget(timing);
    head->addStretch();
    maximize = new QToolButton;
    maximize->setObjectName("runPanelMaximize");
    maximize->setProperty("flat", true);
    maximize->setCheckable(true);
    head->addWidget(maximize);
    connect(maximize, &QToolButton::clicked, this, [this](bool on) {
        panelFull = on;
        placeRunPanel();
    });
    auto *close = new QToolButton;
    close->setObjectName("runPanelClose");
    close->setIcon(lineIcon("close"));
    close->setProperty("flat", true);
    close->setToolTip(tr("閉じる（実行は続きます）"));
    close->setAccessibleName(tr("実行状況を閉じる"));
    head->addWidget(close);
    panel->addLayout(head);
    connect(close, &QToolButton::clicked, this, [this] { showRunPanel(false); });

    auto *columns = new QHBoxLayout;
    columns->setSpacing(10);
    panel->addLayout(columns, 1);
    runCard = new SettingsCard({});
    runCard->setObjectName("executionRunCard");
    runCard->setAccessibleName(tr("実行状況"));
    runCard->layout()->setContentsMargins(14, 10, 14, 10);
    // The folder and the progress scroll where tasks are many; the actions stay in sight.
    auto *detailScroll = new QScrollArea;
    detailScroll->setObjectName("executionRunDetails");
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *details = new QWidget;
    details->setObjectName("executionRunDetailsPage");
    auto *detail = new QVBoxLayout(details);
    detail->setContentsMargins(0, 0, 0, 0);
    detail->setSpacing(7);
    auto *folder = new QHBoxLayout;
    folder->setSpacing(6);
    location = new QLabel;
    location->setObjectName("executionLocation");
    location->setProperty("mono", true);
    location->setProperty("muted", true);
    location->setWordWrap(true);
    location->setTextFormat(Qt::PlainText);
    location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    folder->addWidget(location, 1);
    openFolder = new QPushButton;
    openFolder->setObjectName("executionOpenFolder");
    openFolder->setIcon(lineIcon("folder"));
    openFolder->setToolTip(tr("保存先を開く"));
    openFolder->setAccessibleName(tr("保存先を開く"));
    folder->addWidget(openFolder, 0, Qt::AlignTop);
    detail->addLayout(folder);
    state = new QLabel;
    state->setObjectName("executionState");
    state->setWordWrap(true);
    state->setTextFormat(Qt::PlainText);
    detail->addWidget(state);
    diagnosisSetting = new QPushButton(tr("診断に関係する設定を開く"));
    diagnosisSetting->setObjectName("executionDiagnosisSetting");
    detail->addWidget(diagnosisSetting);
    connect(diagnosisSetting, &QPushButton::clicked, this, [this] {
        for (const auto &value : run.plan().manifest["diagnostics"].toArray()) {
            const auto item = value.toObject();
            if (!item["setting"].toString().isEmpty()) {
                window->showSetting(item["tab"].toInt(), item["setting"].toString());
                break;
            }
        }
    });
    continueRun = new QPushButton(restartLabel("continue"));
    continueRun->setObjectName("executionContinue");
    continueRun->setToolTip(
        tr("終わらなかった計算を、NWChemのデータベースに残った最新の構造と分子軌道から続けます。"
           "新しいジョブとして実行します。"));
    detail->addWidget(continueRun);
    connect(continueRun, &QPushButton::clicked, this,
            [this] { window->safe([this] { emit restartRequested(run.plan().directory, "continue"); }); });
    progress = new RunProgress("execution", 4);
    detail->addWidget(progress);
    detail->addStretch();
    detailScroll->setWidget(details);
    runCard->body()->addWidget(detailScroll, 1);
    auto *actions = new FlowLayout(nullptr, 6, 6);
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
    for (auto *b : {viewResults, history, stopRun})
        actions->addWidget(b);
    runCard->body()->addLayout(actions);
    connect(openFolder, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(saved.directory)); });
    connect(history, &QPushButton::clicked, this, [this] { emit historyRequested(saved.directory); });
    connect(viewResults, &QPushButton::clicked, this, [this] { emit resultsRequested(saved.directory); });
    columns->addWidget(runCard);

    auto *logCard = new SettingsCard({});
    logCard->setObjectName("executionLogCard");
    logCard->setAccessibleName(tr("ログ"));
    logCard->layout()->setContentsMargins(12, 8, 12, 12);
    logCard->layout()->setSpacing(6);
    logChannel = new QComboBox;
    logChannel->setObjectName("executionLogChannel");
    logChannel->addItem(tr("標準出力（末尾128 KiB）"), "output.out");
    logChannel->addItem(tr("標準エラー（末尾128 KiB）"), "stderr.err");
    auto *channels =
        new ChoiceButtons(logChannel, ChoiceButtons::Segments, {tr("標準出力"), tr("標準エラー")});
    choices << channels;
    // As the files of "履歴": the channel, the file and how much of it, following its end, and copy.
    logCard->header()->insertWidget(1, channels);
    logName = caption({});
    logName->setObjectName("executionLogName");
    logName->setWordWrap(false);
    logName->setProperty("mono", true);
    logCard->header()->insertWidget(2, logName);
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
    log->setMinimumHeight(60);
    logCard->body()->addWidget(log, 1);
    connect(copyLog, &QToolButton::clicked, this,
            [this] { QApplication::clipboard()->setText(log->toPlainText()); });
    columns->addWidget(logCard, 1);
}
void ExecutionPanel::dockRunPanel(QSplitter *split) {
    dock = split;
    dock->addWidget(runPanel);
    // The tabs take what the window gains or loses.
    dock->setStretchFactor(0, 1);
    dock->setStretchFactor(1, 0);
    dock->installEventFilter(this);
    connect(dock, &QSplitter::splitterMoved, this, [this] {
        panelHeight = runPanel->height();
        window->setPreference("execution/run_panel_height", panelHeight);
    });
}
void ExecutionPanel::showRunPanel(bool show) {
    if (show) {
        runPanel->show();
        placeRunPanel();
    } else {
        closeOnResults = panelFull = false;
        runPanel->hide();
        dock->widget(0)->show();
    }
    window->setRunPanelShown(show);
}
bool ExecutionPanel::runPanelShown() const {
    return !runPanel->isHidden();
}
void ExecutionPanel::placeRunPanel() {
    auto *tabs = dock->widget(0);
    const int total = dock->height();
    const int room = total - dock->handleWidth() - tabs->minimumHeight();
    const int height = std::min(panelHeight > 0 ? panelHeight : std::clamp(total * 3 / 8, 200, 300), room);
    // A window too low for both: the panel takes the place of the tabs until a tab is asked for.
    panelTight = room < 200;
    const bool full = panelFull || panelTight;
    maximize->setVisible(!panelTight);
    maximize->setChecked(full);
    maximize->setIcon(lineIcon(full ? "collapse" : "expand"));
    const auto label = full ? tr("元の高さに戻す") : tr("全面に広げる");
    maximize->setToolTip(label);
    maximize->setAccessibleName(label);
    tabs->setVisible(!full);
    if (!full)
        dock->setSizes({total - height, height});
}
void ExecutionPanel::showWorkspace() {
    if (!dock->widget(0)->isHidden())
        return;
    if (panelTight)
        showRunPanel(false);
    else {
        panelFull = false;
        placeRunPanel();
    }
}
void ExecutionPanel::resultsShown() {
    if (closeOnResults)
        showRunPanel(false);
}
bool ExecutionPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == dock && event->type() == QEvent::Resize && !runPanel->isHidden())
        placeRunPanel();
    return QWidget::eventFilter(watched, event);
}
QString ExecutionPanel::baseDirectory() const {
    return window->projectPath().isEmpty() ? QDir::currentPath()
                                           : QFileInfo(window->projectPath()).absolutePath();
}
void ExecutionPanel::change(const QString &key, const QJsonValue &value) {
    if (busy || window->hasCandidate() || window->chemistryRunning() ||
        executionSettings(window->document()).value(key) == value)
        return;
    window->safe([&] {
        window->edit(tr("NWChem実行設定を変更"), [&](Document &d) { setExecutionSetting(d, key, value); });
    });
}
void ExecutionPanel::commitPendingEdits() {
    // A stale tab first catches up, dropping typing that an eager refresh would have replaced.
    window->refreshPanel(this);
    if (auto *focused = QApplication::focusWidget(); focused && isAncestorOf(focused))
        focused->clearFocus();
    settingsForm->commitEnvironment();
}
void ExecutionPanel::showField(const QString &key) {
    QWidget *target = key == "memory_mb" ? memory : settingsForm->field(key);
    if (target)
        target->setFocus(Qt::OtherFocusReason);
}
void ExecutionPanel::saveBundle() {
    if (window->hasCandidate() || window->chemistryRunning() || run.active())
        fail(tr("候補・整形・実行中は入力一式を保存できません。"));
    commitPendingEdits();
    if (window->accessPolicy() && window->remoteRequest())
        window->accessPolicy()->checkRun(window->document(), baseDirectory());
    saved = saveExecutionBundle(window->document(), baseDirectory());
    updateRun();
    emit bundleChanged(saved.directory);
}
void ExecutionPanel::startCalculation() {
    if (window->hasCandidate() || window->chemistryRunning() || run.active())
        fail(tr("候補・整形・実行中は新しい計算を開始できません。"));
    commitPendingEdits();
    if (!authorizeCalculation())
        return;
    closeOnResults = false;
    logChannel->setCurrentIndex(0);
    run.start(window->document(), baseDirectory());
    showRunPanel();
}
bool ExecutionPanel::authorizeCalculation() {
    const auto *policy = window->accessPolicy();
    const bool remote = window->remoteRequest();
    if (policy && remote)
        policy->checkRun(window->document(), baseDirectory());
    // The executable, launcher, environment and additional input choose what runs: values that came
    // from a file, the history, an autosave copy or MCP run only once the user confirmed them.
    if ((!policy || !policy->allowExecutionSettings) && !window->untrustedExecution().isEmpty()) {
        if (remote)
            fail(tr("NWChem実行ファイル・MPIランチャー・追加の環境変数・NWChemの追加入力に、利用者が"
                    "MolFormaで入力・確認していない値があります（MCP経由の変更や、プロジェクト・履歴・"
                    "自動保存の読込による）。MCPからはこの設定で実行できません。"
                    "利用者が「2 計算条件」「3 実行設定」タブで設定を確認してください。"));
        if (!confirmExecution())
            return false;
        window->trustCurrentExecution();
    }
    return true;
}
QString ExecutionPanel::enqueueCalculation(const QueueChain &chain) {
    if (window->hasCandidate() || window->chemistryRunning())
        fail(tr("候補を確定・取消し、整形の終了後に実行できます。"));
    commitPendingEdits();
    if (!authorizeCalculation())
        return {};
    const auto document = window->document();
    const auto base = baseDirectory();
    std::function<void()> check;
    if (const auto *policy = window->accessPolicy(); policy && window->remoteRequest())
        check = [policy = *policy, document, base] { policy.checkRun(document, base); };
    const auto id = queue.enqueue(document, base, check, chain);
    if (!chain.active())
        emit bundleChanged(queue.entries().last().plan.directory);
    return id;
}
QStringList ExecutionPanel::enqueueChain(const QString &from, const QString &structure,
                                         const QStringList &steps, bool endFrequencies) {
    if (window->hasCandidate() || window->chemistryRunning())
        fail(tr("候補を確定・取消し、整形の終了後に実行できます。"));
    if (steps.isEmpty())
        fail(tr("登録する計算を選んでください。"));
    commitPendingEdits();
    if (!authorizeCalculation())
        return {};
    const auto base = baseDirectory();
    const auto *policy = window->accessPolicy();
    const bool remote = policy && window->remoteRequest();
    return queue.enqueueChain(window->document(), base, from, structure, steps, endFrequencies,
                              [policy, remote, base](const Document &d) -> std::function<void()> {
                                  if (!remote)
                                      return {};
                                  return [policy = *policy, d, base] { policy.checkRun(d, base); };
                              });
}
void ExecutionPanel::showChainDialog() {
    if (auto *existing = findChild<QDialog *>("calculationChainDialog")) {
        existing->show();
        existing->raise();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("calculationChainDialog");
    dialog->setWindowTitle(tr("前の計算から続けて登録"));
    auto *layout = new QVBoxLayout(dialog);
    auto *hint = new QLabel(
        tr("キューの計算が正常に終わったら、その構造から次の計算を始めます。次の計算は「2 計算条件」"
           "の今の条件（手法・基底など）を使い、連鎖では計算目的と計算名だけを変えます。"
           "入力一式は前の計算が終わった時点で保存します。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *source = new QComboBox;
    source->setObjectName("calculationChainSource");
    for (const auto &entry : queue.entries())
        if (entry.state == RunState::Prepared || runStateActive(entry.state) ||
            entry.state == RunState::Finished) {
            QString task;
            for (const auto &c : nwchemChoices("task"))
                if (c.value == nwchemSettings(entry.document)["task"].toString())
                    task = c.label;
            source->addItem(
                tr("%1（%2・%3）")
                    .arg(entry.name, task,
                         entry.state == RunState::Prepared ? tr("待機中") : runStateName(entry.state)),
                entry.id);
        }
    source->setCurrentIndex(source->count() - 1);
    form->addRow(tr("引継ぎ元"), source);
    auto *structure = new QComboBox;
    structure->setObjectName("calculationChainStructure");
    for (const auto &key : queueChainStructures())
        structure->addItem(queueChainStructureName(key), key);
    form->addRow(tr("引き継ぐ構造"), structure);
    // The highest point for paths, the final structure otherwise.
    const auto suggest = [this, source, structure] {
        for (const auto &entry : queue.entries())
            if (entry.id == source->currentData().toString()) {
                const NwchemCalculation calc(nwchemSettings(entry.document));
                structure->setCurrentIndex(structure->findData(calc.path() ? "highest" : "final"));
            }
    };
    connect(source, &QComboBox::currentIndexChanged, dialog, suggest);
    suggest();
    auto *single = new QRadioButton;
    single->setObjectName("calculationChainCurrent");
    QString task;
    for (const auto &c : nwchemChoices("task"))
        if (c.value == nwchemSettings(window->document())["task"].toString())
            task = c.label;
    single->setText(tr("「2 計算条件」の今の条件で1件（%1）").arg(task));
    auto *recipe = new QRadioButton(tr("反応経路の連鎖"));
    recipe->setObjectName("calculationChainRecipe");
    recipe->setChecked(true);
    layout->addWidget(single);
    layout->addWidget(recipe);
    auto *steps = new QWidget;
    auto *stepLayout = new QVBoxLayout(steps);
    stepLayout->setContentsMargins(24, 0, 0, 0);
    auto *saddle = new QCheckBox(tr("遷移状態探索 → 振動数"));
    saddle->setObjectName("calculationChainSaddle");
    auto *irc = new QCheckBox(tr("IRC（両方向）"));
    irc->setObjectName("calculationChainIrc");
    auto *ends = new QCheckBox(tr("IRCの両端の構造最適化"));
    ends->setObjectName("calculationChainEnds");
    auto *endFrequencies = new QCheckBox(tr("両端の振動数も計算（熱補正・反応エネルギー図用）"));
    endFrequencies->setObjectName("calculationChainEndFrequencies");
    for (auto *box : {saddle, irc, ends, endFrequencies}) {
        box->setChecked(true);
        stepLayout->addWidget(box);
    }
    layout->addWidget(steps);
    connect(recipe, &QRadioButton::toggled, steps, &QWidget::setEnabled);
    connect(ends, &QCheckBox::toggled, endFrequencies, &QWidget::setEnabled);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *add = buttons->addButton(tr("キューに登録"), QDialogButtonBox::AcceptRole);
    add->setObjectName("calculationChainAdd");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(add, &QPushButton::clicked, dialog, [=, this] {
        window->safe([&] {
            if (source->currentIndex() < 0)
                fail(tr("キューに引継ぎ元の計算を登録してください。"));
            QStringList chosen;
            if (single->isChecked())
                chosen << "current";
            else
                for (const auto &[box, key] : {std::pair{saddle, "saddle"}, {irc, "irc"}, {ends, "ends"}})
                    if (box->isChecked())
                        chosen << key;
            const auto ids =
                enqueueChain(source->currentData().toString(), structure->currentData().toString(), chosen,
                             endFrequencies->isChecked());
            if (!ids.isEmpty()) {
                dialog->close();
                window->statusBar()->showMessage(
                    tr("%n件の計算をキューに登録しました。", nullptr, int(ids.size())), 8000);
                showQueue();
            }
        });
    });
    dialog->show();
}
void ExecutionPanel::showQueue() {
    if (auto *existing = findChild<QDialog *>("calculationQueueDialog")) {
        existing->show();
        existing->raise();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("calculationQueueDialog");
    dialog->setWindowTitle(tr("計算キュー"));
    dialog->resize(820, 420);
    auto *layout = new QVBoxLayout(dialog);
    auto *hint = new QLabel(tr("登録時の分子・条件で1件ずつ実行します。一時停止は次の計算から有効です。"
                               "アプリ終了後は自動再開せず、待機分の入力一式は履歴に残ります。"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *table = new QTableWidget(0, 5);
    table->setObjectName("calculationQueueTable");
    table->setHorizontalHeaderLabels({tr("計算名"), tr("状態"), tr("構造"), tr("保存先"), tr("詳細")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table, 1);
    auto *controls = new QHBoxLayout;
    layout->addLayout(controls);
    auto *resume = new QPushButton(tr("キューを開始・再開"));
    resume->setObjectName("calculationQueueResume");
    auto *pause = new QPushButton(tr("キューを一時停止"));
    pause->setObjectName("calculationQueuePause");
    auto *up = new QPushButton(tr("上へ"));
    auto *down = new QPushButton(tr("下へ"));
    auto *cancel = new QPushButton(tr("登録を取消"));
    auto *stop = new QPushButton(tr("実行中を停止"));
    auto *chain = new QPushButton(tr("前の計算から続けて登録…"));
    chain->setObjectName("calculationQueueChain");
    chain->setToolTip(tr("キューの計算の最終構造・経路の最高点・IRCの端から、遷移状態探索・IRC・最適化などを"
                         "続けて実行するように登録します。"));
    for (auto *button : {resume, pause, up, down, cancel, stop, chain})
        controls->addWidget(button);
    connect(chain, &QPushButton::clicked, dialog, [this] { showChainDialog(); });
    auto *stopOnFailure = new QCheckBox(tr("失敗・停止・時間切れでキューを一時停止"));
    stopOnFailure->setObjectName("calculationQueueStopOnFailure");
    layout->addWidget(stopOnFailure);
    const auto selected = [table] {
        auto *item = table->item(table->currentRow(), 0);
        return item ? item->data(Qt::UserRole).toString() : QString();
    };
    const auto update = [this, table, selected, resume, pause, stop, stopOnFailure] {
        const auto id = selected();
        table->setRowCount(queue.entries().size());
        int row = 0;
        for (const auto &entry : queue.entries()) {
            QString structure = tr("登録時の分子");
            if (entry.chain.active()) {
                QString from = tr("前の計算");
                for (const auto &other : queue.entries())
                    if (other.id == entry.chain.from)
                        from = other.name;
                structure = tr("%1の%2").arg(from, queueChainStructureName(entry.chain.structure));
            }
            const QStringList values{
                entry.name, entry.state == RunState::Prepared ? tr("待機中") : runStateName(entry.state),
                structure,
                entry.plan.directory.isEmpty() && entry.chain.active() ? tr("（前の計算の終了後に保存）")
                                                                       : entry.plan.directory,
                entry.error};
            for (int column = 0; column < values.size(); ++column) {
                auto *cell = new QTableWidgetItem(values[column]);
                cell->setToolTip(values[column]);
                table->setItem(row, column, cell);
            }
            table->item(row, 0)->setData(Qt::UserRole, entry.id);
            if (entry.id == id)
                table->selectRow(row);
            ++row;
        }
        resume->setEnabled(queue.paused() && queue.pending());
        pause->setEnabled(!queue.paused());
        stop->setEnabled(run.active());
        const QSignalBlocker blocker(stopOnFailure);
        stopOnFailure->setChecked(queue.stopOnFailure());
    };
    connect(&queue, &NwchemQueue::changed, dialog, update);
    connect(&run, &NwchemRun::changed, dialog, [this, stop] { stop->setEnabled(run.active()); });
    connect(resume, &QPushButton::clicked, dialog, [this] { queue.resume(); });
    connect(pause, &QPushButton::clicked, dialog, [this] { queue.pause(); });
    connect(stopOnFailure, &QCheckBox::toggled, &queue, &NwchemQueue::setStopOnFailure);
    connect(cancel, &QPushButton::clicked, dialog,
            [this, selected] { window->safe([&] { queue.cancel(selected()); }); });
    const auto move = [this, selected, table](int direction) {
        window->safe([&] {
            for (int row = table->currentRow() + direction; row >= 0 && row < queue.entries().size();
                 row += direction)
                if (queue.entries()[row].state == RunState::Prepared) {
                    queue.move(selected(), row);
                    break;
                }
        });
    };
    connect(up, &QPushButton::clicked, dialog, [move] { move(-1); });
    connect(down, &QPushButton::clicked, dialog, [move] { move(1); });
    connect(stop, &QPushButton::clicked, dialog, [this] { confirmStop(); });
    update();
    dialog->show();
}
QStringList ExecutionPanel::enqueueGeometries(const QVector<Document> &geometries, bool frequencies) {
    if (window->hasCandidate() || window->chemistryRunning())
        fail(tr("候補を確定・取消し、整形の終了後に実行できます。"));
    if (geometries.isEmpty() || geometries.size() + queue.entries().size() > 1000)
        fail(tr("キューにはこのセッションで1000件まで登録できます。"));
    commitPendingEdits();
    if (!authorizeCalculation())
        return {};
    QVector<Document> documents;
    const auto base = baseDirectory();
    for (int i = 0; i < geometries.size(); ++i) {
        auto d = window->document();
        applyResultGeometry(d, geometries[i]);
        setNwchemSetting(d, "task", frequencies ? "optimize_frequencies" : "optimize");
        setNwchemSetting(d, "start", nwchemSettings(d)["start"].toString() + QString("_conf%1").arg(i + 1));
        executionPlan(d, base, {}, true); // Validate the entire batch before writing any bundle.
        documents.append(d);
    }
    QStringList directories;
    for (const auto &d : documents) {
        std::function<void()> check;
        if (const auto *policy = window->accessPolicy(); policy && window->remoteRequest())
            check = [policy = *policy, d, base] { policy.checkRun(d, base); };
        queue.enqueue(d, base, check);
        directories << queue.entries().last().plan.directory;
        emit bundleChanged(directories.last());
    }
    showQueue();
    return directories;
}
bool ExecutionPanel::confirmExecution() {
    // A person started the run: confirm in a dialog that MCP cannot see or press.
    const auto &d = window->document();
    const auto settings = protectedExecution(d);
    const auto report = validateExecution(d, baseDirectory(), false);
    // Show where a name or relative path leads (for example a program beside the project file).
    const auto program = [](const QString &value, const QString &found) {
        return found.isEmpty() || found == value ? value : tr("%1（%2）").arg(value, found);
    };
    QStringList environment;
    for (const auto &row : settings["environment"].toArray())
        if (row.toObject()["enabled"].toBool())
            environment << row.toObject()["name"].toString() + "=" + row.toObject()["value"].toString();
    QStringList extra;
    for (const auto *key : {"extra_input", "extra_tasks"})
        if (const auto text = settings[key].toString().trimmed(); !text.isEmpty())
            extra << (QString(key) == "extra_input" ? tr("追加入力（タスクの前）")
                                                    : tr("追加入力（入力の末尾）")) +
                         '\n' + text;
    QMessageBox box(
        QMessageBox::Warning, tr("実行設定の確認"),
        tr("実行ファイル・ランチャー・環境変数・NWChemの追加入力に、MolFormaで入力・確認していない"
           "値があります（プロジェクト・履歴・自動保存の読込やMCPによる変更）。これらの設定は任意の"
           "プログラムを起動できます。内容を確認し、信頼できる場合だけ実行してください。"
           "この設定で実行しますか？"),
        QMessageBox::Yes | QMessageBox::No, window);
    box.setInformativeText(tr("実行ファイル: %1\nランチャー: %2\n環境変数: %3\nNWChemの追加入力: %4")
                               .arg(program(settings["executable"].toString(), report.executable),
                                    program(settings["launcher"].toString(), report.launcher),
                                    environment.isEmpty() ? tr("なし") : environment.join(", "),
                                    extra.isEmpty() ? tr("なし") : tr("あり（「4 入力プレビュー」で確認）")));
    if (!extra.isEmpty())
        box.setDetailedText(extra.join("\n\n"));
    box.setTextFormat(Qt::PlainText); // Paths and values from a file are shown as written.
    box.setDefaultButton(QMessageBox::No);
    box.setProperty("mcpProtected", true);
    return box.exec() == QMessageBox::Yes;
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
        window->showPanel(this);
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
    settingsForm->setSettings(s);
    for (auto *group : std::as_const(choices))
        group->sync();
    {
        const QSignalBlocker blocker(memory);
        memory->setValue(nwchemSettings(d)["memory_mb"].toInt());
    }
    const bool editable = !window->hasCandidate() && !window->chemistryRunning();
    settingsForm->setEnabled(editable);
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
                settingsForm->setFound(key,
                                       !c.ok   ? c.detail
                                       : named ? tr("✓ PATHから: %1").arg(c.detail)
                                               : QString(),
                                       c.ok);
            }
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
    enqueue->setEnabled(editable && runnable && portable);
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
    QStringList lines;
    if (showingRun) {
        lines << endedDetail(run.state());
        if (run.state() == RunState::Finished)
            lines << (!resultSummary.isEmpty()
                          ? tr("結果: %1（「結果ビューア」に読み込みました）").arg(resultSummary)
                          : tr("結果は「結果ビューアへ」で確認できます。"));
        lines << run.message();
        for (const auto &value : run.plan().manifest["diagnostics"].toArray()) {
            const auto item = value.toObject();
            lines << runDiagnosisText(item["code"].toString());
            if (!item["evidence"].toString().isEmpty())
                lines << item["evidence"].toString();
        }
        lines.removeAll(QString());
    }
    const auto stateText = showingRun ? lines.join('\n')
                           : saved.directory.isEmpty()
                               ? tr("未実行")
                               : tr("入力一式を保存しました（NWChemは起動していません）。");
    if (state->text() != stateText)
        state->setText(stateText);
    bool hasSetting = false;
    for (const auto &value : run.plan().manifest["diagnostics"].toArray())
        hasSetting |= !value.toObject()["setting"].toString().isEmpty();
    diagnosisSetting->setVisible(showingRun && !run.active() && hasSetting);
    // A run that did not end leaves NWChem's database: the calculation can go on from it.
    const bool unfinished =
        showingRun && (run.state() == RunState::Failed || run.state() == RunState::Cancelled ||
                       run.state() == RunState::TimedOut);
    if (const auto key = unfinished ? saved.directory + '|' + runStateKey(run.state()) : QString();
        key != continueKey) {
        continueKey = key;
        continuable = false;
        if (unfinished)
            for (const auto &choice : restartChoices(saved.directory))
                continuable |= choice.mode == "continue" && choice.available();
    }
    continueRun->setVisible(unfinished && continuable);
    location->setText(displayPath(saved.directory));
    location->setToolTip(saved.directory);
    location->setVisible(!saved.directory.isEmpty());
    showRunLink->setVisible(!saved.directory.isEmpty());
    const auto kind = showingRun ? runStateKind(run.state()) : runStateKind(RunState::Prepared);
    statePill->setText(showingRun                  ? runStateName(run.state())
                       : saved.directory.isEmpty() ? QString()
                                                   : runStateName(RunState::Prepared));
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
    openFolder->setVisible(!saved.directory.isEmpty());
    viewResults->setEnabled(!saved.directory.isEmpty() && !resultFiles(saved.directory).isEmpty());
    // The next step once a run has ended well.
    const bool nextStep = showingRun && run.state() == RunState::Finished;
    if (viewResults->property("primary").toBool() != nextStep) {
        viewResults->setProperty("primary", nextStep);
        repolish(viewResults);
    }
    const bool stoppable = run.active() && run.state() != RunState::Stopping;
    stopRun->setVisible(run.active());
    stopRun->setEnabled(stoppable);
    prepare->setVisible(!run.active());
    runningNote->setVisible(run.active());
    state->setVisible(!run.active() && !state->text().isEmpty());

    if (run.active()) {
        startable = false;
        prepare->setEnabled(false);
    }
    window->showRun(run.active(), tr("実行中 %1").arg(elapsedClock(seconds)), stoppable);
    // Always visible in the status bar, whatever the tab.
    if (!run.plan().directory.isEmpty()) {
        runStatus->setText(
            QString("%1%2 · %3")
                .arg(run.active() ? "▶ " : "", statusBarText(run.state()))
                .arg(run.active() ? elapsedClock(seconds) : QFileInfo(run.plan().directory).fileName()));
        runStatus->show();
    }
    if (showingRun)
        progress->showRun(saved.directory, run.state());
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
    const auto directory = run.plan().directory;
    const auto message = QString("%1: %2").arg(endedMessage(run.state()), QFileInfo(directory).fileName());
    // The run status at the right of the status bar says how it ended; a message at the left only
    // adds what the log says once it is read (setResultSummary).
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
    closeOnResults = run.state() == RunState::Finished && runPanelShown();
    if (run.state() == RunState::Failed) { // Why it failed stays in sight.
        if (QFileInfo(QDir(directory).filePath("stderr.err")).size() > 0)
            logChannel->setCurrentIndex(logChannel->findData("stderr.err"));
        showRunPanel();
    }
    emit runFinished(directory, openResults->isChecked() && run.state() == RunState::Finished);
}
void ExecutionPanel::setResultSummary(const QString &directory, const QString &summary) {
    if (directory != run.plan().directory)
        return;
    resultSummary = summary;
    updateRun();
    if (!resultSummary.isEmpty())
        window->statusBar()->showMessage(tr("計算が終了しました: %1").arg(resultSummary), 60000);
}
void ExecutionPanel::stopEditing() {
    busy = true;
    run.disconnect(this);
}
void ExecutionPanel::shutdown(bool keepRunning) {
    queue.shutdown();
    if (keepRunning && run.supervised())
        run.release();
    else
        run.shutdown();
}
void ExecutionPanel::superviseRuns(const QString &program) {
    run.setSupervisor(program);
    // Beside the autosave copies, in the application's data folder.
    const auto folder = QFileInfo(MainWindow::autosaveDirectory()).absolutePath();
    if (folder.isEmpty() || !QDir().mkpath(folder))
        return;
    // A second window on the same account keeps its queue to itself, as before.
    queueLock = std::make_unique<QLockFile>(QDir(folder).filePath("queue.lock"));
    queueLock->setStaleLockTime(0);
    if (queueLock->tryLock(0))
        queue.setStore(QDir(folder).filePath("queue.json"));
    else
        queueLock.reset();
}
QString ExecutionPanel::restoreRuns(bool withQueue) {
    QStringList notes;
    if (withQueue && queueLock)
        queue.restore();
    const int waiting = queue.pending();
    if (waiting)
        notes << tr(
            "前回のキューの待機中 %n 件を復元しました（「計算キュー」の「キューを開始・再開」で続けます）。",
            nullptr, waiting);
    // The run of the last session, if the queue has not taken it up already.
    const auto last = window->preference("execution/running").toString();
    if (!last.isEmpty() && !run.active() && !run.attach(last)) {
        window->setPreference("execution/running", {});
        try {
            const auto entry = readHistoryEntry(last);
            if (entry.valid)
                notes << tr("前回実行していた計算「%1」は%2の状態です（履歴で確認できます）。")
                             .arg(entry.name, runStateName(entry.status));
        } catch (const std::exception &) {
        }
    }
    if (run.active())
        notes
            << tr("実行中の計算「%1」の監視を再開しました。").arg(QFileInfo(run.plan().directory).fileName());
    return notes.join(' ');
}
void ExecutionPanel::applyChromeStyle() {
    const auto pal = static_cast<QWidget *>(window)->palette();
    const QString rules = settingsRules() + R"(
        #executionTab { background: @window; }
        #runPanel { background: @window; border: none; border-top: 1px solid @line; }
        QScrollArea#executionRunDetails, #executionRunDetailsPage { background: @base; border: none; }
        #executionUsage { background: @window; border: 1px solid @line; border-radius: 8px; }
        QLabel#executionStatePill { border-radius: 10px; padding: 2px 9px; font-weight: bold; }
        QLabel#executionRunningNote { background: @tint; color: @accentText; border-radius: 8px; padding: 8px 12px; }
        QLabel[tile="true"] { background: @window; border: 1px solid @line; border-radius: 8px; padding: 4px 6px; }
    )";
    const auto pills = pillRules("QLabel#executionStatePill", "state", ChromeColors(pal));
    setStyleSheet(chromeRules(rules, pal) + pills);
    runPanel->setStyleSheet(chromeRules(rules, pal) + pills);
    // Wide enough for the four criteria in a row, with a scroll bar beside them.
    runCard->setFixedWidth(progress->criteriaWidth() + 30 + style()->pixelMetric(QStyle::PM_ScrollBarExtent));
}
} // namespace mf
