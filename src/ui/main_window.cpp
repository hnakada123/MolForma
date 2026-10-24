#include "ui/main_window.h"
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/coordination.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "editing/stereochemistry.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_execution.h"
#include "nwchem/nwchem_import.h"
#include "ui/advanced_panel.h"
#include "ui/calculation_panel.h"
#include "ui/coordinates_panel.h"
#include "ui/execution_panel.h"
#include "ui/history_panel.h"
#include "ui/input_preview_panel.h"
#include "ui/periodic_table.h"
#include "ui/results_panel.h"
#include "ui/template_delegate.h"
#include <QDrag>
#include <QJsonDocument>
#include <QMimeData>
#include <QtWidgets>
// Embedded manual and icon (docs/manual.qrc in this static library); outside namespace mf.
static void initManualResources() {
    Q_INIT_RESOURCE(manual);
}
namespace mf {
class DocumentCommand : public QUndoCommand {
    MainWindow *window;
    Document before, after;
    bool remote; // Made or confirmed over MCP: undo/redo never make its execution settings trusted.

  public:
    DocumentCommand(MainWindow *w, const QString &name, const Document &next, bool remote)
        : QUndoCommand(name), window(w), before(w->doc), after(next), remote(remote) {}
    void undo() override {
        window->setState(before, !remote);
    }
    void redo() override {
        window->setState(after, !remote);
    }
};
class PaletteList : public QListWidget {
  public:
    using QListWidget::QListWidget;

  protected:
    void startDrag(Qt::DropActions) override {
        if (!currentItem())
            return;
        auto *mime = new QMimeData;
        mime->setData("application/x-molforma-template",
                      currentItem()->data(Qt::UserRole).toString().toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(viewport()->grab(visualItemRect(currentItem())));
        drag->exec(Qt::CopyAction);
    }
};
static QString value(QComboBox *box) {
    return box->currentData().toString();
}
static QByteArray identity(const Document &d) {
    return QJsonDocument(d.project()).toJson(QJsonDocument::Compact);
}
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), settings("MolForma", "MolForma") {
    setObjectName("mainWindow");
    setAcceptDrops(true);
    resize(1380, 900);
    setMinimumSize(900, 620);
    customParts = settings.value("parts").toJsonArray();
    buildUi();
    buildMenus();
    connect(&undo, &QUndoStack::cleanChanged, this, [this] { refresh(); });
    if (settings.value("startupWater", true).toBool())
        placeTemplate(doc, findTemplate("skeleton:water"), Vec::Zero());
    applyExecutionPreferences(doc);
    mcp.trustedExecution = protectedExecution(doc);
    refresh();
    QTimer::singleShot(0, view, &MoleculeView::fit);
    auto saved = settings.value("geometry").toByteArray();
    if (!saved.isEmpty())
        restoreGeometry(saved);
}
MainWindow::~MainWindow() {
    // A normal end (window closed, or MCP's standard input closed) leaves nothing to recover.
    removeAutosave();
    autosaveLock.reset();
    results->cancelLoad();
    calculation->stopEditing();
    advanced->stopEditing();
    execution->stopEditing();
    execution->shutdown();
    cancelWorker();
}
void MainWindow::safe(const std::function<void()> &operation) {
    try {
        operation();
    } catch (const std::exception &e) {
        QString message = QString::fromUtf8(e.what());
        statusBar()->showMessage(message, 15000);
        QMessageBox::warning(this, "操作を確認", message);
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
void MainWindow::setState(const Document &d, bool trusted) {
    if (trusted)
        trackExecution(doc, d);
    doc = d;
    candidate.reset();
    selected.removeIf([&](auto id) { return !doc.contains(id); });
    refresh();
}
void MainWindow::loadDocument(const Document &d) {
    cancelWorker();
    candidate.reset();
    selected.clear();
    trackExecution(doc, d);
    doc = d;
    undo.clear();
    undo.setClean();
    refresh();
    view->fit();
}
void MainWindow::commit(const QString &name, const Document &next, bool remote) {
    auto errors = next.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    if (identity(doc) == identity(next))
        return;
    undo.push(new DocumentCommand(this, name, next, remote || mcp.depth > 0));
    if (!keepSelection->isChecked())
        setSelection({});
    statusBar()->showMessage(name, 5000);
}
void MainWindow::edit(const QString &name, const std::function<void(Document &)> &operation, bool preview) {
    if (worker)
        fail("整形処理の完了または中止後に編集してください。");
    if (candidate)
        fail("先に変更候補を確定するか、取消してください。");
    Document next = doc;
    operation(next);
    auto errors = next.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    if (identity(next) == identity(doc))
        return;
    if (preview) {
        candidate = next;
        candidateName = name;
        candidateRemote = mcp.depth > 0;
        refresh();
        statusBar()->showMessage("候補を確認して確定してください。", 8000);
    } else
        commit(name, next);
}
void MainWindow::confirmCandidate() {
    if (!candidate)
        return;
    Document next = *candidate;
    QString name = candidateName;
    candidate.reset();
    // A person confirming an MCP candidate does not make the settings it carries trusted.
    commit(name, next, candidateRemote);
    refresh();
}
void MainWindow::cancelCandidate() {
    candidate.reset();
    refresh();
    statusBar()->showMessage("変更候補を取り消しました。", 4000);
}
void MainWindow::remoteHistory(const QString &action) {
    if (worker)
        fail("整形処理の完了または中止後に操作してください。");
    if (action == "confirm" || action == "cancel") {
        if (!candidate)
            fail("変更候補がありません。");
        if (action == "confirm")
            confirmCandidate();
        else
            cancelCandidate();
        return;
    }
    if (candidate)
        fail("先に変更候補を確定するか、取消してください。");
    if (action == "undo" && undo.canUndo())
        undo.undo();
    else if (action == "redo" && undo.canRedo())
        undo.redo();
    else
        fail("対応する編集履歴がありません。");
}
void MainWindow::buildUi() {
    auto *central = new QWidget;
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(6, 6, 6, 6);
    setCentralWidget(central);
    workspaceTabs = new QTabWidget;
    workspaceTabs->setObjectName("workspaceTabs");
    outer->addWidget(workspaceTabs, 1);
    auto *root = new QWidget;
    workspaceTabs->addTab(root, "1 分子");
    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(10, 6, 10, 8);
    auto *tools = new QHBoxLayout;
    layout->addLayout(tools);
    tool = new QComboBox;
    tool->setObjectName("toolSelector");
    QStringList codes{"select", "atom", "place",   "substituent", "center", "ligand",
                      "bond",   "move", "replace", "delete",      "charge"},
        labels{"選択", "原子追加", "部品配置", "置換基追加", "配位中心", "配位子追加",
               "結合", "移動",     "置換",     "削除",       "点電荷"};
    for (int i = 0; i < codes.size(); ++i)
        tool->addItem(labels[i], codes[i]);
    tools->addWidget(tool);
    auto add = [&](QString text, auto fn) {
        auto *b = new QPushButton(text);
        tools->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, fn] { safe(fn); });
        return b;
    };
    add("元に戻す", [this] {
        if (candidate)
            cancelCandidate();
        else
            undo.undo();
    });
    add("やり直す", [this] {
        if (!candidate)
            undo.redo();
    });
    tools->addSpacing(8);
    unitBox = new QComboBox;
    for (auto pair : QList<QPair<QString, QString>>{{"原子", "atom"},
                                                    {"結合", "bond"},
                                                    {"部品", "part"},
                                                    {"残基", "residue"},
                                                    {"分子", "component"},
                                                    {"全体", "all"}})
        unitBox->addItem(pair.first, pair.second);
    tools->addWidget(new QLabel("選択単位"));
    tools->addWidget(unitBox);
    add("選択解除", [this] { setSelection({}); });
    add("一括削除", [this] {
        auto ids = requireSelection();
        edit("選択を削除", [=](Document &d) { d.remove(ids); });
    });
    tools->addStretch();
    add("視点リセット", [this] { view->viewFrom(2); });
    auto *options = new QHBoxLayout;
    layout->addLayout(options);
    showH = new QCheckBox("水素表示");
    showH->setChecked(true);
    options->addWidget(showH);
    auto *indices = new QCheckBox("原子番号");
    indices->setChecked(true);
    options->addWidget(indices);
    autoH = new QCheckBox("水素自動補完");
    autoH->setChecked(true);
    options->addWidget(autoH);
    keepSelection = new QCheckBox("編集後に選択維持");
    keepSelection->setChecked(settings.value("keepSelection", false).toBool());
    options->addWidget(keepSelection);
    keepTool = new QCheckBox("配置後もツールを保持");
    keepTool->setChecked(settings.value("keepTool", false).toBool());
    options->addWidget(keepTool);
    auto *h = new QPushButton("水素を補完");
    options->addWidget(h);
    connect(h, &QPushButton::clicked, this, [this] {
        safe([this] { edit("水素を補完", [this](Document &d) { completeHydrogens(d, selected); }); });
    });
    auto *refineButton = new QPushButton("全体整形");
    refineButton->setObjectName("refineButton");
    options->addWidget(refineButton);
    connect(refineButton, &QPushButton::clicked, this, [this] { safe([this] { runWorker("refine"); }); });
    cancelWorkerButton = new QPushButton("整形を中止");
    cancelWorkerButton->hide();
    options->addWidget(cancelWorkerButton);
    connect(cancelWorkerButton, &QPushButton::clicked, this, &MainWindow::cancelWorker);
    options->addStretch();
    auto *splitter = new QSplitter;
    layout->addWidget(splitter, 1);
    auto *left = new QWidget;
    left->setMinimumWidth(180);
    left->setMaximumWidth(320);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 6, 0);
    ll->addWidget(new QLabel("部品パレット"));
    search = new QLineEdit;
    search->setObjectName("paletteSearch");
    search->setPlaceholderText("名前・元素・SMILESを検索");
    search->setClearButtonEnabled(true);
    ll->addWidget(search);
    category = new QComboBox;
    category->setObjectName("paletteCategory");
    for (auto pair : QList<QPair<QString, QString>>{{"すべて", "all"},
                                                    {"原子（周期表 H〜Og）", "atom"},
                                                    {"骨格", "skeleton"},
                                                    {"置換基", "substituent"},
                                                    {"配位中心", "center"},
                                                    {"配位子", "ligand"},
                                                    {"アミノ酸", "amino"},
                                                    {"マイ部品", "my"}})
        category->addItem(pair.first, pair.second);
    ll->addWidget(category);
    periodicTableButton = new QPushButton("周期表から原子を選択…");
    periodicTableButton->setObjectName("openPeriodicTable");
    ll->addWidget(periodicTableButton);
    connect(periodicTableButton, &QPushButton::clicked, this, [this] {
        PeriodicTableDialog dialog(paletteElement, search->text(), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        paletteElement = dialog.selectedElement();
        search->clear();
        refreshPalette();
        palette->setCurrentRow(0);
        paletteChosen();
    });
    growth = new QCheckBox("アミノ酸の連続追加");
    ll->addWidget(growth);
    palette = new PaletteList;
    palette->setObjectName("templatePalette");
    palette->setItemDelegate(new TemplateDelegate(palette));
    palette->setDragEnabled(true);
    palette->setDragDropMode(QAbstractItemView::DragOnly);
    palette->setDefaultDropAction(Qt::CopyAction);
    palette->setSpacing(0);
    ll->addWidget(palette, 1);
    auto *hint = new QLabel("空白に配置／原子へ付加\n水素へ置換基をドロップ\n配位サイト・末端の＋へ接続");
    hint->setWordWrap(true);
    hint->setStyleSheet("color:#66798b;");
    ll->addWidget(hint);
    splitter->addWidget(left);
    auto *middle = new QWidget;
    auto *ml = new QVBoxLayout(middle);
    ml->setContentsMargins(0, 0, 0, 0);
    sequenceLabel = new QLabel("1 分子");
    sequenceLabel->setWordWrap(true);
    sequenceLabel->setTextFormat(Qt::RichText);
    sequenceLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(sequenceLabel, &QLabel::linkActivated, this, [this](const QString &rid) {
        safe([this, rid] {
            Ids ids;
            for (auto v : residue(displayDocument(), rid)["atoms"].toObject())
                ids << v.toString();
            setSelection(ids);
        });
    });
    ml->addWidget(sequenceLabel);
    view = new MoleculeView;
    ml->addWidget(view, 1);
    auto *angles = new QHBoxLayout;
    angles->setSpacing(16);
    ml->addLayout(angles);
    QSlider *sliders[3];
    for (int i = 0; i < 3; ++i) {
        auto *control = new QVBoxLayout;
        control->setSpacing(2);
        auto *label = new QLabel(QStringList{"左右回転", "上下回転", "画面内回転"}[i]);
        control->addWidget(label);
        sliders[i] = new QSlider(Qt::Horizontal);
        sliders[i]->setRange(-180, 180);
        label->setBuddy(sliders[i]);
        control->addWidget(sliders[i]);
        angles->addLayout(control, 1);
    }
    for (int i = 0; i < 3; ++i)
        connect(sliders[i], &QSlider::valueChanged, this,
                [this, a = sliders[0], b = sliders[1], c = sliders[2]] {
                    view->setAngles(a->value(), b->value(), c->value());
                });
    auto *axes = new QHBoxLayout;
    axes->setSpacing(4);
    angles->addLayout(axes);
    for (int i = 0; i < 3; ++i) {
        auto *b = new QPushButton(QString("+") + QString("XYZ")[i]);
        b->setMaximumWidth(40);
        axes->addWidget(b, 0, Qt::AlignBottom);
        connect(b, &QPushButton::clicked, this, [this, i] { view->viewFrom(i); });
    }
    measurement = new QLabel("原子を順にクリックすると距離・角度・二面角を測定します。");
    measurement->setWordWrap(true);
    measurement->setObjectName("measurementLabel");
    ml->addWidget(measurement);
    auto *dimensions = new QHBoxLayout;
    dimension = new QDoubleSpinBox;
    dimension->setDecimals(5);
    dimension->setRange(-10000, 10000);
    dimension->setObjectName("dimensionValue");
    dimensions->addWidget(new QLabel("距離 Å / 角度 °"));
    dimensions->addWidget(dimension);
    auto *applyDimension = new QPushButton("寸法を適用");
    applyDimension->setObjectName("applyDimension");
    dimensions->addWidget(applyDimension);
    dimensions->addStretch();
    ml->addLayout(dimensions);
    connect(applyDimension, &QPushButton::clicked, this, [this] {
        safe([this] {
            auto ids = requireSelection();
            double v = dimension->value();
            edit("寸法を適用", [=](Document &d) { setDimension(d, ids, v); });
        });
    });
    auto *mouseHint =
        new QLabel("左ドラッグ: 回転 · 右: 平行移動 · ホイール: 拡縮 · Shiftドラッグ: 範囲選択");
    mouseHint->setWordWrap(true);
    mouseHint->setStyleSheet("color:#66798b;font-size:11px;");
    ml->addWidget(mouseHint);
    splitter->addWidget(middle);
    properties = new QTabWidget;
    properties->setObjectName("propertyTabs");
    properties->setMinimumWidth(300);
    properties->setMaximumWidth(440);
    properties->setUsesScrollButtons(true);
    splitter->addWidget(properties);
    buildEditPanel();
    buildCoordinationPanel();
    buildResiduePanel();
    buildChargePanel();
    splitter->setSizes({300, 690, 340});
    coordinates = new CoordinatesPanel(this);
    workspaceTabs->addTab(coordinates, "2 座標");
    calculation = new CalculationPanel(this);
    workspaceTabs->addTab(calculation, "3 計算条件");
    advanced = new AdvancedPanel(this);
    workspaceTabs->addTab(advanced, "4 詳細設定");
    execution = new ExecutionPanel(this);
    workspaceTabs->addTab(execution, "5 実行設定");
    inputPreview = new InputPreviewPanel(this);
    workspaceTabs->addTab(inputPreview, "6 入力プレビュー");
    results = new ResultsPanel(this);
    workspaceTabs->addTab(results, "結果ビューア");
    calculationHistory = new HistoryPanel(this);
    workspaceTabs->addTab(calculationHistory, "履歴");
    connect(execution, &ExecutionPanel::bundleChanged, calculationHistory, &HistoryPanel::notifyJob);
    connect(workspaceTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (workspaceTabs->widget(index) == inputPreview) {
            execution->commitPendingEdits();
            inputPreview->refresh();
        }
    });
    candidateBar = new QWidget;
    candidateBar->setObjectName("candidateBar");
    candidateBar->setStyleSheet(
        "#candidateBar { background:#fff0cd; border:1px solid #ddbd73; border-radius:4px; }");
    auto *cl = new QHBoxLayout(candidateBar);
    candidateLabel = new QLabel;
    candidateLabel->setWordWrap(true);
    cl->addWidget(candidateLabel, 1);
    auto *confirm = new QPushButton("変更を確定");
    confirm->setObjectName("confirmCandidate");
    cl->addWidget(confirm);
    auto *cancel = new QPushButton("取消");
    cancel->setObjectName("cancelCandidate");
    cl->addWidget(cancel);
    connect(confirm, &QPushButton::clicked, this, [this] { safe([this] { confirmCandidate(); }); });
    connect(cancel, &QPushButton::clicked, this, &MainWindow::cancelCandidate);
    outer->addWidget(candidateBar);
    summary = new QLabel;
    summary->setObjectName("documentSummary");
    outer->addWidget(summary);
    connect(tool, &QComboBox::currentIndexChanged, this, [this] { view->setTool(value(tool)); });
    connect(unitBox, &QComboBox::currentIndexChanged, this,
            [this] { safe([this] { setSelection(expandedSelection(selected)); }); });
    connect(showH, &QCheckBox::toggled, view, &MoleculeView::setShowHydrogens);
    connect(indices, &QCheckBox::toggled, view, &MoleculeView::setShowIndices);
    connect(search, &QLineEdit::textChanged, this, &MainWindow::refreshPalette);
    connect(category, &QComboBox::currentIndexChanged, this, &MainWindow::refreshPalette);
    connect(palette, &QListWidget::itemClicked, this, [this] { safe([this] { paletteChosen(); }); });
    connect(view, &MoleculeView::atomClicked, this,
            [this](auto id, auto mods) { safe([this, id, mods] { atomClicked(id, mods); }); });
    connect(view, &MoleculeView::blankClicked, this, [this](Vec p) {
        safe([this, p] {
            if (value(tool) == "select")
                setSelection({});
            else
                place({}, p);
        });
    });
    connect(view, &MoleculeView::bondClicked, this, [this](auto a, auto b) {
        safe([this, a, b] {
            if (value(tool) == "atom") {
                edit("結合間に原子を挿入", [=, this](Document &d) {
                    Vec p = (d.atom(a).position + d.atom(b).position) * .5;
                    d.setBond(a, b, "none");
                    Atom x;
                    x.element = value(elementBox);
                    x.position = p;
                    d.atoms << x;
                    d.setBond(a, x.id, "single");
                    d.setBond(x.id, b, "single");
                    if (autoH->isChecked())
                        completeHydrogens(d, {a, b, x.id});
                });
            } else
                setSelection({a, b});
        });
    });
    connect(view, &MoleculeView::siteClicked, this, [this](auto c, int site) {
        safe([this, c, site] {
            setSelection({c});
            if (value(tool) == "ligand") {
                auto t = currentTemplate();
                auto indices = sites();
                if (t["connections"].toArray().size() == 2 && !indices.contains(site) &&
                    indices.size() == 1) {
                    indices << site;
                } else
                    indices = {site};
                QStringList text;
                for (int i : indices)
                    text << QString::number(i + 1);
                siteInput->setText(text.join(','));
                if (indices.size() == t["connections"].toArray().size())
                    edit("配位子を配置", [=](Document &d) { placeLigand(d, c, indices, t); }, true);
            } else
                siteInput->setText(QString::number(site + 1));
        });
    });
    connect(view, &MoleculeView::terminusClicked, this,
            [this](auto rid, bool n) { safe([this, rid, n] { addAmino(rid, n); }); });
    connect(view, &MoleculeView::atomDragged, this, [this](auto id, Vec delta) {
        safe([this, id, delta] {
            auto ids = selected.contains(id) ? selected : expandedSelection({id});
            edit("原子を移動", [=](Document &d) { transform(d, ids, Mat::Identity(), delta); });
        });
    });
    connect(view, &MoleculeView::rectangleSelected, this, [this](Ids ids, bool append) {
        safe([this, ids, append] {
            auto next = append ? selected : Ids();
            for (auto id : expandedSelection(ids))
                if (!next.contains(id))
                    next << id;
            setSelection(next);
        });
    });
    connect(view, &MoleculeView::contextRequested, this, &MainWindow::contextMenu);
    connect(view, &MoleculeView::shortcut, this, [this](int key) {
        safe([this, key] {
            if (key == Qt::Key_Escape) {
                if (candidate)
                    cancelCandidate();
                else
                    setSelection({});
                growth->setChecked(false);
                growthResidue.clear();
            } else if (key == Qt::Key_H)
                showH->toggle();
            else if (key == Qt::Key_Delete) {
                auto ids = requireSelection();
                edit("選択を削除", [=](Document &d) { d.remove(ids); }, true);
            } else if (key >= Qt::Key_1 && key <= Qt::Key_3) {
                auto ids = requireSelection(2);
                QString kind = QStringList{"single", "double", "triple"}[key - Qt::Key_1];
                edit("結合次数を変更", [=, this](Document &d) {
                    d.setBond(ids[0], ids[1], kind);
                    if (autoH->isChecked())
                        completeHydrogens(d, ids);
                });
            }
        });
    });
    connect(view, &MoleculeView::templateDropped, this,
            [this](QString key, QString atom, Vec p, QString c, int site, QString rid, bool n) {
                safe([=, this] {
                    for (int i = 0; i < palette->count(); ++i)
                        if (palette->item(i)->data(Qt::UserRole) == key) {
                            palette->setCurrentRow(i);
                            break;
                        }
                    paletteChosen();
                    if (!rid.isEmpty())
                        addAmino(rid, n);
                    else if (!c.isEmpty()) {
                        auto t = currentTemplate();
                        setSelection({c});
                        siteInput->setText(QString::number(site + 1));
                        edit("配位子を配置", [=](Document &d) { placeLigand(d, c, {site}, t); }, true);
                    } else
                        place(atom, p);
                });
            });
    refreshPalette();
}
void MainWindow::buildMenus() {
    auto *file = menuBar()->addMenu("ファイル");
    auto action = [&](QMenu *m, const QString &text, const QKeySequence &key, auto fn) {
        auto *a = m->addAction(text);
        if (!key.isEmpty())
            a->setShortcut(key);
        connect(a, &QAction::triggered, this, [this, fn] { safe(fn); });
        return a;
    };
    const QList<QAction *> toolbarActions{action(file, "新規", QKeySequence::New, [this] { newDocument(); }),
                                          action(file, "開く…", QKeySequence::Open, [this] { open(); }),
                                          action(file, "SMILESから作成…", {}, [this] { importSmiles(); })};
    recentMenu = file->addMenu("最近使ったファイル");
    recentMenu->setObjectName("recentMenu");
    refreshRecent();
    action(file, "SMILESから断片を追加…", {}, [this] { addFragmentFromSmiles(); });
    action(file, "ファイルから断片を追加…", {}, [this] { addFragmentFromFile(); });
    file->addSeparator();
    auto *saveAction = action(file, "プロジェクトを保存", QKeySequence::Save, [this] { save(); });
    action(file, "名前を付けて保存…", QKeySequence::SaveAs, [this] { save(true); });
    action(file, "構造を書き出す…", {}, [this] { exportFile(); });
    action(file, "XYZをコピー", {}, [this] {
        if (resolveCandidate())
            QApplication::clipboard()->setText(doc.xyz());
    });
    action(file, "XYZを貼付…", {}, [this] {
        bool ok;
        QString text = QInputDialog::getMultiLineText(this, "XYZを貼付", "原子数、コメント、元素と座標（Å）",
                                                      QApplication::clipboard()->text(), &ok);
        if (ok) {
            auto next = xyzStructure(text);
            // Like the coordinate table, keep calculation/execution settings and the envelope.
            edit("XYZを読込", [next](Document &d) { replaceCoordinateStructure(d, next); }, true);
            view->fit();
        }
    });
    file->addSeparator();
    action(file, "終了", QKeySequence::Quit, [this] { close(); });
    auto *editMenu = menuBar()->addMenu("編集");
    action(editMenu, "元に戻す", QKeySequence::Undo, [this] {
        if (candidate)
            cancelCandidate();
        else
            undo.undo();
    });
    auto *redo = action(editMenu, "やり直す", QKeySequence::Redo, [this] {
        if (!candidate)
            undo.redo();
    });
    redo->setShortcuts({QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")});
    action(editMenu, "番号で原子を選択…", {}, [this] { selectNumbers(); });
    action(editMenu, "座標表（2 座標）", {}, [this] { workspaceTabs->setCurrentWidget(coordinates); });
    action(editMenu, "座標と原子属性の一覧…", {}, [this] { coordinateDialog(); });
    action(editMenu, "構造を確認…", {}, [this] { showReport(); });
    action(editMenu, "点群・対称化…", {}, [this] { symmetryDialog(); })->setObjectName("symmetryAction");
    action(editMenu, "Z-matrix（内部座標）…", {}, [this] {
        zMatrixDialog();
    })->setObjectName("zMatrixAction");
    action(editMenu, "配座探索…", {}, [this] {
        bool ok;
        const int count =
            QInputDialog::getInt(this, "配座探索",
                                 "回転可能な結合をランダムに回して力場で最適化し、エネルギーの低い"
                                 "配座を探します（MMFF94、使えない場合UFF）。\n残す配座の数",
                                 10, 1, 100, 1, &ok);
        if (ok)
            runWorker("conformers", {}, {}, count);
    })->setObjectName("conformerAction");
    action(editMenu, "構造を比較・重ね合わせ…", {}, [this] {
        compareDialog();
    })->setObjectName("compareAction");
    editMenu->addSeparator();
    // Preferences hold the executable, launcher and environment: only the user may change them.
    auto *preferencesAction =
        action(editMenu, "環境設定…", QKeySequence::Preferences, [this] { preferences(); });
    preferencesAction->setObjectName("preferencesAction");
    preferencesAction->setProperty("mcpProtected", true);
    auto *viewMenu = menuBar()->addMenu("表示");
    auto *water = viewMenu->addAction("起動時に水分子を読み込む");
    water->setCheckable(true);
    water->setChecked(settings.value("startupWater", true).toBool());
    connect(water, &QAction::toggled, this, [this](bool on) { settings.setValue("startupWater", on); });
    auto *styles = viewMenu->addMenu("表示スタイル");
    auto *styleGroup = new QActionGroup(this);
    const auto chosenStyle = settings.value("view/style", "ball_and_stick").toString();
    for (const auto &[label, style, name] :
         {std::tuple{"ボール＆スティック", "ball_and_stick", "viewStyleBallAndStick"},
          {"スティック", "stick", "viewStyleStick"},
          {"空間充填（CPK）", "spacefill", "viewStyleSpacefill"},
          {"ワイヤーフレーム", "wireframe", "viewStyleWireframe"}}) {
        auto *a = styles->addAction(label);
        a->setObjectName(name);
        a->setCheckable(true);
        a->setChecked(chosenStyle == style);
        styleGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, style = QString(style)] {
            view->setStyle(style);
            settings.setValue("view/style", style);
        });
    }
    view->setStyle(chosenStyle);
    auto *hideAction = action(viewMenu, "選択した原子を隠す", QKeySequence("Ctrl+Shift+H"), [this] {
        auto hidden = view->hiddenAtoms();
        for (const auto &id : requireSelection())
            hidden.insert(id);
        setSelection({});
        view->setHidden(hidden);
        statusBar()->showMessage(
            QString("%1原子を隠しています（表示のみ。計算・保存には含みます）。").arg(hidden.size()), 8000);
    });
    hideAction->setObjectName("viewHideSelection");
    action(viewMenu, "すべての原子を表示", QKeySequence("Ctrl+Shift+U"), [this] {
        view->setHidden({});
    })->setObjectName("viewShowAll");
    viewMenu->addSeparator();
    auto *hbondAction = viewMenu->addAction("水素結合を表示");
    hbondAction->setObjectName("viewHydrogenBonds");
    hbondAction->setCheckable(true);
    hbondAction->setChecked(settings.value("view/hydrogen_bonds", false).toBool());
    hbondAction->setToolTip(
        "N・O・Fに結合したHと、N・O・Fの間（H···Y 1.5〜2.6 Å、X–H···Y 120°以上）を破線で表示します。");
    view->setHydrogenBonds(hbondAction->isChecked());
    connect(hbondAction, &QAction::toggled, this, [this](bool on) {
        view->setHydrogenBonds(on);
        settings.setValue("view/hydrogen_bonds", on);
    });
    action(viewMenu, "選択中の計測を画面に表示", QKeySequence("Ctrl+M"), [this] {
        if (selected.size() < 2 || selected.size() > 4)
            fail("2〜4原子を順に選択してください（距離・角度・二面角）。");
        measure(displayDocument(), selected); // Rejects coincident atoms before adding.
        monitors.append(selected);
        view->setMonitors(monitors);
    })->setObjectName("viewAddMonitor");
    action(viewMenu, "画面の計測表示を消去", {}, [this] {
        monitors.clear();
        view->setMonitors({});
    })->setObjectName("viewClearMonitors");
    viewMenu->addSeparator();
    action(viewMenu, "画面をPNGで保存…", {}, [this] {
        QString p = QFileDialog::getSaveFileName(this, "画面を保存", "molforma.png", "PNG (*.png)");
        if (!p.isEmpty() && !grab().save(p))
            fail("画像を書き出せません。");
    });
    auto *help = menuBar()->addMenu("ヘルプ");
    action(help, "操作ガイド・マニュアル", QKeySequence::HelpContents, [this] {
        showManual();
    })->setObjectName("manualAction");
    action(help, "MolFormaについて", {}, [this] {
        QMessageBox::about(
            this, "MolForma",
            QString("MolForma %1\nNWChem向けの分子作成・計算入力・実行・結果表示（Qt6 / C++）\n"
                    "KAGAMI GUIを基に移植\n部品・元素データ: KAGAMI (Apache-2.0)\n"
                    "化学ファイル・力場: Open Babel\n立体判定・残基認識（RDKit）: %2\n3D描画: %3")
                .arg(MOLFORMA_VERSION, hasStereochemistryBackend() ? "有効" : "無効",
                     view->rendererName() == "vtk" ? "VTK（OpenGL）" : "QPainter（CPU）"));
    });
    auto *bar = addToolBar("ファイル");
    bar->setMovable(false);
    bar->addActions(toolbarActions);
    bar->addAction(saveAction);
}
void MainWindow::refreshPalette() {
    QString cat = value(category), needle = search->text().trimmed();
    periodicTableButton->setVisible(cat == "all" || cat == "atom");
    search->setPlaceholderText(cat == "atom"     ? "周期表で元素記号・名前・番号を検索"
                               : cat == "center" ? "元素記号・名前・原子番号を検索"
                                                 : "名前・元素・SMILESを検索");
    growth->setVisible(cat == "amino");
    if (cat != "all" && cat != "amino")
        growth->setChecked(false);
    QString old = palette->currentItem() ? palette->currentItem()->data(Qt::UserRole).toString() : "";
    palette->clear();
    auto add = [&](const QString &label, const QString &id, const QString &searchText,
                   const QJsonObject &t = QJsonObject()) {
        if (!(id.startsWith("atom:") && cat == "atom") && !needle.isEmpty() &&
            !searchText.contains(needle, Qt::CaseInsensitive))
            return;
        auto *item = new QListWidgetItem(label, palette);
        item->setData(Qt::UserRole, id);
        if (!t.isEmpty())
            item->setData(TemplateDelegate::TemplateRole, t);
        if (id.startsWith("atom:"))
            item->setData(TemplateDelegate::ElementRole, id.mid(5));
        else if (id.startsWith("center:")) {
            const auto symbol = id.mid(7);
            const auto &e = element(symbol);
            item->setData(TemplateDelegate::ElementRole, e.symbol);
            item->setData(TemplateDelegate::ElementDetailRole,
                          QString("配位中心 · 原子番号 %1").arg(e.number));
        }
        item->setToolTip(searchText);
        item->setData(Qt::AccessibleTextRole, searchText);
        if (id == old)
            palette->setCurrentItem(item);
    };
    if (cat == "all" || cat == "atom") {
        const auto &e = element(paletteElement);
        add(e.nameJa, "atom:" + e.symbol,
            QString("%1  %2  %3\n原子番号 %4").arg(e.symbol, e.nameJa, e.nameEn).arg(e.number));
    }
    if (cat == "all" || cat == "center")
        for (auto s :
             QStringList{"Li", "Na", "Mg", "Al", "K",  "Ca", "Ti", "V",  "Cr", "Mn", "Fe", "Co", "Ni",
                         "Cu", "Zn", "Ru", "Rh", "Pd", "Ag", "Cd", "Pt", "Au", "La", "Ce", "Gd"}) {
            const auto &e = element(s);
            add(e.nameJa, "center:" + s,
                QString("%1  %2  %3\n金属 配位中心 · 原子番号 %4")
                    .arg(e.symbol, e.nameJa, e.nameEn)
                    .arg(e.number));
        }
    QJsonArray all = templates();
    for (auto t : customParts)
        all.append(t);
    for (auto v : all) {
        auto t = v.toObject();
        bool mine = t["id"].toString().startsWith("my:");
        if (cat != "all" && !(cat == "my" ? mine : t["category"] == cat))
            continue;
        QString text = t["name_ja"].toString() + "  " + t["name_en"].toString() + " " +
                       t["smiles"].toString() + " " + idsFromJson(t["aliases"]).join(' ');
        QString label = t["name_ja"].toString(t["name_en"].toString());
        if (t["category"] == "amino") {
            int variants = 0;
            for (auto other : templates())
                if (other.toObject()["category"] == "amino" && other.toObject()["code"] == t["code"])
                    ++variants;
            if (variants > 1) {
                QStringList states;
                bool histidine = t["code"] == "HIS";
                if (histidine)
                    states << t["state"].toString();
                int charge = t["formal_charge"].toInt();
                if (charge)
                    states << (charge > 0 ? "+" : "−") + QString::number(std::abs(charge));
                else if (!histidine)
                    states << "中性";
                label += "（" + states.join(", ") + "）";
            }
        }
        add(label, t["id"].toString(), text, t);
    }
    if (!palette->currentItem() && palette->count())
        palette->setCurrentRow(0);
}
QJsonObject MainWindow::currentTemplate() const {
    if (!palette->currentItem())
        fail("左のパレットから部品を選択してください。");
    QString id = palette->currentItem()->data(Qt::UserRole).toString();
    for (auto t : customParts)
        if (t.toObject()["id"] == id)
            return t.toObject();
    return findTemplate(id);
}
void MainWindow::paletteChosen() {
    if (!palette->currentItem())
        return;
    QString id = palette->currentItem()->data(Qt::UserRole).toString();
    QString mode;
    if (id.startsWith("atom:")) {
        mode = "atom";
        elementBox->setCurrentIndex(elementBox->findData(id.mid(5)));
    } else if (id.startsWith("center:")) {
        mode = "center";
        elementBox->setCurrentIndex(elementBox->findData(id.mid(7)));
        properties->setCurrentIndex(1);
    } else {
        auto t = currentTemplate();
        QString cat = t["category"].toString();
        mode = cat == "substituent" ? "substituent" : cat == "ligand" ? "ligand" : "place";
        if (cat == "amino") {
            properties->setCurrentIndex(2);
            if (growth->isChecked() && !growthResidue.isEmpty() && !candidate)
                addAmino(growthResidue, growthN);
        }
    }
    tool->setCurrentIndex(tool->findData(mode));
    view->setAminoHandles(id.startsWith("amino:"));
}
Ids MainWindow::requireSelection(int count) const {
    if (selected.isEmpty() || (count && selected.size() != count))
        fail(count ? QString("%1原子を選択してください。").arg(count) : "原子を選択してください。");
    return selected;
}
Ids MainWindow::expandedSelection(const Ids &ids) const {
    const auto &d = displayDocument();
    QString mode = value(unitBox);
    if (mode == "atom" || mode == "bond" || ids.isEmpty())
        return ids;
    Ids out;
    for (auto id : ids) {
        Ids group;
        if (mode == "all")
            return d.ids();
        if (mode == "component")
            group = d.component(id);
        else if (mode == "residue") {
            auto rid = residueForAtom(d, id);
            if (!rid.isEmpty())
                for (auto a : residue(d, rid)["atoms"].toObject())
                    group << a.toString();
        } else if (mode == "part") {
            for (auto v : d.data["parts"].toArray()) {
                auto list = idsFromJson(v.toObject()["atoms"]);
                if (list.contains(id)) {
                    group = list;
                    break;
                }
            }
        }
        if (group.isEmpty())
            group << id;
        for (auto a : group)
            if (!out.contains(a))
                out << a;
    }
    return out;
}
void MainWindow::setSelection(const Ids &ids) {
    selected = ids;
    selected.removeIf([this](auto id) { return !displayDocument().contains(id); });
    view->setSelection(selected);
    refreshSelection();
}
void MainWindow::atomClicked(const QString &id, Qt::KeyboardModifiers mods) {
    QString mode = value(tool);
    if (mode == "select" || mode == "move") {
        auto next = selected;
        auto group = expandedSelection({id});
        if (next.contains(id)) {
            for (auto a : group)
                next.removeAll(a);
        } else {
            for (auto a : group)
                if (!next.contains(a))
                    next << a;
        }
        setSelection(next);
    } else if (mode == "bond") {
        if (selected.size() != 1) {
            setSelection({id});
            return;
        }
        auto a = selected.first();
        auto kind = value(bondKind);
        edit("結合を変更", [=, this](Document &d) {
            d.setBond(a, id, kind);
            if (autoH->isChecked())
                completeHydrogens(d, {a, id});
        });
    } else {
        place(id, doc.atom(id).position);
        if (mods.testFlag(Qt::ShiftModifier))
            return;
    }
}
void MainWindow::place(const QString &id, const Vec &position) {
    QString mode = value(tool);
    auto symbol = value(elementBox), kind = value(bondKind);
    bool hydrogen = autoH->isChecked();
    if (mode == "atom")
        edit("原子を追加", [=](Document &d) { addAtom(d, symbol, position, id, kind, hydrogen); });
    else if (mode == "center") {
        QString g = value(geometryBox);
        double dist = distance->value();
        int ox = oxidation->value();
        edit("配位中心を配置", [=](Document &d) { addCenter(d, symbol, g, position, dist, ox); });
    } else if (mode == "charge") {
        double q = chargeValue->value();
        edit("点電荷を配置", [=](Document &d) {
            auto points = d.data["point_charges"].toArray();
            points.append(QJsonObject{{"id", newId()},
                                      {"position", vectorToJson(position)},
                                      {"charge", q},
                                      {"source", "manual"},
                                      {"source_unit", "angstrom"}});
            d.data["point_charges"] = points;
        });
    } else if (mode == "delete") {
        if (id.isEmpty())
            return;
        auto ids = expandedSelection({id});
        edit("選択を削除", [=](Document &d) { d.remove(ids); }, true);
    } else if (mode == "replace") {
        if (id.isEmpty())
            return;
        auto ids = expandedSelection({id});
        auto item = palette->currentItem();
        auto key = item ? item->data(Qt::UserRole).toString() : QString();
        if (key.startsWith("atom:") || key.startsWith("center:") || key.isEmpty())
            edit("元素を変更", [=](Document &d) { replaceElement(d, ids, symbol, hydrogen); });
        else {
            auto t = currentTemplate();
            double angle = twist->value();
            edit("部品を置換", [=](Document &d) { replacePart(d, ids, t, angle); }, true);
        }
    } else if (mode == "ligand")
        fail("番号付きの配位サイトをクリックしてください。");
    else if (mode == "place" || mode == "substituent") {
        auto t = currentTemplate();
        double angle = twist->value();
        if (t["category"] == "amino") {
            if (!id.isEmpty()) {
                auto rid = residueForAtom(doc, id);
                if (rid.isEmpty())
                    fail("ペプチドの末端の＋を選択してください。");
                auto r = residue(doc, rid), names = r["atoms"].toObject();
                QString parent = id;
                if (doc.atom(id).element == "H" && doc.neighbors(id).size() == 1)
                    parent = doc.neighbors(id).first();
                if (parent == names["N"].toString())
                    addAmino(rid, true);
                else if (parent == names["C"].toString() || parent == names["OXT"].toString())
                    addAmino(rid, false);
                else
                    fail("N末端またはC末端を選択してください。");
                return;
            }
            QString code = t["code"].toString(), state = t["state"].toString("default"),
                    ch = value(chiralityBox), nt = value(nTerm), ct = value(cTerm);
            edit("アミノ酸を配置", [=](Document &d) {
                Ids before = d.ids();
                buildPeptide(d, {code}, nt, ct, ch, state);
                Ids added;
                for (auto a : d.ids())
                    if (!before.contains(a))
                        added << a;
                if (!before.isEmpty())
                    transform(d, added, Mat::Identity(), position);
            });
        } else if (!id.isEmpty())
            edit("置換基を接続", [=](Document &d) { attachPart(d, id, t, angle); });
        else if (mode == "substituent")
            fail("置換する水素へクリックまたはドロップしてください。");
        else
            edit("部品を配置",
                 [=](Document &d) { placeTemplate(d, t, d.atoms.isEmpty() ? Vec::Zero() : position); });
        if (!keepTool->isChecked())
            tool->setCurrentIndex(0);
    } else
        return;
    if (doc.atoms.size() < 10)
        view->fit();
}
void MainWindow::refresh() {
    if (!view)
        return;
    refreshing = true;
    const auto &d = displayDocument();
    view->setDocument(d);
    view->setSelection(selected);
    // Monitors of removed atoms disappear with them.
    monitors.removeIf([&](const Ids &ids) {
        return std::any_of(ids.begin(), ids.end(), [&](const auto &id) { return !d.contains(id); });
    });
    view->setMonitors(monitors);
    candidateBar->setVisible(candidate.has_value());
    candidateLabel->setText("未確定: " + candidateName + " — 内容を確認してください");
    summary->setText(QString("%1  |  %2 原子  %3 結合  |  電荷 %4 / 多重度 %5  |  %6点電荷%7")
                         .arg(d.formula())
                         .arg(d.atoms.size())
                         .arg(d.bonds.size())
                         .arg(d.job()["charge"].toInt())
                         .arg(d.job()["multiplicity"].toInt(1))
                         .arg(d.data["point_charges"].toArray().size())
                         .arg(candidate ? "  ·  変更候補" : ""));
    setWindowTitle(QString("%1%2 — MolForma")
                       .arg(path.isEmpty() ? "新規分子" : QFileInfo(path).fileName())
                       .arg(undo.isClean() && !candidate ? "" : " *"));
    QStringList seq;
    residueList->clear();
    for (auto v : d.data["residues"].toArray()) {
        auto r = v.toObject();
        QString label = r["code"].toString() + " " + QString::number(r["number"].toInt());
        seq << QString("<a href=\"%1\">%2</a>").arg(r["id"].toString(), label.toHtmlEscaped());
        auto *item = new QListWidgetItem(
            label + "  " + r["n_terminal"].toString() + " / " + r["c_terminal"].toString(), residueList);
        item->setData(Qt::UserRole, r["id"].toString());
    }
    sequenceLabel->setText(
        seq.isEmpty() ? "1 分子" : (candidate ? "候補 · " : "") + QString("配列 N → C　") + seq.join(" · "));
    QString current = pointList->currentItem() ? pointList->currentItem()->data(Qt::UserRole).toString() : "";
    pointList->clear();
    int index = 0;
    for (auto v : d.data["point_charges"].toArray()) {
        auto q = v.toObject();
        Vec p = vectorFromJson(q["position"]);
        auto *item = new QListWidgetItem(QString("%1: %2 e  (%3, %4, %5)")
                                             .arg(++index)
                                             .arg(q["charge"].toDouble())
                                             .arg(p.x(), 0, 'f', 2)
                                             .arg(p.y(), 0, 'f', 2)
                                             .arg(p.z(), 0, 'f', 2),
                                         pointList);
        item->setData(Qt::UserRole, q["id"].toString());
        if (q["id"] == current)
            pointList->setCurrentItem(item);
    }
    refreshChargePanel();
    refreshing = false;
    refreshSelection();
    coordinates->refresh();
    calculation->refresh();
    advanced->refresh();
    execution->refresh();
    inputPreview->refresh();
}
void MainWindow::refreshSelection() {
    const auto &d = displayDocument();
    QStringList labels;
    for (const auto &id : selected)
        if (d.contains(id))
            labels << d.atom(id).element + QString::number(d.index(id) + 1);
    QString text = "選択順: " + labels.join(" → ");
    if (selected.size() >= 2 && selected.size() <= 4) {
        try {
            double m = measure(d, selected);
            text += QString("    %1 %2").arg(m, 0, 'f', 4).arg(selected.size() == 2 ? "Å" : "°");
            dimension->setValue(m);
        } catch (const std::exception &e) {
            text += "  " + QString::fromUtf8(e.what());
        }
    }
    measurement->setText(selected.isEmpty() ? "原子を順にクリックして距離・角度・二面角を測定" : text);
    if (selected.size() == 1 && d.contains(selected.first())) {
        const auto &a = d.atom(selected.first());
        for (int i = 0; i < 3; ++i)
            positionFields[i]->setValue(a.position[i]);
        formal->setValue(a.charge);
        radical->setValue(a.radical);
        mass->setText(a.isotope    ? QString::number(a.isotope)
                      : a.mass > 0 ? QString::number(a.mass, 'g', 12)
                                   : "");
    } else if (selected.size() > 1) {
        formal->setValue(-99);
        radical->setValue(-1);
        mass->clear();
        mass->setPlaceholderText("混在（空欄は変更なし）");
    }
    if (selected.size() == 2)
        if (auto b = d.bond(selected[0], selected[1]))
            bondKind->setCurrentIndex(bondKind->findData(b->kind));
    if (coordinates)
        coordinates->refreshSelection();
}
bool MainWindow::resolveCandidate() {
    if (!candidate)
        return true;
    QMessageBox box(QMessageBox::Question, "変更候補があります",
                    "保存・読込の前に候補の扱いを選択してください。", QMessageBox::NoButton, this);
    auto *yes = box.addButton("確定して続ける", QMessageBox::AcceptRole);
    auto *no = box.addButton("取消して続ける", QMessageBox::DestructiveRole);
    box.addButton("編集に戻る", QMessageBox::RejectRole);
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
    if (auto *focused = QApplication::focusWidget();
        focused && (calculation->isAncestorOf(focused) || advanced->isAncestorOf(focused)))
        focused->clearFocus();
    if (!resolveCandidate())
        return false;
    if (undo.isClean())
        return true;
    auto answer = QMessageBox::question(this, "未保存の変更", "現在のプロジェクトを保存しますか？",
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                        QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    return answer != QMessageBox::Save || save();
}
void MainWindow::newDocument() {
    if (!confirmDiscard())
        return;
    edit("新規分子", [](Document &d) {
        d = Document();
        applyExecutionPreferences(d);
    });
    path.clear();
    view->fit();
    refresh();
}
void MainWindow::open() {
    QString p = QFileDialog::getOpenFileName(
        this, "分子を開く", settings.value("directory").toString(),
        "分子 (*.kgui.json *.json *.xyz *.mol *.sdf *.mol2 *.cml *.pdb *.ent *.nw *.gjf *.com);;"
        "NWChem入力 (*.nw);;Gaussian入力 (*.gjf *.com);;すべて (*)");
    if (p.isEmpty() || !confirmDiscard())
        return;
    loadPath(p);
}
void MainWindow::loadPath(const QString &p) {
    checkMcpPath(p); // Also for recent files and other menu paths reached over MCP.
    Document next;
    if (QFileInfo(p).suffix().toLower() == "pdb" || QFileInfo(p).suffix().toLower() == "ent") {
        auto text = QString::fromUtf8(readFile(p));
        auto models = pdbModels(text);
        bool ok = true;
        QString model = models.first();
        if (models.size() > 1)
            model = QInputDialog::getItem(this, "PDB MODEL", "読み込むモデル", models, 0, false, &ok);
        if (!ok)
            return;
        QString alt;
        if (text.contains(QRegularExpression("(?m)^(ATOM  |HETATM).{10}[A-Z]"))) {
            alt = QInputDialog::getItem(this, "PDB ALTLOC", "代替位置（自動: 残基ごとの占有率最大）",
                                        {"自動", "A", "B", "C"}, 0, true, &ok);
            if (!ok)
                return;
            if (alt == "自動")
                alt.clear();
        }
        next = importStructure(p, model.toInt(), alt);
    } else if (QFileInfo(p).suffix().toLower() == "xyz") {
        next = importStructure(p, 1, {}, chooseXyzFrame(p));
    } else if (QFileInfo(p).suffix().toLower() == "nw") {
        auto imported = importNwchemInput(QString::fromUtf8(readFile(p)));
        next = imported.document;
        if (!imported.notes.isEmpty())
            QMessageBox::information(this, "NWChem入力の取込み",
                                     "構造・電荷・多重度と、MolFormaで扱える計算条件を取り込みました。\n\n"
                                     "取り込まなかった項目:\n・" +
                                         imported.notes.mid(0, 20).join("\n・"));
    } else
        next = importStructure(p);
    if (!p.endsWith(".json", Qt::CaseInsensitive))
        applyExecutionPreferences(next); // Structure files carry no execution settings.
    loadDocument(next);
    path = p.endsWith(".json", Qt::CaseInsensitive) ? p : "";
    settings.setValue("directory", QFileInfo(p).absolutePath());
    addRecent(p);
    refresh();
}
bool MainWindow::save(bool as) {
    execution->commitPendingEdits();
    if (auto *focused = QApplication::focusWidget();
        focused && (calculation->isAncestorOf(focused) || advanced->isAncestorOf(focused)))
        focused->clearFocus(); // Include the active calculation field when saving via Ctrl+S.
    if (!resolveCandidate())
        return false;
    QString p = path;
    if (as || p.isEmpty())
        p = QFileDialog::getSaveFileName(this, "プロジェクトを保存", p.isEmpty() ? "molecule.kgui.json" : p,
                                         "KAGAMI GUI project (*.kgui.json)");
    if (p.isEmpty())
        return false;
    if (!p.endsWith(".json", Qt::CaseInsensitive))
        p += ".kgui.json";
    checkMcpPath(p); // Ctrl+S reuses the project path without a file dialog.
    doc.save(p);
    path = p;
    undo.setClean();
    addRecent(p);
    removeAutosave(); // Saved work needs no recovery copy.
    refresh();
    return true;
}
int MainWindow::chooseXyzFrame(const QString &p) {
    const auto frames = xyzFrames(QString::fromUtf8(readFile(p)));
    if (frames.size() == 1)
        return 0;
    bool ok;
    if (frames.size() > 2000) { // A long trajectory: ask for the frame number.
        const int number = QInputDialog::getInt(
            this, "XYZのフレーム",
            QString("%1フレームのXYZです。読み込むフレーム番号（既定: 最後）").arg(frames.size()),
            frames.size(), 1, frames.size(), 1, &ok);
        if (!ok)
            fail("読込を取り消しました。");
        return number - 1;
    }
    QStringList labels;
    for (int i = 0; i < frames.size(); ++i)
        labels << QString("%1: %2").arg(i + 1).arg(frames[i].section('\n', 1, 1).trimmed().left(60));
    const auto choice = QInputDialog::getItem(
        this, "XYZのフレーム", QString("%1フレームのXYZです。読み込む構造（既定: 最後）").arg(frames.size()),
        labels, labels.size() - 1, false, &ok);
    if (!ok)
        fail("読込を取り消しました。");
    return labels.indexOf(choice);
}
void MainWindow::exportFile() {
    if (!resolveCandidate())
        return;
    QString p = QFileDialog::getSaveFileName(this, "構造を書き出す", "molecule.xyz",
                                             "XYZ (*.xyz);;SDF (*.sdf);;MOL (*.mol);;Mol2 (*.mol2);;CML "
                                             "(*.cml);;PDB (*.pdb);;Gaussian入力 (*.gjf)");
    if (!p.isEmpty())
        exportStructure(doc, p);
}
void MainWindow::addFragmentFromSmiles() {
    bool ok;
    const auto smiles =
        QInputDialog::getText(this, "SMILESから断片を追加",
                              "追加する分子のSMILES（今の分子の+X側に置きます）", QLineEdit::Normal, "", &ok);
    if (ok && !smiles.trimmed().isEmpty())
        runWorker("fragment", smiles);
}
void MainWindow::addFragmentFromFile() {
    const auto p =
        QFileDialog::getOpenFileName(this, "断片を追加", settings.value("directory").toString(),
                                     "構造 (*.xyz *.mol *.sdf *.mol2 *.cml *.pdb *.ent *.nw *.gjf *.com)");
    if (p.isEmpty())
        return;
    // PDB: model 1 and automatic alternate locations.
    const auto fragment =
        importStructure(p, 1, {}, p.endsWith(".xyz", Qt::CaseInsensitive) ? chooseXyzFrame(p) : -1);
    edit("ファイルから断片を追加", [&](Document &d) { insertFragment(d, fragment); }, true);
    view->fit();
}
void MainWindow::importSmiles() {
    bool ok;
    QString smiles = QInputDialog::getText(this, "SMILESから作成", "SMILES", QLineEdit::Normal, "", &ok);
    if (ok && !smiles.trimmed().isEmpty())
        runWorker("smiles", smiles);
}
void MainWindow::runWorker(const QString &mode, const QString &smiles, const Ids &moving, int count) {
    if (worker || candidate)
        fail("処理中または変更候補があります。先に完了・取消してください。");
    if (mode == "smiles") {
        Document replaced = doc; // Fail before starting when fixed atoms block the replacement.
        replaced.remove(replaced.ids());
    }
    workdir = std::make_unique<QTemporaryDir>();
    if (!workdir->isValid())
        fail("一時ディレクトリを作成できません。");
    QString input = workdir->filePath("input.json"), output = workdir->filePath("output.json");
    QJsonObject request{{"mode", mode},
                        {"smiles", smiles},
                        {"moving", idsToJson(moving)},
                        {"count", count},
                        {"document", doc.toJson()}};
    writeFile(input, QJsonDocument(request).toJson());
    workerBase = identity(doc);
    workerRemote = mcp.depth > 0;
    worker = new QProcess(this);
    auto *running = worker;
    running->setProgram(QCoreApplication::applicationFilePath());
    running->setArguments({"--worker", input, output});
    cancelWorkerButton->show();
    statusBar()->showMessage("構造を生成・整形しています…");
    connect(running, &QProcess::finished, this,
            [this, running, output, mode](int code, QProcess::ExitStatus status) {
                if (worker != running)
                    return;
                QString error;
                try {
                    auto response = parseJson(readFile(output));
                    if (code != 0 || status != QProcess::NormalExit || response.contains("error"))
                        fail(response["error"].toString("整形に失敗しました。"));
                    if (workerBase != identity(doc))
                        fail("分子が変更されたため整形結果を破棄しました。");
                    Document next = doc;
                    if (mode == "conformers") {
                        const auto list = response["conformers"].toArray();
                        if (list.isEmpty())
                            fail("配座が見つかりませんでした。");
                        int chosen = 0;
                        if (!workerRemote && list.size() > 1) {
                            // MCP-started searches take the lowest energy without a dialog.
                            const double lowest = list[0].toObject()["energy"].toDouble();
                            QStringList labels;
                            for (int i = 0; i < list.size(); ++i)
                                labels
                                    << QString("%1: ΔE %2 kcal/mol（力場エネルギー %3）")
                                           .arg(i + 1)
                                           .arg(list[i].toObject()["energy"].toDouble() - lowest, 0, 'f', 2)
                                           .arg(list[i].toObject()["energy"].toDouble(), 0, 'f', 2);
                            bool ok;
                            const auto item = QInputDialog::getItem(
                                this, "配座探索",
                                QString("%1個の配座が見つかりました。候補にする配座").arg(list.size()),
                                labels, 0, false, &ok);
                            if (!ok)
                                fail("配座の選択を取り消しました。");
                            chosen = labels.indexOf(item);
                        }
                        applyResultGeometry(
                            next, Document::fromJson(list[chosen].toObject()["document"].toObject()));
                        candidate = next;
                        candidateName = "配座を適用";
                        candidateRemote = workerRemote;
                        statusBar()->showMessage(
                            QString("配座 %1/%2 を候補にしました。「変更を確定」で適用します。")
                                .arg(chosen + 1)
                                .arg(list.size()),
                            10000);
                    } else {
                        auto result = Document::fromJson(response["document"].toObject());
                        // The worker exchanges the document without the project envelope.
                        if (mode == "smiles") {
                            replaceCoordinateStructure(next, result);
                            next.setJobValue("charge", next.formalCharge());
                        } else if (mode == "fragment")
                            insertFragment(next, result);
                        else {
                            next = result;
                            next.envelope = doc.envelope;
                        }
                        candidate = next;
                        candidateName = mode == "smiles"     ? "SMILESから分子を作成"
                                        : mode == "fragment" ? "SMILESから断片を追加"
                                                             : "構造を整形";
                        candidateRemote = workerRemote;
                    }
                } catch (const std::exception &e) {
                    error = QString::fromUtf8(e.what());
                }
                worker = nullptr;
                running->deleteLater();
                cancelWorkerButton->hide();
                workdir.reset();
                refresh();
                view->fit();
                if (!error.isEmpty())
                    QMessageBox::warning(this, "整形結果", error);
            });
    connect(running, &QProcess::errorOccurred, this, [this, running](QProcess::ProcessError error) {
        if (worker != running || error != QProcess::FailedToStart)
            return;
        QString message = running->errorString();
        worker = nullptr;
        running->deleteLater();
        workdir.reset();
        cancelWorkerButton->hide();
        refreshChargePanel();
        coordinates->refresh();
        calculation->refresh();
        advanced->refresh();
        execution->refresh();
        inputPreview->refresh();
        QMessageBox::warning(this, "処理を起動できません", message);
    });
    running->start();
    refreshChargePanel();
    coordinates->refresh();
    calculation->refresh();
    advanced->refresh();
    execution->refresh();
    inputPreview->refresh();
}
void MainWindow::cancelWorker() {
    if (!worker)
        return;
    auto *process = worker;
    worker = nullptr;
    process->disconnect(this);
    process->kill();
    process->waitForFinished(2000);
    process->deleteLater();
    workdir.reset();
    if (cancelWorkerButton)
        cancelWorkerButton->hide();
    if (chargeEditor)
        refreshChargePanel();
    if (coordinates)
        coordinates->refresh();
    if (calculation)
        calculation->refresh();
    if (advanced)
        advanced->refresh();
    if (execution)
        execution->refresh();
    if (inputPreview)
        inputPreview->refresh();
    statusBar()->showMessage("処理を中止しました。", 4000);
}
QStringList MainWindow::recentFiles() const {
    return settings.value("recentFiles").toStringList();
}
void MainWindow::addRecent(const QString &p) {
    const auto absolute = QFileInfo(p).absoluteFilePath();
    auto list = recentFiles();
    list.removeAll(absolute);
    list.prepend(absolute);
    settings.setValue("recentFiles", list.mid(0, 10));
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
    auto *clear = recentMenu->addAction("一覧を消去");
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
void MainWindow::startAutosave() {
    const QDir directory(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                         "/autosave");
    if (!directory.mkpath("."))
        return;
    const auto name = QString("session-%1-%2")
                          .arg(QCoreApplication::applicationPid())
                          .arg(QDateTime::currentMSecsSinceEpoch());
    autosaveLock = std::make_unique<QLockFile>(directory.filePath(name + ".lock"));
    autosaveLock->setStaleLockTime(0); // Only a dead owner makes the lock stale.
    if (!autosaveLock->tryLock(0)) {
        autosaveLock.reset();
        return;
    }
    autosavePath = directory.filePath(name + ".kgui.json");
    autosaveTimer = new QTimer(this);
    autosaveTimer->setInterval(60000);
    connect(autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    autosaveTimer->start();
}
void MainWindow::autosave() {
    if (autosavePath.isEmpty())
        return;
    if (undo.isClean()) {
        removeAutosave();
        return;
    }
    // Compare the content: undo followed by a new edit returns to the same undo index.
    const auto current = identity(doc);
    if (current == autosavedIdentity && QFileInfo::exists(autosavePath))
        return;
    try {
        auto project = doc.project();
        project["molforma_autosave"] =
            QJsonObject{{"path", path}, {"time", QDateTime::currentDateTime().toString(Qt::ISODate)}};
        writeFile(autosavePath + ".tmp", QJsonDocument(project).toJson(QJsonDocument::Compact));
        QFile::remove(autosavePath);
        QFile::rename(autosavePath + ".tmp", autosavePath);
        autosavedIdentity = current;
    } catch (const std::exception &e) {
        statusBar()->showMessage("自動保存できません: " + QString::fromUtf8(e.what()), 8000);
    }
}
void MainWindow::removeAutosave() {
    if (!autosavePath.isEmpty())
        QFile::remove(autosavePath);
    autosavedIdentity.clear();
}
void MainWindow::offerRecovery() {
    const QDir directory(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                         "/autosave");
    // Each copy belongs to a session lock; a live MolForma keeps its lock, a crashed one leaves it stale.
    for (const auto &copy : directory.entryList({"session-*.kgui.json"}, QDir::Files, QDir::Time)) {
        const auto file = directory.filePath(copy);
        if (file == autosavePath)
            continue;
        QLockFile lock(directory.filePath(copy.chopped(QString(".kgui.json").size()) + ".lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            continue; // Another MolForma window is still running and owns this copy.
        QString origin, time;
        Document recovered;
        try {
            const auto project = parseJson(readFile(file));
            origin = project["molforma_autosave"].toObject()["path"].toString();
            time = project["molforma_autosave"].toObject()["time"].toString();
            auto clean = project;
            clean.remove("molforma_autosave");
            recovered = Document::fromProject(clean);
        } catch (const std::exception &) {
            QFile::remove(file); // Unreadable: nothing to recover.
            continue;
        }
        QMessageBox box(QMessageBox::Question, "作業の復旧",
                        QString("前回のMolFormaは正常に終了しませんでした。自動保存した作業があります。\n\n"
                                "保存日時: %1\n元のファイル: %2\n分子: %3（%4原子）\n\n復旧しますか？")
                            .arg(QDateTime::fromString(time, Qt::ISODate).toString("yyyy-MM-dd HH:mm:ss"),
                                 origin.isEmpty() ? QString("（未保存の新規プロジェクト）") : origin,
                                 recovered.formula())
                            .arg(recovered.atoms.size()),
                        QMessageBox::NoButton, this);
        box.setObjectName("recoveryDialog");
        auto *restore = box.addButton("復旧する", QMessageBox::AcceptRole);
        auto *discard = box.addButton("破棄する", QMessageBox::DestructiveRole);
        box.addButton("後で決める", QMessageBox::RejectRole); // Keeps the copy for the next start.
        box.exec();
        if (box.clickedButton() == restore) {
            loadDocument(recovered);
            path.clear(); // Save explicitly; the recovered copy may differ from the original file.
            undo.resetClean();
            refresh();
            QFile::remove(file);
            statusBar()->showMessage("自動保存から復旧しました。名前を付けて保存してください。", 15000);
            return;
        }
        if (box.clickedButton() == discard)
            QFile::remove(file);
    }
}
void MainWindow::showManual() {
    initManualResources();
    QDialog dialog(this);
    dialog.setObjectName("manualDialog");
    dialog.setWindowTitle("MolForma 操作ガイド・マニュアル");
    dialog.resize(900, 720);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tips = new QLabel(
        "左のパレットを選び、3D画面へクリックまたはドロップして配置します。原子をクリックするたびに選択へ追加"
        "・解除"
        "し、2〜4原子を順に選ぶと距離・角度・二面角を測定できます。配位子・ペプチド配列・削除・整形は画面下の"
        "「変更を確定」で適用し、Escで候補を取消、Ctrl+Zで確定済みの変更を戻します。");
    tips->setWordWrap(true);
    layout->addWidget(tips);
    auto *bar = new QHBoxLayout;
    layout->addLayout(bar);
    auto *pages = new QComboBox;
    pages->setObjectName("manualPage");
    pages->addItem("機能と操作（features.md）", ":/docs/features.md");
    pages->addItem("MCPサーバー（mcp.md）", ":/docs/mcp.md");
    bar->addWidget(pages);
    auto *find = new QLineEdit;
    find->setObjectName("manualSearch");
    find->setPlaceholderText("検索（Enterで次へ）");
    find->setClearButtonEnabled(true);
    bar->addWidget(find, 1);
    auto *browser = new QTextBrowser;
    browser->setObjectName("manualText");
    browser->setOpenExternalLinks(true);
    layout->addWidget(browser, 1);
    const auto show = [browser, pages] {
        QFile file(pages->currentData().toString());
        browser->setMarkdown(file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                                            : QString("マニュアルを読み込めません。"));
    };
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
        QMessageBox::question(this, "NWChemを実行中",
                              "実行中の計算を停止してウィンドウを閉じますか？ログは保存先に残ります。",
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
    settings.setValue("keepSelection", keepSelection->isChecked());
    settings.setValue("keepTool", keepTool->isChecked());
    settings.setValue("parts", customParts);
    removeAutosave();
    autosaveLock.reset();
    e->accept();
}
void MainWindow::contextMenu(const QPoint &p, const QString &id) {
    if (!id.isEmpty() && !selected.contains(id))
        setSelection(expandedSelection({id}));
    QMenu menu(this);
    auto add = [&](const QString &text, auto fn) {
        auto *a = menu.addAction(text);
        connect(a, &QAction::triggered, this, [this, fn] { safe(fn); });
    };
    if (!selected.isEmpty()) {
        add("選択を削除（候補）", [this] {
            auto ids = requireSelection();
            edit("選択を削除", [=](Document &d) { d.remove(ids); }, true);
        });
        add("元素を変更", [this] {
            auto ids = requireSelection();
            auto symbol = value(elementBox);
            edit("元素を変更", [=](Document &d) { replaceElement(d, ids, symbol, false); });
        });
        add("固定 / 解除", [this] {
            auto ids = requireSelection();
            edit("固定を切替", [=](Document &d) {
                bool fixed = !d.atom(ids.first()).fixed;
                for (auto a : ids)
                    d.atom(a).fixed = fixed;
            });
        });
        if (selected.size() <= 3)
            add("この順で向きをそろえる", [this] {
                auto ids = requireSelection();
                edit("向きをそろえる", [=](Document &d) { orient(d, ids); });
                view->fit();
            });
        if (selected.size() == 2)
            for (auto kind : QStringList{"single", "double", "triple", "aromatic", "dative", "none"})
                add(kind, [this, kind] {
                    auto ids = requireSelection(2);
                    edit("結合を変更", [=](Document &d) { d.setBond(ids[0], ids[1], kind); });
                });
    }
    add("視点をリセット", [this] { view->viewFrom(2); });
    menu.exec(p);
}
void MainWindow::selectNumbers() {
    bool ok;
    QString s = QInputDialog::getText(this, "原子番号で選択", "例: 1,3,5-8（この順で選択）",
                                      QLineEdit::Normal, "", &ok);
    if (!ok)
        return;
    Ids ids;
    for (auto part : s.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts)) {
        auto range = part.split('-');
        bool good;
        int a = range[0].toInt(&good);
        if (!good)
            fail("原子番号が不正です。");
        int b = a;
        if (range.size() == 2) {
            b = range[1].toInt(&good);
            if (!good || b < a)
                fail("範囲が不正です。");
        }
        if (range.size() > 2 || a < 1 || b > displayDocument().atoms.size())
            fail("原子番号が範囲外です。");
        for (int i = a; i <= b; ++i)
            if (!ids.contains(displayDocument().atoms[i - 1].id))
                ids << displayDocument().atoms[i - 1].id;
    }
    setSelection(ids);
}
void MainWindow::showReport() {
    QDialog dialog(this);
    dialog.setWindowTitle("構造の確認");
    dialog.resize(680, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QPlainTextEdit(structureReport(displayDocument()));
    text->setReadOnly(true);
    layout->addWidget(text);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
void MainWindow::compareDialog() {
    if (!resolveCandidate())
        return;
    const auto p = QFileDialog::getOpenFileName(
        this, "比較する構造", settings.value("directory").toString(),
        "構造 (*.kgui.json *.json *.xyz *.mol *.sdf *.mol2 *.cml *.pdb *.ent *.nw *.gjf *.com);;すべて (*)");
    if (p.isEmpty())
        return;
    checkMcpPath(p);
    const auto reference =
        importStructure(p, 1, {}, p.endsWith(".xyz", Qt::CaseInsensitive) ? chooseXyzFrame(p) : -1);
    const auto fit = superpose(doc, reference);
    QMessageBox box(QMessageBox::Information, "構造の比較",
                    QString("%1\n\nRMSD（座標そのまま）: %2 Å\n重ね合わせ後のRMSD: %3 Å（水素以外 %4 Å）\n"
                            "重ね合わせ後の最大のずれ: %5 Å")
                        .arg(QFileInfo(p).fileName())
                        .arg(fit.raw, 0, 'f', 4)
                        .arg(fit.rmsd, 0, 'f', 4)
                        .arg(fit.heavyRmsd, 0, 'f', 4)
                        .arg(fit.maximum, 0, 'f', 4),
                    QMessageBox::Close, this);
    box.setObjectName("compareResult");
    auto *align = box.addButton("比較対象に重ねる（候補）", QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() != align)
        return;
    edit("比較対象に重ね合わせ", [&](Document &d) { transform(d, d.ids(), fit.rotation, fit.shift); }, true);
}
void MainWindow::zMatrixDialog() {
    if (!resolveCandidate())
        return;
    QDialog dialog(this);
    dialog.setObjectName("zMatrixDialog");
    dialog.setWindowTitle("Z-matrix（内部座標）");
    dialog.resize(760, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro =
        new QLabel("原子番号の順に、前の原子を参照して距離・角度・二面角で表します（結合した原子を優先）。"
                   "値を編集すると、その原子の側（結合の先）を動かして「1 分子」に反映します。");
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *table = new QTableWidget(0, 8);
    table->setObjectName("zMatrixTable");
    table->setHorizontalHeaderLabels(
        {"元素", "結合相手", "距離 / Å", "角度相手", "角度 / °", "二面角相手", "二面角 / °", ""});
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *copy = buttons->addButton("テキストをコピー", QDialogButtonBox::ActionRole);
    copy->setObjectName("zMatrixCopy");
    layout->addWidget(buttons);
    QVector<ZMatrixRow> rows;
    bool filling = false;
    const auto fill = [&] {
        QScopedValueRollback guard(filling, true);
        rows = zMatrix(doc);
        table->setRowCount(rows.size());
        for (int i = 0; i < rows.size(); ++i) {
            const auto &r = rows[i];
            // Reuse items: fill() also runs from the table's own itemChanged signal.
            const auto cell = [&](int column, const QString &text, bool editable) {
                auto *item = table->item(i, column);
                if (!item) {
                    item = new QTableWidgetItem;
                    table->setItem(i, column, item);
                }
                item->setText(text);
                item->setFlags(editable ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable
                                        : Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            };
            const auto atom = [&](int k) {
                return k >= 0 ? doc.atoms[k].element + QString::number(k + 1) : QString();
            };
            cell(0, doc.atoms[i].element + QString::number(i + 1), false);
            cell(1, atom(r.bond), false);
            cell(2, r.bond >= 0 ? QLocale().toString(r.distance, 'f', 4) : QString(), r.bond >= 0);
            cell(3, atom(r.angle), false);
            cell(4, r.angle >= 0 ? QLocale().toString(r.angleValue, 'f', 2) : QString(), r.angle >= 0);
            cell(5, atom(r.dihedral), false);
            cell(6, r.dihedral >= 0 ? QLocale().toString(r.dihedralValue, 'f', 2) : QString(),
                 r.dihedral >= 0);
            cell(7, QString(), false);
        }
    };
    fill();
    connect(table, &QTableWidget::itemChanged, &dialog, [&](QTableWidgetItem *item) {
        if (filling || item->row() >= rows.size())
            return;
        const auto &r = rows[item->row()];
        const auto id = [&](int k) { return doc.atoms[k].id; };
        Ids ids;
        if (item->column() == 2)
            ids = {id(r.bond), id(item->row())};
        else if (item->column() == 4)
            ids = {id(r.angle), id(r.bond), id(item->row())};
        else if (item->column() == 6)
            ids = {id(r.dihedral), id(r.angle), id(r.bond), id(item->row())};
        bool ok;
        const double value = QLocale().toDouble(item->text(), &ok);
        if (ids.isEmpty() || !ok) {
            fill();
            return;
        }
        safe([&] { edit("Z-matrixの値を変更", [&](Document &d) { setDimension(d, ids, value); }); });
        fill(); // Other references move with the edited side.
    });
    connect(copy, &QPushButton::clicked, &dialog, [&] {
        QApplication::clipboard()->setText(zMatrixText(doc));
        statusBar()->showMessage("Z-matrixをコピーしました。", 5000);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
void MainWindow::symmetryDialog() {
    QDialog dialog(this);
    dialog.setObjectName("symmetryDialog");
    dialog.setWindowTitle("点群・対称化");
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *tolerance = new QDoubleSpinBox;
    tolerance->setObjectName("symmetryTolerance");
    tolerance->setRange(.001, .5);
    tolerance->setDecimals(3);
    tolerance->setSingleStep(.01);
    tolerance->setValue(settings.value("symmetry/tolerance", .05).toDouble());
    tolerance->setSuffix(" Å");
    form->addRow("許容差（原子の位置）", tolerance);
    auto *result = new QLabel;
    result->setObjectName("symmetryResult");
    result->setWordWrap(true);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(result);
    auto *note =
        new QLabel("全原子（ゴースト・同位体は別の種類）で判定します。対称化は対称操作で座標を平均し、"
                   "候補として表示します。NWChemの入力はnoautosymのため、計算では対称性を使いません。");
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *apply = buttons->addButton("対称化（候補にする）", QDialogButtonBox::AcceptRole);
    apply->setObjectName("symmetryApply");
    layout->addWidget(buttons);
    PointGroup group;
    const auto evaluate = [&] {
        group = pointGroup(displayDocument(), tolerance->value());
        settings.setValue("symmetry/tolerance", tolerance->value());
        if (group.name.isEmpty())
            result->setText(displayDocument().atoms.size() > 200 ? "200原子を超える構造は判定しません。"
                                                                 : "点群を決められませんでした。");
        else
            result->setText(
                QString("点群: %1（対称操作 %2、対称な位置からの最大のずれ %3 Å）")
                    .arg(group.name)
                    .arg(group.name.contains("∞") ? QString("∞") : QString::number(group.operations.size()))
                    .arg(group.deviation, 0, 'f', 4));
        apply->setEnabled(!group.name.isEmpty() && group.name != "C1" && group.name != "Kh" &&
                          !hasCandidate() && !worker);
    };
    evaluate();
    connect(tolerance, &QDoubleSpinBox::valueChanged, &dialog, evaluate);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const double limit = tolerance->value();
    double moved = 0;
    edit(
        "点群に合わせて対称化",
        [&](Document &d) {
            // A few passes: within tolerance the found operations are only nearly a group.
            auto current = pointGroup(d, limit);
            for (int pass = 0; pass < 5 && !current.name.isEmpty(); ++pass) {
                moved = std::max(moved, symmetrize(d, current));
                const auto next = pointGroup(d, limit);
                if (next.name != current.name || next.deviation < 1e-10)
                    break;
                current = next;
            }
        },
        true);
    statusBar()->showMessage(
        QString("%1に対称化した候補です（最大の移動 %2 Å）。「変更を確定」で適用します。")
            .arg(group.name)
            .arg(moved, 0, 'f', 4),
        15000);
}
void MainWindow::preferences() {
    QDialog dialog(this);
    dialog.setObjectName("preferencesDialog");
    dialog.setProperty("mcpProtected", true); // Same protection as the executable fields in "5 実行設定".
    dialog.setWindowTitle("環境設定");
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel("新規作成、およびプロジェクト以外の構造ファイル（XYZ・SDF・PDB等）を開いたときの"
                             "実行設定です。開いているプロジェクトの設定は「5 実行設定」で変更します。");
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    const auto saved = executionPreferences();
    auto path = [&](const QString &label, const QString &key, bool directory) {
        auto *line = new QLineEdit(saved[key].toString());
        line->setObjectName("preferences_" + key);
        auto *browse = new QPushButton("参照…");
        connect(browse, &QPushButton::clicked, &dialog, [&dialog, line, directory] {
            const auto chosen =
                directory ? QFileDialog::getExistingDirectory(&dialog, "ジョブ保存先", line->text())
                          : QFileDialog::getOpenFileName(&dialog, "NWChem実行ファイル", line->text());
            if (!chosen.isEmpty())
                line->setText(chosen);
        });
        auto *row = new QHBoxLayout;
        row->addWidget(line, 1);
        row->addWidget(browse);
        form->addRow(label, row);
        return line;
    };
    auto spin = [&](const QString &label, const QString &key, int minimum, int maximum) {
        auto *box = new QSpinBox;
        box->setObjectName("preferences_" + key);
        box->setRange(minimum, maximum);
        box->setValue(saved[key].toInt());
        form->addRow(label, box);
        return box;
    };
    auto *executable = path("NWChem実行ファイル", "executable", false);
    auto *mode = new QComboBox;
    mode->addItem("MPI", "mpi");
    mode->addItem("直接実行", "direct");
    mode->setCurrentIndex(std::max(0, mode->findData(saved["mode"].toString())));
    form->addRow("起動方式", mode);
    auto *launcher = new QLineEdit(saved["launcher"].toString());
    form->addRow("MPIランチャー", launcher);
    auto *ranks = spin("MPI起動数", "mpi_ranks", 1, 4096);
    auto *omp = spin("OpenMPスレッド / プロセス", "omp_threads", 1, 1024);
    auto *blas = spin("BLASスレッド / プロセス", "blas_threads", 1, 1024);
    auto *stack = new QLineEdit(saved["omp_stacksize"].toString());
    stack->setPlaceholderText("空欄で継承（例: 64M）");
    form->addRow("OpenMPスタック", stack);
    auto *root = path("ジョブ保存先", "job_root", true);
    auto *timeout = spin("制限時間 / 秒（0: なし）", "timeout_seconds", 0, 604800);
    auto *environment = new QPlainTextEdit;
    environment->setPlaceholderText(
        "1行に1つ: NAME=VALUE（例: NWCHEM_BASIS_LIBRARY=/opt/nwchem/libraries/）");
    QStringList lines;
    for (const auto &value : saved["environment"].toArray()) {
        const auto row = value.toObject();
        if (row["enabled"].toBool())
            lines << row["name"].toString() + '=' + row["value"].toString();
    }
    environment->setPlainText(lines.join('\n'));
    environment->setMaximumHeight(90);
    form->addRow("追加の環境変数", environment);
    auto *renderer = new QComboBox;
    renderer->addItem("自動（VTKを優先）", "auto");
    renderer->addItem("QPainter（CPU）", "painter");
    renderer->setCurrentIndex(settings.value("view/renderer").toString() == "painter" ? 1 : 0);
    form->addRow("3D描画（再起動後に反映）", renderer);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *current = buttons->addButton("現在の実行設定を読み込む", QDialogButtonBox::ResetRole);
    layout->addWidget(buttons);
    connect(current, &QPushButton::clicked, &dialog, [&] {
        const auto s = executionSettings(doc);
        executable->setText(s["executable"].toString());
        mode->setCurrentIndex(std::max(0, mode->findData(s["mode"].toString())));
        launcher->setText(s["launcher"].toString());
        ranks->setValue(s["mpi_ranks"].toInt());
        omp->setValue(s["omp_threads"].toInt());
        blas->setValue(s["blas_threads"].toInt());
        stack->setText(s["omp_stacksize"].toString());
        root->setText(s["job_root"].toString());
        timeout->setValue(s["timeout_seconds"].toInt());
        QStringList rows;
        for (const auto &value : s["environment"].toArray())
            if (value.toObject()["enabled"].toBool())
                rows << value.toObject()["name"].toString() + '=' + value.toObject()["value"].toString();
        environment->setPlainText(rows.join('\n'));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        safe([&] {
            QJsonArray rows;
            for (const auto &line : environment->toPlainText().split('\n', Qt::SkipEmptyParts)) {
                const auto at = line.indexOf('=');
                if (at <= 0)
                    fail("環境変数は NAME=VALUE の形式で1行に1つ入力してください: " + line);
                rows.append(QJsonObject{
                    {"enabled", true}, {"name", line.left(at).trimmed()}, {"value", line.mid(at + 1)}});
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
            // Check the values with the same rules as "5 実行設定", ignoring the molecule itself.
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
            settings.setValue("view/renderer", renderer->currentData().toString());
            dialog.accept();
        });
    });
    dialog.exec();
}
void MainWindow::coordinateDialog() {
    if (!resolveCandidate())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle("座標（Å）と原子属性");
    dialog.resize(820, 550);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(doc.atoms.size(), 8);
    table->setHorizontalHeaderLabels(
        {"元素", "X / Å", "Y / Å", "Z / Å", "形式電荷", "不対電子", "質量数 / amu", "原子名"});
    for (int i = 0; i < doc.atoms.size(); ++i) {
        const auto &a = doc.atoms[i];
        QStringList row{a.element,
                        QString::number(a.position.x(), 'g', 14),
                        QString::number(a.position.y(), 'g', 14),
                        QString::number(a.position.z(), 'g', 14),
                        QString::number(a.charge),
                        QString::number(a.radical),
                        a.isotope ? QString::number(a.isotope)
                        : a.mass  ? QString::number(a.mass, 'g', 12)
                                  : "",
                        a.name};
        for (int j = 0; j < row.size(); ++j)
            table->setItem(i, j, new QTableWidgetItem(row[j]));
    }
    layout->addWidget(table);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, &dialog, table] {
        safe([&] {
            edit("座標と属性を変更", [&](Document &d) {
                for (int i = 0; i < d.atoms.size(); ++i) {
                    auto &a = d.atoms[i];
                    auto cell = [&](int j) { return table->item(i, j)->text().trimmed(); };
                    element(cell(0));
                    a.element = cell(0);
                    Vec p;
                    for (int j = 0; j < 3; ++j) {
                        bool ok;
                        p[j] = cell(j + 1).toDouble(&ok);
                        if (!ok || !std::isfinite(p[j]))
                            fail("座標は有限の数値にしてください。");
                    }
                    if (a.fixed && (p - a.position).norm() > 1e-7)
                        fail("固定原子の座標は変更できません。");
                    a.position = p;
                    bool ok;
                    a.charge = cell(4).toInt(&ok);
                    if (!ok)
                        fail("形式電荷は整数です。");
                    a.radical = cell(5).toInt(&ok);
                    if (!ok || a.radical < 0)
                        fail("不対電子は非負の整数です。");
                    a.isotope = 0;
                    a.mass = 0;
                    if (!cell(6).isEmpty()) {
                        a.isotope = cell(6).toInt(&ok);
                        if (!ok) {
                            a.isotope = 0;
                            a.mass = cell(6).toDouble(&ok);
                            if (!ok || a.mass <= 0)
                                fail("質量が不正です。");
                        } else if (a.isotope <= 0)
                            fail("質量数は正の整数です。");
                    }
                    a.name = cell(7);
                }
                d.invalidateAssignments();
            });
            dialog.accept();
        });
    });
    dialog.exec();
}
void MainWindow::savePart() {
    auto ids = requireSelection();
    bool ok;
    QString name = QInputDialog::getText(this, "マイ部品", "部品名", QLineEdit::Normal, "", &ok);
    if (!ok)
        return;
    const QStringList kinds{"置換基（接続点1つ）", "配位子", "骨格（接続点なし・空白に配置）"};
    QString kind = QInputDialog::getItem(
        this, "部品の種類", "種類（置換基・配位子は接続点の原子を最初に選択）", kinds, 0, false, &ok);
    if (!ok)
        return;
    auto t = makePart(doc, ids, name,
                      kind == kinds[1]   ? "ligand"
                      : kind == kinds[2] ? "skeleton"
                                         : "substituent");
    QString p = QFileDialog::getSaveFileName(this, "部品を保存", name + ".kpart.json", "部品 (*.kpart.json)");
    if (p.isEmpty())
        return;
    writeFile(p, QJsonDocument(t).toJson());
    customParts.append(t);
    settings.setValue("parts", customParts);
    refreshPalette();
}
void MainWindow::loadPart() {
    QString p = QFileDialog::getOpenFileName(this, "マイ部品を読込", {}, "部品 (*.kpart.json *.json)");
    if (p.isEmpty())
        return;
    auto t = parseJson(readFile(p));
    validateTemplate(t);
    if (!t["id"].toString().startsWith("my:"))
        fail("マイ部品のIDはmy:で始まる必要があります。");
    for (int i = customParts.size() - 1; i >= 0; --i)
        if (customParts[i].toObject()["id"] == t["id"])
            customParts.removeAt(i);
    customParts.append(t);
    settings.setValue("parts", customParts);
    category->setCurrentIndex(category->findData("my"));
    refreshPalette();
}
} // namespace mf
