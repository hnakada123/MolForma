// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/execution_panel.h"
#include "model/i18n.h"
#include "nwchem/nwchem.h"
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
    runStatus->setToolTip(tr("実行状況とログを表示します。"));
    runStatus->hide();
    window->statusBar()->addPermanentWidget(runStatus);
    connect(runStatus, &QToolButton::clicked, this, [this] { showRunPanel(); });
    connect(&run, &NwchemRun::changed, this, [this] {
        if (run.state() != previousRunState) {
            const bool ended = runStateActive(previousRunState) && !run.active();
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
    connect(history, &QPushButton::clicked, this, [this] { emit historyRequested(saved.directory); });
    connect(viewResults, &QPushButton::clicked, this, [this] { emit resultsRequested(saved.directory); });
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
    toolbar->installEventFilter(this);
    connect(window, &MainWindow::workspaceGeometryChanged, this, [this] {
        if (runPanel->isVisible())
            placeRunPanel();
    });
    connect(window, &MainWindow::panelChanged, this, [this] {
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
    window->setRunPanelShown(show);
}
bool ExecutionPanel::runPanelShown() const {
    return runPanel->isVisible();
}
void ExecutionPanel::placeRunPanel() {
    auto area = window->runPanelArea();
    // Over "3 実行設定" it starts below the bar: the calculation, "終了したら結果ビューアに読み込む"
    // and "ジョブとして保存" stay in sight.
    if (window->currentPanel() == this)
        area.setTop(toolbar->mapTo(window, QPoint(0, toolbar->height())).y());
    runPanel->setGeometry(area);
}
bool ExecutionPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == toolbar && (event->type() == QEvent::Resize || event->type() == QEvent::Move) &&
        runPanel->isVisible())
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
            return;
        window->trustCurrentExecution();
    }
    run.start(window->document(), baseDirectory());
    showRunPanel();
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
    openFolder->setEnabled(!saved.directory.isEmpty());
    viewResults->setEnabled(!saved.directory.isEmpty() && !resultFiles(saved.directory).isEmpty());
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
        QLabel#executionRunningNote { background: @tint; color: @accentText; border-radius: 8px; padding: 8px 12px; }
        QLabel[tile="true"] { background: @window; border: 1px solid @line; border-radius: 8px; padding: 6px 9px; }
    )";
    const auto pills = pillRules("QLabel#executionStatePill", "state", ChromeColors(pal));
    setStyleSheet(chromeRules(rules, pal) + pills);
    runPanel->setStyleSheet(chromeRules(rules, pal) + pills);
}
} // namespace mf
