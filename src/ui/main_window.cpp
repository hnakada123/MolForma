// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/main_window.h"
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/library.h"
#include "editing/stereochemistry.h"
#include "io/structure_io.h"
#include "model/i18n.h"
#include "model/settings.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_execution.h"
#include "nwchem/nwchem_import.h"
#include "ui/calculation_panel.h"
#include "ui/candidate_controls.h"
#include "ui/coordinates_panel.h"
#include "ui/editor_chrome.h"
#include "ui/execution_panel.h"
#include "ui/history_panel.h"
#include "ui/input_preview_panel.h"
#include "ui/molecule_panel.h"
#include "ui/results_panel.h"
#include "ui/structure_import.h"
#include "ui/theme.h"
#include "ui/workspace_controller.h"
#include <Eigen/Eigenvalues>
#include <QJsonDocument>
#include <QtWidgets>
// Embedded manual and icon (docs/manual.qrc in this static library); outside namespace mf.
static void initManualResources() {
    Q_INIT_RESOURCE(manual);
}
namespace mf {
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), settings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma"),
      storage(settings), chemistry(session) {
    setObjectName("mainWindow");
    setAcceptDrops(true);
    resize(1380, 900);
    setMinimumSize(900, 620);
    buildUi();
    buildMenus();
    connect(workspaceTabs, &QTabWidget::currentChanged, this, &MainWindow::panelChanged);
    connect(&chemistry, &ChemistryJob::ready, this, &MainWindow::chemistryReady);
    connect(&chemistry, &ChemistryJob::failed, this, &MainWindow::chemistryFailed);
    connect(&chemistry, &ChemistryJob::changed, this, [this] {
        moleculePage->setWorkerRunning(chemistryRunning());
        refresh();
    });
    connect(&storage, &DocumentStorage::autosaveRequested, this, &MainWindow::autosave);
    // Commands change the document in undo()/redo(); refresh once the stack index (and so the
    // Undo/Redo texts) has moved. Saving only changes the title.
    connect(&session, &EditorSession::changed, this, &MainWindow::refresh);
    connect(&session, &EditorSession::applied, this,
            [this](const Document &before, const Document &after, bool remote) {
                if (!remote)
                    trackExecution(before, after);
                moleculePage->pruneSelection(after);
            });
    connect(&session, &EditorSession::historyMoved, this,
            [this](bool undo, const QString &name, const Document &before, const Document &after) {
                announceHistory(undo ? tr("元に戻しました") : tr("やり直しました"), name, before, after);
            });
    connect(&undo, &QUndoStack::cleanChanged, this, &MainWindow::refreshTitle);
    Document initial;
    if (settings.value("startupWater", false).toBool())
        placeTemplate(initial, findTemplate("skeleton:water"), Vec::Zero());
    applyExecutionPreferences(initial);
    session.reset(initial);
    mcp.trustedExecution = protectedExecution(document());
    refresh();
    QTimer::singleShot(0, view, &MoleculeView::fit);
    auto saved = settings.value("geometry").toByteArray();
    if (!saved.isEmpty())
        restoreGeometry(saved);
}
MainWindow::~MainWindow() {
    // A normal end (window closed, or MCP's standard input closed) leaves nothing to recover.
    removeAutosave();
    storage.close();
    results->cancelLoad();
    moleculePage->stopEditing();
    calculation->stopEditing();
    execution->stopEditing();
    execution->shutdown();
    cancelWorker();
    session.disconnect(this);
    undo.disconnect(this); // ~QUndoStack signals while clearing, after later members are gone.
}
void MainWindow::safe(const std::function<void()> &operation) {
    try {
        operation();
    } catch (const std::exception &e) {
        QString message = QString::fromUtf8(e.what());
        statusBar()->showMessage(message, 15000);
        QMessageBox::warning(this, tr("操作できません"), message);
    }
}
void MainWindow::trackExecution(const Document &before, const Document &after) {
    // Only a change made outside MCP that itself alters these settings makes them trusted.
    if (mcp.depth == 0 && protectedExecution(before) != protectedExecution(after))
        mcp.trustedExecution = protectedExecution(after);
}
void MainWindow::checkMcpPath(const QString &path) const {
    if (mcp.policy && mcp.depth > 0)
        mcp.policy->checkPath(path);
}
void MainWindow::loadDocument(const Document &d) {
    cancelWorker();
    moleculePage->setSelection({});
    session.reset(d, mcp.depth > 0);
    view->fit();
}
bool MainWindow::replaceUnsavedDocument(const Document &d) {
    if (!confirmDiscard())
        return false;
    storage.setPath({});
    loadDocument(d);
    undo.resetClean();
    return true;
}
void MainWindow::announceHistory(const QString &verb, const QString &name, const Document &from,
                                 const Document &to) {
    // One Undo/Redo covers the molecule and the settings of every tab: say which tab changed,
    // so undoing in "1 分子" does not silently revert a setting the user cannot see.
    const auto structure = [](const Document &d) {
        auto o = d.toJson();
        for (const auto *key : {"nwchem", "nwchem_execution", "job"})
            o.remove(key);
        return o;
    };
    QWidget *page = nullptr;
    if (structure(from) != structure(to))
        page = moleculePage;
    else if (from.data["nwchem_execution"] != to.data["nwchem_execution"])
        page = execution;
    else
        page = calculation; // Calculation conditions, charge and multiplicity.
    const bool elsewhere = workspaceTabs && workspaceTabs->currentWidget() != page;
    statusBar()->showMessage(elsewhere
                                 ? tr("%1: %2（「%3」の内容）")
                                       .arg(verb, name, workspaceTabs->tabText(workspaceTabs->indexOf(page)))
                                 : QString("%1: %2").arg(verb, name),
                             elsewhere ? 10000 : 5000);
}
void MainWindow::edit(const QString &name, const std::function<void(Document &)> &operation, bool preview) {
    if (chemistryRunning())
        fail(tr("整形処理の完了または中止後に編集してください。"));
    if (!session.edit(name, operation, preview, mcp.depth > 0))
        return;
    if (preview)
        statusBar()->showMessage(tr("候補を確認して確定してください。"), 8000);
    else {
        if (!moleculePage->keepsSelection())
            setSelection({});
        statusBar()->showMessage(name, 5000);
    }
}
void MainWindow::confirmCandidate() {
    const auto name = session.candidateName();
    if (session.confirm(mcp.depth > 0)) {
        if (!moleculePage->keepsSelection())
            setSelection({});
        statusBar()->showMessage(name, 5000);
    }
}
void MainWindow::cancelCandidate() {
    session.cancel();
    statusBar()->showMessage(tr("変更候補を取り消しました。"), 4000);
}
void MainWindow::remoteHistory(const QString &action) {
    if (chemistryRunning())
        fail(tr("整形処理の完了または中止後に操作してください。"));
    if (action == "confirm" || action == "cancel") {
        if (!hasCandidate())
            fail(tr("変更候補がありません。"));
        if (action == "confirm")
            confirmCandidate();
        else
            cancelCandidate();
    } else
        session.navigate(action);
}
const Ids &MainWindow::selection() const {
    // Child panels can read the editor context while the molecular workspace is being built.
    static const Ids empty;
    return moleculePage ? moleculePage->selection() : empty;
}
void MainWindow::setSelection(const Ids &ids) {
    moleculePage->setSelection(ids);
}
void MainWindow::sortAtoms(const QString &mode) {
    moleculePage->sortAtoms(mode);
}
void MainWindow::showCoordinateTable() {
    moleculePage->showCoordinateTable();
}
void MainWindow::showPanel(QWidget *panel) {
    workspaceTabs->setCurrentWidget(panel);
}
void MainWindow::showPanel(int index) {
    if (index >= 0 && index < workspaceTabs->count())
        workspaceTabs->setCurrentIndex(index);
}
void MainWindow::showSetting(int tab, const QString &setting, const QString &group) {
    if (setting == "memory_mb")
        tab = 2;
    else if (!group.isEmpty())
        tab = 1;
    showPanel(tab);
    if (tab == 1) {
        if (!group.isEmpty())
            calculation->showDetailGroup(group);
        else if (!setting.isEmpty())
            calculation->showField(setting);
    } else if (tab == 2 && !setting.isEmpty())
        execution->showField(setting);
}
QString MainWindow::settingSectionTitle(const QString &setting) const {
    return calculation->sectionTitle(setting);
}
void MainWindow::showInputLine(int line) {
    showPanel(inputPreview);
    if (line >= 0)
        inputPreview->showLine(line);
}
void MainWindow::commitPendingSettings() {
    execution->commitPendingEdits();
    if (auto *focused = QApplication::focusWidget())
        focused->clearFocus();
}
QWidget *MainWindow::currentPanel() const {
    return workspaceTabs->currentWidget();
}
int MainWindow::currentPanelIndex() const {
    return workspaceTabs->currentIndex();
}
QWidget *MainWindow::workspaceWidget() const {
    return workspaceTabs;
}
QRect MainWindow::runPanelArea() const {
    QRect area(workspaceTabs->mapTo(this, QPoint()), workspaceTabs->size());
    int left = area.right() + 1 - std::clamp(area.width() * 2 / 5, std::min(420, area.width()), 600);
    if (moleculePanelVisible())
        if (const auto properties = moleculePage->propertiesArea(this); !properties.isEmpty())
            left = properties.left();
    area.setLeft(left);
    return area;
}
bool MainWindow::moleculePanelVisible() const {
    return currentPanel() == moleculePage;
}
QString MainWindow::moleculeLayoutMode() const {
    return moleculePage ? moleculePage->layoutMode() : QString("view");
}
void MainWindow::setPanelDiagnostics(QWidget *panel, bool error, const QString &tip) {
    const int index = workspaceTabs->indexOf(panel);
    if (index >= 0) {
        workspaceTabs->tabBar()->setTabTextColor(index,
                                                 error ? errorColor(QMainWindow::palette()) : QColor());
        workspaceTabs->setTabToolTip(index, tip);
    }
    syncWorkflowSteps();
}
void MainWindow::setRunPanelShown(bool shown) {
    if (runPanelAction)
        runPanelAction->setChecked(shown);
}
void MainWindow::trustCurrentExecution() {
    mcp.trustedExecution = protectedExecution(document());
}
void MainWindow::buildUi() {
    auto *central = new QWidget;
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    setCentralWidget(central);
    workspaceTabs = new QTabWidget;
    workspaceTabs->installEventFilter(this);
    workspaceTabs->setObjectName("workspaceTabs");
    workspaceTabs->setDocumentMode(true);
    // The workflow steps of the header switch the tabs, so their own bar stays hidden.
    workspaceTabs->tabBar()->hide();
    outer->addWidget(workspaceTabs, 1);
    moleculePage = new MoleculePanel(*this, session, settings);
    view = moleculePage->canvas();
    coordinates = moleculePage->coordinatePanel();
    workspaceTabs->addTab(moleculePage, tr("1 分子"));
    connect(moleculePage, &MoleculePanel::geometryChanged, this, &MainWindow::workspaceGeometryChanged);
    connect(moleculePage, &MoleculePanel::openRequested, this, &MainWindow::open);
    calculation = new CalculationPanel(this);
    workspaceTabs->addTab(calculation, tr("2 計算条件"));
    execution = new ExecutionPanel(this);
    workspaceTabs->addTab(execution, tr("3 実行設定"));
    inputPreview = new InputPreviewPanel(this);
    workspaceTabs->addTab(inputPreview, tr("4 入力プレビュー"));
    results = new ResultsPanel(this);
    workspaceTabs->addTab(results, tr("結果ビューア"));
    calculationHistory = new HistoryPanel(this, execution->currentRun());
    workspaceTabs->addTab(calculationHistory, tr("履歴"));
    new WorkspaceController(*this, *execution, *calculationHistory, *results);
    connect(workspaceTabs, &QTabWidget::currentChanged, this, [this](int index) {
        auto *page = workspaceTabs->widget(index);
        if (page == inputPreview) {
            execution->commitPendingEdits();
            refreshPanel(inputPreview, true);
        } else
            refreshPanel(page);
    });
    buildHeader();
    outer->insertWidget(0, header);
    candidateBar = new CandidateControls(false);
    connect(candidateBar, &CandidateControls::confirmRequested, this,
            [this] { safe([this] { confirmCandidate(); }); });
    connect(candidateBar, &CandidateControls::cancelRequested, this, &MainWindow::cancelCandidate);
    outer->addWidget(candidateBar);
    connect(workspaceTabs, &QTabWidget::currentChanged, this, [this] {
        candidateBar->setVisible(session.candidate().has_value() &&
                                 workspaceTabs->currentWidget() != moleculePage);
    });
    // The document in brief at the left of the status bar; a message takes its place while shown.
    summary = new QLabel;
    summary->setObjectName("documentSummary");
    statusBar()->addWidget(summary, 1);
    // Hidden explicitly while a message is shown: the status bar alone shows its widgets again when
    // it lays them out anew (as the run status at its right appears), under the message.
    connect(statusBar(), &QStatusBar::messageChanged, summary,
            [this](const QString &message) { summary->setVisible(message.isEmpty()); });
    connect(workspaceTabs, &QTabWidget::currentChanged, this, [this] {
        if (!refreshing)
            refreshSummary();
    });
}
void MainWindow::buildHeader() {
    initManualResources(); // The application icon.
    auto *identity = new QWidget;
    auto *left = new QHBoxLayout(identity);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(10);
    auto *logo = new QLabel;
    logo->setPixmap(QIcon(":/icons/molforma.png").pixmap(QSize(26, 26)));
    left->addWidget(logo);
    auto *name = new QLabel("MolForma");
    name->setObjectName("appName");
    auto nameFont = name->font();
    nameFont.setBold(true);
    nameFont.setPointSizeF(nameFont.pointSizeF() * 1.15);
    name->setFont(nameFont);
    left->addWidget(name);
    auto *rule = new QFrame;
    rule->setFrameShape(QFrame::VLine);
    rule->setFrameShadow(QFrame::Sunken);
    left->addWidget(rule);
    // The open file; its menu creates, opens and saves (the "ファイル" menu has the same).
    documentButton = new QToolButton;
    documentButton->setObjectName("documentButton");
    documentButton->setIcon(lineIcon("document"));
    documentButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    documentButton->setPopupMode(QToolButton::InstantPopup);
    documentButton->setToolTip(tr("新規・開く・保存・SMILESから作成"));
    left->addWidget(documentButton);
    // The workflow: one step per tab, in the order of building, setting up and running a calculation.
    workflowSteps = new QWidget;
    workflowSteps->setObjectName("workflowSteps");
    auto *steps = new QHBoxLayout(workflowSteps);
    steps->setContentsMargins(0, 0, 0, 0);
    steps->setSpacing(2);
    stepGroup = new QButtonGroup(this);
    static const QRegularExpression numbered(R"(^(\d) (.+)$)");
    bool numberedSteps = true;
    for (int i = 0; i < workspaceTabs->count(); ++i) {
        const auto text = workspaceTabs->tabText(i);
        const auto match = numbered.match(text);
        if (numberedSteps && !match.hasMatch()) {
            numberedSteps = false; // Results and history are looked at, not steps of a calculation.
            auto *gap = new QFrame;
            gap->setFrameShape(QFrame::VLine);
            gap->setFrameShadow(QFrame::Sunken);
            steps->addSpacing(6);
            steps->addWidget(gap);
            steps->addSpacing(6);
        }
        auto *b = new QToolButton;
        b->setObjectName(QString("workflowStep_%1").arg(i + 1));
        b->setText(match.hasMatch() ? match.captured(2) : text);
        if (match.hasMatch()) {
            b->setIcon(stepIcon(match.captured(1).toInt()));
            b->setIconSize(QSize(18, 18));
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
        b->setAccessibleName(text);
        b->setCheckable(true);
        steps->addWidget(b);
        stepGroup->addButton(b, i);
        connect(b, &QToolButton::clicked, this, [this, i] { workspaceTabs->setCurrentIndex(i); });
    }
    connect(workspaceTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (auto *b = stepGroup->button(index))
            b->setChecked(true);
    });
    stepGroup->button(workspaceTabs->currentIndex())->setChecked(true);
    auto *actions = new QWidget;
    auto *right = new QHBoxLayout(actions);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(6);
    // One history for the molecule and the settings of every tab, so Undo/Redo sit in the header.
    const auto history = [&](const QString &text, const QString &icon, const QString &objectName) {
        auto *b = new QPushButton;
        b->setObjectName(objectName);
        b->setIcon(lineIcon(icon));
        b->setIconSize(QSize(18, 18));
        b->setAccessibleName(text);
        b->setToolTip(text);
        right->addWidget(b);
        return b;
    };
    undoButton = history(tr("元に戻す"), "undo", "editorUndo");
    connect(undoButton, &QPushButton::clicked, this, [this] {
        safe([this] {
            if (session.candidate())
                cancelCandidate();
            else
                undo.undo();
        });
    });
    redoButton = history(tr("やり直す"), "redo", "editorRedo");
    connect(redoButton, &QPushButton::clicked, this, [this] {
        safe([this] {
            if (!session.candidate())
                undo.redo();
        });
    });
    runPill = new QToolButton;
    runPill->setObjectName("runStatePill");
    runPill->setToolTip(tr("実行状況とログを表示します。"));
    runPill->hide();
    connect(runPill, &QToolButton::clicked, this, [this] { execution->showRunPanel(); });
    right->addSpacing(4);
    right->addWidget(runPill);
    headerStop = new QToolButton;
    headerStop->setObjectName("headerStop");
    headerStop->setText(tr("停止"));
    headerStop->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    headerStop->setToolTip(tr("実行中のNWChemを停止します（確認してから停止）。"));
    headerStop->hide();
    right->addWidget(headerStop);
    connect(headerStop, &QToolButton::clicked, this, [this] { execution->confirmStop(); });
    runButton = new QToolButton;
    runButton->setObjectName("runButton");
    runButton->setText(tr("計算を実行"));
    runButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    runButton->setToolTip(tr("入力一式を新しいジョブフォルダーに保存し、NWChemを起動します（F5）。"));
    right->addSpacing(4);
    right->addWidget(runButton);
    header = new HeaderBar(identity, workflowSteps, actions);
    header->setObjectName("headerBar");
    header->setAttribute(Qt::WA_StyledBackground);
    syncWorkflowSteps();
}
void MainWindow::buildMenus() {
    auto *file = menuBar()->addMenu(tr("ファイル"));
    // Every command has an icon; options that are switched on or chosen show their check mark instead.
    auto action = [&](QMenu *m, const QString &text, const QKeySequence &key, const QString &icon, auto fn) {
        auto *a = m->addAction(text);
        if (!icon.isEmpty())
            a->setIcon(lineIcon(icon));
        if (!key.isEmpty())
            a->setShortcut(key);
        connect(a, &QAction::triggered, this, [this, fn] { safe(fn); });
        return a;
    };
    const QList<QAction *> documentActions{
        action(file, tr("新規"), QKeySequence::New, "new", [this] { newDocument(); }),
        action(file, tr("開く…"), QKeySequence::Open, "folder", [this] { open(); }),
        action(file, tr("SMILESから作成…"), {}, "ring", [this] { moleculePage->importSmiles(); })};
    for (int i = 0; i < documentActions.size(); ++i)
        documentActions[i]->setObjectName(QStringList{"newAction", "openAction", "smilesAction"}[i]);
    recentMenu = file->addMenu(tr("最近使ったファイル"));
    recentMenu->setObjectName("recentMenu");
    recentMenu->setIcon(lineIcon("recent"));
    refreshRecent();
    action(file, tr("SMILESから断片を追加…"), {}, "ringAdd",
           [this] { moleculePage->addFragmentFromSmiles(); });
    action(file, tr("ファイルから断片を追加…"), {}, "import",
           [this] { moleculePage->addFragmentFromFile(); });
    file->addSeparator();
    auto *saveAction = action(file, tr("プロジェクトを保存"), QKeySequence::Save, "save", [this] { save(); });
    auto *saveAsAction =
        action(file, tr("名前を付けて保存…"), QKeySequence::SaveAs, "saveAs", [this] { save(true); });
    action(file, tr("構造を書き出す…"), {}, "export", [this] { exportFile(); });
    action(file, tr("分子全体をXYZでコピー"), {}, "copy", [this] {
        if (resolveCandidate())
            QApplication::clipboard()->setText(document().xyz());
    });
    action(file, tr("XYZを貼り付けて置き換え…"), {}, "paste", [this] {
        bool ok;
        QString text = QInputDialog::getMultiLineText(
            this, tr("XYZを貼り付けて置き換え"),
            tr("原子数、コメント、元素と座標（Å）。今の構造を置き換える候補を作ります。"),
            QApplication::clipboard()->text(), &ok);
        if (ok) {
            auto next = xyzStructure(text);
            // Like the coordinate table, keep calculation/execution settings and the envelope.
            edit(tr("XYZを読込"), [next](Document &d) { replaceCoordinateStructure(d, next); }, true);
            view->fit();
        }
    });
    file->addSeparator();
    action(file, tr("終了"), QKeySequence::Quit, "quit", [this] { close(); });
    auto *editMenu = menuBar()->addMenu(tr("編集"));
    undoAction = action(editMenu, tr("元に戻す"), QKeySequence::Undo, "undo", [this] {
        if (session.candidate())
            cancelCandidate();
        else
            undo.undo();
    });
    undoAction->setObjectName("undoAction");
    auto *redo = redoAction = action(editMenu, tr("やり直す"), QKeySequence::Redo, "redo", [this] {
        if (!session.candidate())
            undo.redo();
    });
    redo->setObjectName("redoAction");
    redo->setShortcuts({QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")});
    // Candidate keys work in every tab; editing keys only while the focus is in "1 分子", so text
    // fields keep their own keys and Delete in other tables never deletes atoms.
    confirmAction = action(editMenu, tr("変更を確定"), QKeySequence("Ctrl+Return"), "check", [this] {
        if (!session.candidate())
            fail(tr("確定する変更候補がありません。"));
        confirmCandidate();
    });
    confirmAction->setObjectName("confirmAction");
    // Listed as Ctrl+Enter like the candidate buttons; the main Return key works the same.
    confirmAction->setShortcuts({QKeySequence("Ctrl+Enter"), QKeySequence("Ctrl+Return")});
    action(editMenu, tr("候補を取消・選択を解除"), QKeySequence(Qt::Key_Escape), "close", [this] {
        if (session.candidate())
            cancelCandidate();
        else
            setSelection({});
        moleculePage->cancelGrowth();
    })->setObjectName("escapeAction");
    editMenu->addSeparator();
    moleculePage->addEditActions(editMenu);
    // Preferences hold the executable, launcher and environment: only the user may change them.
    auto *preferencesAction =
        action(editMenu, tr("環境設定…"), QKeySequence::Preferences, "settings", [this] { preferences(); });
    preferencesAction->setObjectName("preferencesAction");
    preferencesAction->setProperty("mcpProtected", true);
    // Checks and transformations of the whole structure.
    auto *toolsMenu = menuBar()->addMenu(tr("ツール"));
    action(toolsMenu, tr("構造を検査…"), {}, "inspect", [this] {
        moleculePage->showReport();
    })->setObjectName("reportAction");
    action(toolsMenu, tr("点群・対称化…"), {}, "symmetry", [this] {
        moleculePage->symmetryDialog();
    })->setObjectName("symmetryAction");
    action(toolsMenu, tr("Z-matrix（内部座標）…"), {}, "angle", [this] {
        moleculePage->zMatrixDialog();
    })->setObjectName("zMatrixAction");
    action(toolsMenu, tr("配座探索…"), {}, "newman", [this] {
        bool ok;
        const int count =
            QInputDialog::getInt(this, tr("配座探索"),
                                 tr("回転可能な結合をランダムに回して力場で最適化し、エネルギーの低い"
                                    "配座を探します（MMFF94、使えない場合UFF）。\n残す配座の数"),
                                 10, 1, 100, 1, &ok);
        if (ok)
            runWorker("conformers", {}, {}, count);
    })->setObjectName("conformerAction");
    auto *regenerate = action(toolsMenu, tr("3Dを再生成"), {}, "cube", [this] { runWorker("regenerate"); });
    regenerate->setObjectName("regenerateAction");
    regenerate->setToolTip(tr("結合から3D座標を作り直し、力場で整えます。結果は候補として表示します。"));
    action(toolsMenu, tr("構造を比較・重ね合わせ…"), {}, "compare", [this] {
        moleculePage->compareDialog();
    })->setObjectName("compareAction");
    // NWChem runs from any tab, not only from the button in "3 実行設定".
    auto *calculationMenu = menuBar()->addMenu(tr("計算"));
    auto *runAction = action(calculationMenu, tr("計算を実行"), QKeySequence(Qt::Key_F5), "play",
                             [this] { execution->startFromMenu(); });
    runAction->setObjectName("runCalculationAction");
    stopAction = action(calculationMenu, tr("計算を停止…"), {}, "stop", [this] { execution->confirmStop(); });
    stopAction->setObjectName("stopCalculationAction");
    stopAction->setEnabled(execution->running()); // ExecutionPanel follows the run.
    runPanelAction = action(calculationMenu, tr("実行状況とログ"), QKeySequence("Ctrl+Shift+L"), {},
                            [this] { execution->showRunPanel(!execution->runPanelShown()); });
    runPanelAction->setObjectName("runPanelAction");
    runPanelAction->setCheckable(true);
    action(calculationMenu, tr("ジョブとして保存"), {}, "newFolder", [this] {
        workspaceTabs->setCurrentWidget(execution);
        execution->saveBundle();
    })->setObjectName("saveBundleAction");
    calculationMenu->addSeparator();
    action(calculationMenu, tr("結果ファイルを開く…"), {}, "chart", [this] {
        workspaceTabs->setCurrentWidget(results);
        if (auto *open = results->findChild<QAbstractButton *>("resultsOpen"))
            open->click();
    });
    action(calculationMenu, tr("計算の履歴"), {}, "history",
           [this] { workspaceTabs->setCurrentWidget(calculationHistory); });
    auto *viewMenu = menuBar()->addMenu(tr("表示"));
    moleculePage->addViewActions(viewMenu);
    // The tabs with the icons of the header's steps; results and history are not steps.
    auto *tabsMenu = viewMenu->addMenu(lineIcon("tabs"), tr("タブを表示"));
    for (int i = 0; i < workspaceTabs->count(); ++i) {
        const auto *page = workspaceTabs->widget(i);
        const bool step = page != results && page != calculationHistory;
        auto *a = action(tabsMenu, workspaceTabs->tabText(i), QKeySequence(QString("Ctrl+%1").arg(i + 1)),
                         step              ? ""
                         : page == results ? "chart"
                                           : "history",
                         [this, i] { workspaceTabs->setCurrentIndex(i); });
        a->setObjectName(QString("showTab%1Action").arg(i + 1));
        if (step)
            a->setIcon(stepIcon(i + 1));
    }
    viewMenu->addSeparator();
    action(viewMenu, tr("画面をPNGで書き出す…"), {}, "image", [this] {
        QString p = QFileDialog::getSaveFileName(this, tr("画面を保存"), "molforma.png", "PNG (*.png)");
        if (!p.isEmpty() && !grab().save(p))
            fail(tr("画像を書き出せません。"));
    });
    auto *help = menuBar()->addMenu(tr("ヘルプ"));
    action(help, tr("操作ガイド・マニュアル"), QKeySequence::HelpContents, "book", [this] {
        showManual();
    })->setObjectName("manualAction");
    action(help, tr("キーボードショートカット一覧"), {}, "keyboard", [this] {
        showShortcuts();
    })->setObjectName("shortcutsAction");
    action(help, tr("MolFormaについて"), {}, "info", [this] {
        QMessageBox::about(
            this, "MolForma",
            tr("MolForma %1\nNWChem向けの分子作成・計算入力・実行・結果表示（Qt6 / C++）\n"
               "ライセンス: GNU GPL バージョン2以降\n"
               "化学ファイル・力場: Open Babel\n立体判定・残基認識（RDKit）: %2\n3D描画: %3")
                .arg(MOLFORMA_VERSION, hasStereochemistryBackend() ? tr("有効") : tr("無効"),
                     view->rendererName() == "vtk" ? tr("VTK（OpenGL）") : tr("QPainter（CPU）")));
    });
    // The header's document menu and run button, and the "表示" button over the 3D view.
    auto *documentMenu = new QMenu(documentButton);
    documentMenu->addActions(documentActions.mid(0, 2));
    documentMenu->addMenu(recentMenu);
    documentMenu->addAction(documentActions[2]);
    documentMenu->addSeparator();
    documentMenu->addAction(saveAction);
    documentMenu->addAction(saveAsAction);
    documentButton->setMenu(documentMenu);
    connect(runButton, &QToolButton::clicked, runAction, &QAction::trigger);
    connect(runAction, &QAction::enabledChanged, runButton, &QToolButton::setEnabled);
    applyChromeStyle();
}
void MainWindow::refresh() {
    if (!moleculePage)
        return;
    refreshing = true;
    moleculePage->refresh();
    candidateBar->refresh(session.candidateName(), session.candidate()
                                                       ? atomChanges(document(), *session.candidate())
                                                       : AtomChanges{});
    candidateBar->setVisible(session.candidate().has_value() &&
                             workspaceTabs->currentWidget() != moleculePage);
    // Undo/Redo show the operation they act on, and are off while there is none.
    undoButton->setEnabled(session.candidate() || undo.canUndo());
    redoButton->setEnabled(!session.candidate() && undo.canRedo());
    undoButton->setToolTip(session.candidate() ? tr("候補を取消")
                           : undo.canUndo()    ? tr("元に戻す: %1").arg(undo.undoText())
                                               : tr("元に戻す操作はありません"));
    redoButton->setToolTip(undo.canRedo() ? tr("やり直す: %1").arg(undo.redoText())
                                          : tr("やり直す操作はありません"));
    if (undoAction) {
        undoAction->setEnabled(session.candidate() || undo.canUndo());
        redoAction->setEnabled(!session.candidate() && undo.canRedo());
        confirmAction->setEnabled(session.candidate().has_value());
        undoAction->setText(session.candidate() ? tr("候補を取消")
                            : undo.canUndo()    ? tr("元に戻す: %1").arg(undo.undoText())
                                                : tr("元に戻す"));
        redoAction->setText(undo.canRedo() ? tr("やり直す: %1").arg(undo.redoText()) : tr("やり直す"));
    }
    refreshSummary();
    refreshTitle();
    refreshing = false;
    refreshPanels();
}
void MainWindow::refreshSummary() {
    const auto &d = displayDocument();
    // In the result viewer and the history the molecule is not the one shown: name it.
    const auto *page = workspaceTabs->currentWidget();
    summary->setText(tr("%1%2 · %3 原子 · %4 結合 · 電荷 %5 · 多重度 %6 · 点電荷 %7%8")
                         .arg(page == results || page == calculationHistory ? tr("編集中の分子: ") : "")
                         .arg(d.atoms.isEmpty() ? tr("原子なし") : d.formula())
                         .arg(d.atoms.size())
                         .arg(d.bonds.size())
                         .arg(d.charge)
                         .arg(d.multiplicity)
                         .arg(d.pointCharges.size())
                         .arg(session.candidate() ? tr(" · 変更候補あり") : ""));
}
void MainWindow::refreshTitle() {
    const auto path = projectPath();
    const auto name = path.isEmpty() ? tr("新規分子") : QFileInfo(path).fileName();
    const auto mark = undo.isClean() && !session.candidate() ? "" : " *";
    setWindowTitle(QString("%1%2 — MolForma").arg(name, mark));
    if (documentButton) {
        documentButton->setText(documentButton->fontMetrics().elidedText(name, Qt::ElideMiddle, 220) + mark);
        documentButton->setToolTip(tr("%1\n新規・開く・保存・SMILESから作成")
                                       .arg(path.isEmpty() ? name : QDir::toNativeSeparators(path)));
    }
}
void MainWindow::showRun(bool active, const QString &text, bool stoppable) {
    if (stopAction)
        stopAction->setEnabled(stoppable);
    if (!runPill)
        return;
    runPill->setText(text);
    runPill->setVisible(active);
    headerStop->setVisible(active);
    headerStop->setEnabled(stoppable);
    runButton->setVisible(!active);
}
void MainWindow::syncWorkflowSteps() {
    if (!stepGroup)
        return;
    for (int i = 0; i < workspaceTabs->count(); ++i) {
        auto *b = stepGroup->button(i);
        if (!b)
            continue;
        const auto tip = workspaceTabs->tabToolTip(i);
        b->setToolTip(tr("「%1」を表示します（Ctrl+%2）%3")
                          .arg(workspaceTabs->tabText(i))
                          .arg(i + 1)
                          .arg(tip.isEmpty() ? QString() : "\n" + tip));
        // The input preview marks an input that cannot be generated, as on its tab.
        const bool failed = workspaceTabs->tabBar()->tabTextColor(i).isValid();
        if (b->property("failed").toBool() != failed) {
            b->setProperty("failed", failed);
            b->style()->unpolish(b);
            b->style()->polish(b);
        }
    }
}
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == workspaceTabs && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        emit workspaceGeometryChanged();
    return QMainWindow::eventFilter(watched, event);
}
void MainWindow::changeEvent(QEvent *e) {
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && !stylingChrome) {
        applyChromeStyle();
        refresh(); // Texts colored in their markup, such as the residue links.
    }
}
void MainWindow::applyChromeStyle() {
    if (!header)
        return;
    QScopedValueRollback guard(stylingChrome, true);
    const ChromeColors c(QMainWindow::palette());
    const auto style = [&](QWidget *w, const QString &rules) {
        if (w)
            w->setStyleSheet(chromeRules(rules, QMainWindow::palette()));
    };
    style(header, R"(
        #headerBar { background: @base; border-bottom: 1px solid @line; }
        QToolButton { border: none; border-radius: 6px; padding: 5px 9px; color: @text; }
        QToolButton:hover { background: @hover; }
        QToolButton:checked { background: @tint; color: @accentText; font-weight: bold; }
        QToolButton[failed="true"] { color: @error; }
        QToolButton#documentButton { color: @muted; }
        QPushButton { border: 1px solid @line; border-radius: 6px; background: @base; padding: 6px; }
        QPushButton:hover { background: @hover; }
        QPushButton:disabled { border-color: @hover; }
        QToolButton#runButton { background: @accent; color: @onAccent; font-weight: bold; padding: 6px 14px; }
        QToolButton#runButton:hover { background: @accentHover; }
        QToolButton#runButton:disabled { background: @line; color: @muted; }
        QToolButton#runStatePill { background: @tint; color: @accentText; border-radius: 12px; padding: 4px 11px;
                                   font-weight: bold; }
        QToolButton#runStatePill:hover { background: @hover; }
        QToolButton#headerStop { border: 1px solid @error; color: @error; font-weight: bold; padding: 5px 12px; }
        QToolButton#headerStop:hover { background: @badTint; }
    )");
    runButton->setIcon(lineIcon("play", c.onAccent));
    headerStop->setIcon(lineIcon("stop", c.bad));
    moleculePage->applyChromeStyle();
    candidateBar->applyChromeStyle();
    style(statusBar(), R"(
        QStatusBar { background: @base; border-top: 1px solid @line; color: @text; }
        QStatusBar::item { border: none; }
        QLabel#documentSummary { color: @muted; padding: 2px 6px; }
    )");
    if (coordinates)
        coordinates->applyChromeStyle();
    if (calculation)
        calculation->applyChromeStyle();
    if (execution)
        execution->applyChromeStyle();
    if (inputPreview)
        inputPreview->applyChromeStyle();
    if (results)
        results->applyChromeStyle();
    if (calculationHistory)
        calculationHistory->applyChromeStyle();
}
QList<QWidget *> MainWindow::workspacePanels() const {
    return {coordinates, calculation, execution, inputPreview};
}
void MainWindow::refreshPanels() {
    for (auto *panel : workspacePanels()) {
        if (!panel)
            continue;
        // Hidden panels of a shown window catch up when shown (or before MCP reads the GUI).
        if (isVisible() && !panel->isVisible()) {
            stalePanels.insert(panel);
            if (panel == inputPreview)
                inputPreview->refreshBadge();
        } else
            refreshPanel(panel, true);
    }
}
void MainWindow::refreshPanel(QWidget *panel, bool always) {
    if (!panel || (!always && !stalePanels.contains(panel)))
        return;
    stalePanels.remove(panel);
    if (panel == coordinates)
        coordinates->refresh();
    else if (panel == calculation)
        calculation->refresh();
    else if (panel == execution)
        execution->refresh();
    else if (panel == inputPreview)
        inputPreview->refresh();
}
void MainWindow::refreshStalePanels() {
    for (auto *panel : workspacePanels())
        refreshPanel(panel);
}
bool MainWindow::resolveCandidate() {
    if (!session.candidate())
        return true;
    QMessageBox box(QMessageBox::Question, tr("変更候補があります"),
                    tr("保存・読込の前に候補の扱いを選択してください。"), QMessageBox::NoButton, this);
    auto *yes = box.addButton(tr("確定して続ける"), QMessageBox::AcceptRole);
    auto *no = box.addButton(tr("取消して続ける"), QMessageBox::DestructiveRole);
    box.addButton(tr("編集に戻る"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == yes) {
        confirmCandidate();
        return true;
    }
    if (box.clickedButton() == no) {
        cancelCandidate();
        return true;
    }
    return false;
}
bool MainWindow::confirmDiscard() {
    execution->commitPendingEdits();
    if (auto *focused = QApplication::focusWidget(); focused && calculation->isAncestorOf(focused))
        focused->clearFocus();
    if (!resolveCandidate())
        return false;
    if (undo.isClean())
        return true;
    auto answer = QMessageBox::question(this, tr("未保存の変更"), tr("現在のプロジェクトを保存しますか？"),
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                        QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    return answer != QMessageBox::Save || save();
}
void MainWindow::newDocument() {
    if (!confirmDiscard())
        return;
    // A new, empty project like a freshly opened file: nothing to save until it is edited.
    Document next;
    applyExecutionPreferences(next);
    clearProjectPath();
    loadDocument(next);
    removeAutosave();
    view->viewFrom(2); // A new molecule starts from the front, whatever the last one was turned to.
    workspaceTabs->setCurrentWidget(moleculePage);
    statusBar()->showMessage(tr("新しい分子を作成しました。"), 4000);
}
void MainWindow::open() {
    QString p = QFileDialog::getOpenFileName(
        this, tr("分子を開く"), settings.value("directory").toString(),
        tr("分子 (*.molforma.json *.json *.xyz *.mol *.sdf *.mol2 *.cml *.pdb *.ent *.nw *.gjf *.com);;"
           "NWChem入力 (*.nw);;Gaussian入力 (*.gjf *.com);;すべて (*)"));
    if (p.isEmpty() || !confirmDiscard())
        return;
    loadPath(p);
}
void MainWindow::loadPath(const QString &p) {
    checkMcpPath(p); // Also for recent files and other menu paths reached over MCP.
    // Reading errors name the file (the messages themselves give the line where they can).
    const auto reading = [&](const std::function<Document()> &read) -> Document {
        try {
            return read();
        } catch (const std::exception &e) {
            fail(tr("%1 を読み込めません。\n%2").arg(QFileInfo(p).fileName(), QString::fromUtf8(e.what())));
        }
    };
    const auto base = document().project();
    const auto options = chooseStructureImport(this, p);
    if (!options)
        return;
    Document next;
    if (QFileInfo(p).suffix().toLower() == "nw") {
        NwchemImport imported;
        reading([&] {
            imported = importNwchemInput(QString::fromUtf8(readFile(p)));
            return imported.document;
        });
        next = imported.document;
        if (!imported.notes.isEmpty()) {
            QStringList items;
            for (const auto &note : imported.notes.mid(0, 20))
                items << tr("・%1").arg(note);
            QMessageBox::information(this, tr("NWChem入力の取込み"),
                                     tr("構造・電荷・多重度と、MolFormaで扱える計算条件を取り込みました。\n\n"
                                        "取り込まなかった項目:\n%1")
                                         .arg(items.join('\n')));
        }
    } else
        next = reading([&] { return importStructure(p, options->model, options->altloc, options->frame); });
    if (base != document().project())
        fail(tr("読込中に分子が変更されました。読込をやり直してください。"));
    if (!p.endsWith(".json", Qt::CaseInsensitive))
        applyExecutionPreferences(next); // Structure files carry no execution settings.
    loadDocument(next);
    storage.setPath(p.endsWith(".json", Qt::CaseInsensitive) ? p : "");
    settings.setValue("directory", QFileInfo(p).absolutePath());
    addRecent(p);
    refresh();
    // A three-dimensional structure is seen at an angle like in the result viewer, so atoms behind
    // each other along an axis stay visible; flat molecules keep the front view for editing.
    if (document().atoms.size() >= 4) {
        Vec mean = Vec::Zero();
        for (const auto &a : document().atoms)
            mean += a.position;
        mean /= document().atoms.size();
        Mat spread = Mat::Zero();
        for (const auto &a : document().atoms)
            spread += (a.position - mean) * (a.position - mean).transpose();
        if (Eigen::SelfAdjointEigenSolver<Mat>(spread / document().atoms.size()).eigenvalues()(0) > 0.1) {
            view->setAngles(-25, 25, 0);
            view->fit();
            emit view->orientationChanged(); // The rotation sliders follow.
            return;
        }
    }
    view->viewFrom(2);
}
bool MainWindow::save(bool as) {
    execution->commitPendingEdits();
    if (auto *focused = QApplication::focusWidget(); focused && calculation->isAncestorOf(focused))
        focused->clearFocus(); // Include the active calculation field when saving via Ctrl+S.
    if (!resolveCandidate())
        return false;
    QString p = projectPath();
    if (as || p.isEmpty())
        p = QFileDialog::getSaveFileName(
            this, tr("プロジェクトを保存"),
            p.isEmpty()
                ? (document().atoms.isEmpty() ? QString("molecule") : document().formula()) + ".molforma.json"
                : p,
            tr("MolFormaプロジェクト (*.molforma.json)"));
    if (p.isEmpty())
        return false;
    if (!p.endsWith(".json", Qt::CaseInsensitive))
        p += ".molforma.json";
    checkMcpPath(p); // Ctrl+S reuses the project path without a file dialog.
    storage.save(document(), p);
    undo.setClean();
    addRecent(p);
    removeAutosave(); // Saved work needs no recovery copy.
    refresh();
    return true;
}
void MainWindow::exportFile() {
    if (!resolveCandidate())
        return;
    QString p = QFileDialog::getSaveFileName(this, tr("構造を書き出す"), "molecule.xyz",
                                             tr("XYZ (*.xyz);;SDF (*.sdf);;MOL (*.mol);;Mol2 (*.mol2);;CML "
                                                "(*.cml);;PDB (*.pdb);;Gaussian入力 (*.gjf)"));
    if (!p.isEmpty())
        exportStructure(document(), p);
}
void MainWindow::runWorker(const QString &mode, const QString &smiles, const Ids &moving, int count) {
    chemistry.start({mode, smiles, moving, count, mcp.depth > 0});
    statusBar()->showMessage(tr("構造を生成・整形しています…"));
}
void MainWindow::chemistryReady(const ChemistryResult &result) {
    const auto &request = result.request;
    int chosen = 0;
    if (request.mode == "conformers" && !request.remote && result.conformers.size() > 1) {
        const double lowest = result.conformers[0].energy;
        QStringList labels;
        for (int i = 0; i < result.conformers.size(); ++i)
            labels << tr("%1: ΔE %2 kcal/mol（力場エネルギー %3）")
                          .arg(i + 1)
                          .arg(result.conformers[i].energy - lowest, 0, 'f', 2)
                          .arg(result.conformers[i].energy, 0, 'f', 2);
        bool ok;
        const auto item = QInputDialog::getItem(
            this, tr("配座探索"), tr("%1個の配座が見つかりました。候補にする配座").arg(labels.size()), labels,
            0, false, &ok);
        if (!ok)
            fail(tr("配座の選択を取り消しました。"));
        chosen = labels.indexOf(item);
    }
    const bool proposed = chemistry.propose(result, chosen);
    if (request.mode == "smiles")
        view->fit();
    else
        view->keepInView();
    if (request.mode == "conformers") {
        statusBar()->showMessage(proposed ? tr("配座 %1/%2 を候補にしました。「変更を確定」で適用します。")
                                                .arg(chosen + 1)
                                                .arg(result.conformers.size())
                                          : tr("配座探索: 別の配座は見つからず、現在の構造のままです"
                                               "（回転できる結合がない分子など）。"),
                                 10000);
    } else
        statusBar()->showMessage(tr("「%1」の候補を作成しました。確認して「変更を確定」してください。")
                                     .arg(session.candidateName()),
                                 10000);
}
void MainWindow::chemistryFailed(const ChemistryRequest &request, const QString &error, bool starting) {
    statusBar()->clearMessage();
    if (starting) {
        QMessageBox::warning(this, tr("処理を起動できません"), error);
        return;
    }
    const bool smiles = request.mode == "smiles" || request.mode == "fragment";
    QMessageBox box(QMessageBox::Warning,
                    smiles                         ? tr("SMILESから作成できません")
                    : request.mode == "conformers" ? tr("配座探索できません")
                                                   : tr("構造を整形できません"),
                    error, QMessageBox::Close, this);
    auto *retry =
        smiles && !request.remote ? box.addButton(tr("SMILESを修正…"), QMessageBox::AcceptRole) : nullptr;
    box.exec();
    if (retry && box.clickedButton() == retry)
        safe([&] {
            request.mode == "smiles" ? moleculePage->importSmiles(request.smiles)
                                     : moleculePage->addFragmentFromSmiles(request.smiles);
        });
}
void MainWindow::cancelWorker() {
    if (chemistry.cancel())
        statusBar()->showMessage(tr("処理を中止しました。"), 4000);
}
QStringList MainWindow::recentFiles() const {
    return storage.recentFiles();
}
void MainWindow::addRecent(const QString &path) {
    storage.remember(path);
    refreshRecent();
}
void MainWindow::refreshRecent() {
    if (!recentMenu)
        return;
    recentMenu->clear();
    const auto list = recentFiles();
    for (int i = 0; i < list.size(); ++i) {
        const QFileInfo info(list[i]);
        auto *a = recentMenu->addAction(
            lineIcon("document"),
            QString("&%1 %2  —  %3").arg((i + 1) % 10).arg(info.fileName(), info.absolutePath()));
        a->setEnabled(info.isFile());
        a->setToolTip(list[i]);
        a->setProperty("mcpPath", true); // Blocked for MCP while --mcp-allow-dir restricts paths.
        connect(a, &QAction::triggered, this, [this, file = list[i]] {
            safe([&] {
                if (confirmDiscard())
                    loadPath(file);
            });
        });
    }
    if (!list.isEmpty())
        recentMenu->addSeparator();
    auto *clear = recentMenu->addAction(lineIcon("eraser"), tr("一覧を消去"));
    clear->setEnabled(!list.isEmpty());
    connect(clear, &QAction::triggered, this, [this] {
        settings.remove("recentFiles");
        refreshRecent();
    });
}
void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
    const auto urls = e->mimeData()->urls();
    if (!urls.isEmpty() && urls.first().isLocalFile())
        e->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent *e) {
    const auto urls = e->mimeData()->urls();
    if (urls.isEmpty() || !urls.first().isLocalFile())
        return;
    e->acceptProposedAction();
    const auto file = urls.first().toLocalFile();
    // Open after the drop finishes, so dialogs do not run inside the drag-and-drop loop.
    QTimer::singleShot(0, this, [this, file] { safe([&] { openDropped(file); }); });
}
bool MainWindow::useSample(const QString &resource, const QString &target) {
    const auto name = QFileInfo(resource).fileName();
    if (name.endsWith(".molforma-part.json")) {
        moleculePage->loadPartFile(resource);
        statusBar()->showMessage(tr("%1 をマイ部品に読み込みました。").arg(name), 5000);
        return true;
    }
    if (!confirmDiscard())
        return false;
    // A copy where the user keeps it, so that it is saved and calculated like any project.
    QString p = target;
    if (p.isEmpty()) {
        auto *parent = QApplication::activeModalWidget();
        p = QFileDialog::getSaveFileName(
            parent ? parent : this, tr("練習データを保存して開く"),
            QDir(settings.value("directory", QDir::homePath()).toString()).filePath(name),
            name.endsWith(".sdf") ? tr("構造ファイル (*.sdf)")
                                  : tr("MolFormaプロジェクト (*.molforma.json)"));
        if (p.isEmpty())
            return false;
    }
    checkMcpPath(p);
    writeFile(p, readFile(resource));
    loadPath(p);
    return true;
}
void MainWindow::openDropped(const QString &file) {
    checkMcpPath(file);
    const auto suffix = QFileInfo(file).suffix().toLower();
    // Calculation outputs and orbital/density files go to the result viewer.
    if (QStringList{"out", "log", "cube", "cub", "molden", "mld", "molde"}.contains(suffix)) {
        workspaceTabs->setCurrentWidget(results);
        results->openFile(file);
        return;
    }
    if (QFileInfo(file).isDir()) {
        workspaceTabs->setCurrentWidget(results);
        results->openDirectory(file);
        return;
    }
    if (confirmDiscard())
        loadPath(file);
}
QString MainWindow::autosaveDirectory() {
    return DocumentStorage::autosaveDirectory();
}
void MainWindow::startAutosave() {
    storage.startAutosave();
}
void MainWindow::autosave() {
    try {
        storage.autosave(document(), !undo.isClean());
    } catch (const std::exception &e) {
        statusBar()->showMessage(tr("自動保存できません: %1").arg(QString::fromUtf8(e.what())), 8000);
    }
}
void MainWindow::removeAutosave() {
    storage.removeAutosave();
}
void MainWindow::offerRecovery() {
    for (const auto &copy : storage.recoverable()) {
        const auto &recovered = copy.document;
        const auto &origin = copy.origin;
        const auto &time = copy.time;
        QMessageBox box(QMessageBox::Question, tr("作業の復旧"),
                        tr("前回のMolFormaは正常に終了しませんでした。自動保存した作業があります。\n\n"
                           "保存日時: %1\n元のファイル: %2\n分子: %3（%4原子）\n\n復旧しますか？")
                            .arg(QDateTime::fromString(time, Qt::ISODate).toString("yyyy-MM-dd HH:mm:ss"),
                                 origin.isEmpty() ? tr("（未保存の新規プロジェクト）") : origin,
                                 recovered.formula())
                            .arg(recovered.atoms.size()),
                        QMessageBox::NoButton, this);
        box.setObjectName("recoveryDialog");
        auto *restore = box.addButton(tr("復旧する"), QMessageBox::AcceptRole);
        auto *discard = box.addButton(tr("破棄する"), QMessageBox::DestructiveRole);
        box.addButton(tr("後で決める"), QMessageBox::RejectRole); // Keeps the copy for the next start.
        box.exec();
        if (box.clickedButton() == restore) {
            loadDocument(recovered);
            clearProjectPath(); // Save explicitly; the recovered copy may differ from the original file.
            undo.resetClean();
            refresh();
            copy.discard();
            statusBar()->showMessage(tr("自動保存から復旧しました。名前を付けて保存してください。"), 15000);
            return;
        }
        if (box.clickedButton() == discard)
            copy.discard();
    }
}
void MainWindow::showShortcuts() {
    QDialog dialog(this);
    dialog.setObjectName("shortcutsDialog");
    dialog.setWindowTitle(tr("キーボードショートカット"));
    dialog.resize(560, 600);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(0, 3);
    table->setObjectName("shortcutsTable");
    table->setHorizontalHeaderLabels({tr("キー"), tr("操作"), tr("有効な場所")});
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    QList<QAction *> actions;
    std::function<void(QMenu *)> collect = [&](QMenu *menu) {
        for (auto *a : menu->actions()) {
            if (a->menu())
                collect(a->menu());
            else if (!a->shortcut().isEmpty() && !actions.contains(a))
                actions << a;
        }
    };
    for (auto *a : menuBar()->actions())
        if (a->menu())
            collect(a->menu());
    const auto row = [&](const QString &key, const QString &text, const QString &where) {
        const int r = table->rowCount();
        table->insertRow(r);
        table->setItem(r, 0, new QTableWidgetItem(key));
        table->setItem(r, 1, new QTableWidgetItem(text));
        table->setItem(r, 2, new QTableWidgetItem(where));
    };
    for (auto *a : actions) {
        QStringList keys;
        for (const auto &k : a->shortcuts())
            keys << k.toString(QKeySequence::NativeText);
        // Undo/Redo name the next operation in the menu; the list names the command itself.
        const auto text = a == undoAction   ? tr("元に戻す（候補があれば取消）")
                          : a == redoAction ? tr("やり直す")
                                            : QString(a->text()).remove('&');
        row(keys.join(" / "), text,
            a->shortcutContext() == Qt::WidgetWithChildrenShortcut ? tr("「1 分子」") : tr("全体", "where"));
    }
    for (const auto &[key, text] : {std::pair{tr("左ドラッグ"), tr("回転（画面の縦・横軸まわり）")},
                                    {tr("右ドラッグ"), tr("平行移動", "view")},
                                    {tr("ホイール"), tr("カーソル位置を中心に拡大・縮小")},
                                    {tr("クリック"), tr("原子を選択（選択単位に従う）")},
                                    {tr("Shift／Ctrl＋クリック"), tr("選択へ追加・解除（順番を保持）")},
                                    {tr("Shift／Ctrlドラッグ"), tr("範囲選択（Ctrlは追加）")},
                                    {"Ctrl+Tab", tr("次のタブ（タブ内にフォーカスがあるとき）")}})
        row(key, text, tr("3D画面・タブ"));
    table->resizeColumnsToContents();
    layout->addWidget(table, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
void MainWindow::showManual() {
    initManualResources();
    QDialog dialog(this);
    dialog.setObjectName("manualDialog");
    dialog.setWindowTitle(tr("MolForma 操作ガイド・マニュアル"));
    dialog.resize(900, 720);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tips = new QLabel(
        tr("左のパレットで部品・原子を選び、3D画面をクリックするかドロップして配置します。"
           "左端の列でツールを切り替え、3D画面の上にツールの設定、下にクリックしたときの動作を表示します。"
           "原子をクリックで選択、Shift／Ctrl＋クリックで順に追加し、2〜4原子で距離・角度・二面角を"
           "測定できます。配位子・削除・整形などは「変更を確定」（Ctrl+Enter）で適用し、Escで"
           "候補を取消、Ctrl+Zで確定済みの変更を戻します。計算は「計算 → 計算を実行」（F5）で"
           "始めます。キー操作の一覧は「ヘルプ → キーボードショートカット一覧」にあります。"));
    tips->setWordWrap(true);
    layout->addWidget(tips);
    auto *bar = new QHBoxLayout;
    layout->addLayout(bar);
    auto *pages = new QComboBox;
    pages->setObjectName("manualPage");
    // The English manual beside the Japanese one.
    const QString docs = english() ? ":/docs/en/" : ":/docs/";
    pages->addItem(tr("機能と操作（はじめに・各タブの説明）"), docs + "features.md");
    pages->addItem(tr("題材から選ぶ実習（分子を組んで計算するまで）"), docs + "recipes.md");
    pages->addItem(tr("MCPサーバー（AIクライアントからの操作）"), docs + "mcp.md");
    auto *back = new QPushButton(tr("戻る"));
    back->setObjectName("manualBack");
    back->setEnabled(false);
    bar->addWidget(back);
    bar->addWidget(pages);
    auto *find = new QLineEdit;
    find->setObjectName("manualSearch");
    find->setPlaceholderText(tr("検索（Enterで次へ）"));
    find->setClearButtonEnabled(true);
    bar->addWidget(find, 1);
    auto *browser = new QTextBrowser;
    browser->setObjectName("manualText");
    layout->addWidget(browser, 1);
    // Links are followed here: the practice files beside the recipes are used rather than shown.
    browser->setOpenLinks(false);
    connect(browser, &QTextBrowser::anchorClicked, &dialog, [this, browser, &dialog](const QUrl &url) {
        if (url.scheme() != "qrc")
            QDesktopServices::openUrl(url);
        else if (!url.path().startsWith("/docs/recipes/samples/"))
            browser->setSource(url);
        else
            safe([&] {
                if (useSample(":" + url.path()))
                    dialog.accept();
            });
    });
    // Pages are shown by their resource URL, so links between them and their images resolve.
    const auto show = [browser, pages] {
        const auto path = pages->currentData().toString();
        if (QFile::exists(path))
            browser->setSource(QUrl("qrc" + path), QTextDocument::MarkdownResource);
        else
            browser->setPlainText(tr("マニュアルを読み込めません。"));
    };
    connect(back, &QPushButton::clicked, browser, &QTextBrowser::backward);
    connect(browser, &QTextBrowser::backwardAvailable, back, &QPushButton::setEnabled);
    // Screenshots in the recipes are wider than the page: they are shown smoothly scaled to its width
    // (the document itself would scale them pixel by pixel).
    const auto fitImages = [browser] {
        auto *document = browser->document();
        const int width = browser->viewport()->width() - 2 * int(document->documentMargin()) - 8;
        const double ratio = browser->devicePixelRatioF();
        QTextCursor cursor(document);
        for (auto block = document->begin(); block.isValid(); block = block.next()) {
            // Figures inside the steps of a list are indented (Markdown continuation paragraphs).
            const auto format = block.blockFormat();
            double indent = format.leftMargin() + format.indent() * document->indentWidth();
            if (auto *list = block.textList())
                indent += list->format().indent() * document->indentWidth();
            const int fit = width - std::max(0, int(indent));
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (!fragment.charFormat().isImageFormat())
                    continue;
                auto image = fragment.charFormat().toImageFormat();
                const auto url = document->baseUrl().resolved(QUrl(image.name()));
                QImage original(url.scheme() == "qrc" ? ":" + url.path() : url.toLocalFile());
                if (original.isNull() || original.width() <= fit)
                    continue;
                auto fitted = original.scaledToWidth(int(fit * ratio), Qt::SmoothTransformation);
                fitted.setDevicePixelRatio(ratio);
                const QUrl name =
                    document->baseUrl().resolved(QString("fitted/%1/%2").arg(fit).arg(image.name()));
                document->addResource(QTextDocument::ImageResource, name, fitted);
                image.setName(name.toString());
                image.setWidth(fit);
                image.setHeight(original.height() * double(fit) / original.width());
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                cursor.setCharFormat(image);
            }
        }
    };
    connect(browser, &QTextBrowser::sourceChanged, &dialog, fitImages);
    show();
    connect(pages, &QComboBox::currentIndexChanged, &dialog, show);
    connect(find, &QLineEdit::returnPressed, &dialog, [browser, find] {
        if (!browser->find(find->text())) {
            browser->moveCursor(QTextCursor::Start); // Wrap around.
            browser->find(find->text());
        }
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
void MainWindow::closeEvent(QCloseEvent *e) {
    if (execution->running() &&
        QMessageBox::question(this, tr("NWChemを実行中"),
                              tr("実行中の計算を停止してウィンドウを閉じますか？ログは保存先に残ります。"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    if (!confirmDiscard()) {
        e->ignore();
        return;
    }
    cancelWorker();
    execution->shutdown();
    settings.setValue("geometry", saveGeometry());
    moleculePage->savePreferences();
    removeAutosave();
    storage.close();
    e->accept();
}
void MainWindow::preferences() {
    QDialog dialog(this);
    dialog.setObjectName("preferencesDialog");
    dialog.setProperty("mcpProtected", true); // Same protection as the executable fields in "3 実行設定".
    dialog.setProperty("canvas", true);
    dialog.setWindowTitle(tr("環境設定"));
    dialog.resize(640, 700);
    // The cards and controls of "3 実行設定", for the settings new documents start with.
    dialog.setStyleSheet(
        chromeRules(settingsRules() + "QDialog { background: @window; }", QMainWindow::palette()));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(12);
    auto *intro =
        new QLabel(tr("新規作成、およびプロジェクト以外の構造ファイル（XYZ・SDF・PDB等）を開いたときの"
                      "実行設定です。開いているプロジェクトの設定は「3 実行設定」で変更します。"));
    intro->setWordWrap(true);
    intro->setProperty("muted", true);
    layout->addWidget(intro);
    const auto saved = executionPreferences();
    const auto card = [&](const QString &title) {
        auto *c = new SettingsCard(title);
        auto *form = new FormLayout;
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(8);
        c->body()->addLayout(form);
        layout->addWidget(c);
        return form;
    };
    auto *files = card(tr("実行ファイル・保存先"));
    auto path = [&](const QString &label, const QString &key, bool directory) {
        auto *line = new QLineEdit(saved[key].toString());
        line->setObjectName("preferences_" + key);
        auto *browse = new QPushButton(tr("参照…"));
        connect(browse, &QPushButton::clicked, &dialog, [&dialog, line, directory] {
            const auto chosen =
                directory ? QFileDialog::getExistingDirectory(&dialog, tr("ジョブ保存先"), line->text())
                          : QFileDialog::getOpenFileName(&dialog, tr("NWChem実行ファイル"), line->text());
            if (!chosen.isEmpty())
                line->setText(chosen);
        });
        auto *row = new QHBoxLayout;
        row->addWidget(line, 1);
        row->addWidget(browse);
        files->addRow(label, row);
        return line;
    };
    auto *executable = path(tr("NWChem実行ファイル"), "executable", false);
    auto *mode = new QComboBox;
    mode->setObjectName("preferences_mode");
    mode->addItem(tr("MPIで起動"), "mpi");
    mode->addItem(tr("直接実行（逐次版など）"), "direct");
    mode->setCurrentIndex(std::max(0, mode->findData(saved["mode"].toString())));
    files->addRow(tr("起動方式"), new ChoiceButtons(mode, ChoiceButtons::Segments));
    auto *launcher = new QLineEdit(saved["launcher"].toString());
    launcher->setObjectName("preferences_launcher");
    files->addRow(tr("MPIランチャー"), launcher);
    auto *root = path(tr("ジョブ保存先"), "job_root", true);
    auto *resources = card(tr("計算資源"));
    auto spin = [&](const QString &label, const QString &key, int minimum, int maximum,
                    const QString &unit = {}) {
        auto *box = new QSpinBox;
        box->setObjectName("preferences_" + key);
        box->setRange(minimum, maximum);
        box->setSuffix(unit);
        box->setValue(saved[key].toInt());
        resources->addRow(label, box);
        return box;
    };
    auto *ranks = spin(tr("MPI起動プロセス数"), "mpi_ranks", 1, 4096);
    auto *omp = spin(tr("OpenMPスレッド / プロセス"), "omp_threads", 1, 1024);
    auto *blas = spin(tr("BLASスレッド / プロセス"), "blas_threads", 1, 1024);
    auto *stack = new QLineEdit(saved["omp_stacksize"].toString());
    stack->setObjectName("preferences_omp_stacksize");
    stack->setPlaceholderText(tr("空欄: 継承（例: 64M）"));
    resources->addRow(tr("OpenMPスタック"), stack);
    auto *timeout = spin(tr("制限時間（0: なし）"), "timeout_seconds", 0, 604800, tr(" 秒"));
    // The environment as the table of "3 実行設定": rows that are off are kept.
    auto *environmentCard = new SettingsCard(tr("追加の環境変数"));
    auto *environment = new QTableWidget(0, 3);
    environment->setObjectName("preferences_environment");
    environment->setHorizontalHeaderLabels({tr("有効"), tr("名前"), tr("値")});
    environment->verticalHeader()->hide();
    environment->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    environment->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    environment->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    environment->setMinimumHeight(110);
    environment->setMaximumHeight(170);
    const auto showEnvironment = [environment](const QJsonArray &rows) {
        environment->setRowCount(rows.size());
        for (int row = 0; row < rows.size(); ++row) {
            const auto entry = rows[row].toObject();
            auto *enabled = new QTableWidgetItem;
            enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            enabled->setCheckState(entry["enabled"].toBool() ? Qt::Checked : Qt::Unchecked);
            environment->setItem(row, 0, enabled);
            environment->setItem(row, 1, new QTableWidgetItem(entry["name"].toString()));
            environment->setItem(row, 2, new QTableWidgetItem(entry["value"].toString()));
        }
    };
    showEnvironment(saved["environment"].toArray());
    auto *addRow = new QPushButton(tr("追加"));
    addRow->setIcon(lineIcon("plus"));
    auto *removeRow = new QPushButton(tr("選択行を削除"));
    environmentCard->header()->addWidget(removeRow);
    environmentCard->header()->addWidget(addRow);
    connect(addRow, &QPushButton::clicked, &dialog, [environment] {
        const int row = environment->rowCount();
        environment->insertRow(row);
        auto *enabled = new QTableWidgetItem;
        enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        enabled->setCheckState(Qt::Checked);
        environment->setItem(row, 0, enabled);
        environment->setItem(row, 1, new QTableWidgetItem);
        environment->setItem(row, 2, new QTableWidgetItem);
        environment->editItem(environment->item(row, 1));
    });
    connect(removeRow, &QPushButton::clicked, &dialog, [environment] {
        if (environment->currentRow() >= 0)
            environment->removeRow(environment->currentRow());
    });
    environmentCard->body()->addWidget(environment);
    auto *environmentNote =
        new QLabel(tr("有効な行だけを起動環境へ上書きします（例: NWCHEM_BASIS_LIBRARY）。"));
    environmentNote->setProperty("caption", true);
    environmentNote->setWordWrap(true);
    environmentCard->body()->addWidget(environmentNote);
    layout->addWidget(environmentCard);
    auto *startup = card(tr("起動・表示"));
    auto *water = new QCheckBox(tr("起動時に水分子を読み込む"));
    water->setObjectName("preferences_startupWater");
    water->setChecked(settings.value("startupWater", false).toBool());
    startup->addRow(tr("新しいウィンドウ"), water);
    auto *renderer = new QComboBox;
    renderer->addItem(tr("自動（VTKを優先）"), "auto");
    renderer->addItem(tr("QPainter（CPU）"), "painter");
    renderer->setCurrentIndex(settings.value("view/renderer").toString() == "painter" ? 1 : 0);
    startup->addRow(tr("3D描画（再起動後に反映）"), renderer);
    // Named in both languages, so that either reader finds it.
    auto *language = new QComboBox;
    language->setObjectName("preferences_language");
    language->addItem(tr("システムに合わせる"), "");
    language->addItem("日本語", "ja"); // i18n-exempt
    language->addItem("English", "en");
    language->setCurrentIndex(std::max(0, language->findData(settings.value("ui/language").toString())));
    startup->addRow(tr("表示言語 / Language（再起動後に反映）"), language);
    layout->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    auto *current = buttons->addButton(tr("現在の実行設定を読み込む"), QDialogButtonBox::ResetRole);
    layout->addWidget(buttons);
    connect(current, &QPushButton::clicked, &dialog, [&] {
        const auto s = executionSettings(document());
        executable->setText(s["executable"].toString());
        mode->setCurrentIndex(std::max(0, mode->findData(s["mode"].toString())));
        launcher->setText(s["launcher"].toString());
        ranks->setValue(s["mpi_ranks"].toInt());
        omp->setValue(s["omp_threads"].toInt());
        blas->setValue(s["blas_threads"].toInt());
        stack->setText(s["omp_stacksize"].toString());
        root->setText(s["job_root"].toString());
        timeout->setValue(s["timeout_seconds"].toInt());
        showEnvironment(s["environment"].toArray());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        safe([&] {
            QJsonArray rows;
            for (int row = 0; row < environment->rowCount(); ++row) {
                const auto name = environment->item(row, 1)->text().trimmed();
                const auto value = environment->item(row, 2)->text();
                if (name.isEmpty() && value.isEmpty())
                    continue;
                rows.append(QJsonObject{{"enabled", environment->item(row, 0)->checkState() == Qt::Checked},
                                        {"name", name},
                                        {"value", value}});
            }
            const QJsonObject chosen{{"executable", executable->text().trimmed()},
                                     {"mode", mode->currentData().toString()},
                                     {"launcher", launcher->text().trimmed()},
                                     {"mpi_ranks", ranks->value()},
                                     {"omp_threads", omp->value()},
                                     {"blas_threads", blas->value()},
                                     {"omp_stacksize", stack->text().trimmed()},
                                     {"job_root", root->text().trimmed()},
                                     {"timeout_seconds", timeout->value()},
                                     {"environment", rows}};
            // Check the values with the same rules as "3 実行設定", ignoring the molecule itself.
            Document probe;
            for (auto it = chosen.begin(); it != chosen.end(); ++it)
                setExecutionSetting(probe, it.key(), it.value());
            const auto molecule = validateNwchem(probe).errors;
            QStringList errors;
            for (const auto &error : validateExecution(probe, QDir::homePath(), false).errors)
                if (!molecule.contains(error))
                    errors << error;
            if (!errors.isEmpty())
                fail(errors.join('\n'));
            saveExecutionPreferences(chosen);
            settings.setValue("startupWater", water->isChecked());
            settings.setValue("view/renderer", renderer->currentData().toString());
            settings.setValue("ui/language", language->currentData().toString());
            dialog.accept();
        });
    });
    dialog.exec();
}
} // namespace mf
