#include "ui/main_window.h"
#include "editing/chemistry.h"
#include "editing/coordination.h"
#include "editing/operations.h"
#include "editing/peptide.h"
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
namespace mf {
class DocumentCommand : public QUndoCommand {
    MainWindow *window;
    Document before, after;

  public:
    DocumentCommand(MainWindow *w, const QString &name, const Document &next)
        : QUndoCommand(name), window(w), before(w->doc), after(next) {}
    void undo() override {
        window->setState(before);
    }
    void redo() override {
        window->setState(after);
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
    return QJsonDocument(d.toJson()).toJson(QJsonDocument::Compact);
}
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), settings("MolForma", "MolForma") {
    setObjectName("mainWindow");
    resize(1380, 900);
    setMinimumSize(900, 620);
    customParts = settings.value("parts").toJsonArray();
    buildUi();
    buildMenus();
    connect(&undo, &QUndoStack::cleanChanged, this, [this] { refresh(); });
    if (settings.value("startupWater", true).toBool())
        placeTemplate(doc, findTemplate("skeleton:water"), Vec::Zero());
    refresh();
    QTimer::singleShot(0, view, &MoleculeView::fit);
    auto saved = settings.value("geometry").toByteArray();
    if (!saved.isEmpty())
        restoreGeometry(saved);
}
MainWindow::~MainWindow() {
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
void MainWindow::setState(const Document &d) {
    doc = d;
    candidate.reset();
    selected.removeIf([&](auto id) { return !doc.contains(id); });
    refresh();
}
void MainWindow::loadDocument(const Document &d) {
    cancelWorker();
    candidate.reset();
    selected.clear();
    doc = d;
    undo.clear();
    undo.setClean();
    refresh();
    view->fit();
}
void MainWindow::commit(const QString &name, const Document &next) {
    auto errors = next.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    if (identity(doc) == identity(next))
        return;
    undo.push(new DocumentCommand(this, name, next));
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
    commit(name, next);
    refresh();
}
void MainWindow::cancelCandidate() {
    candidate.reset();
    refresh();
    statusBar()->showMessage("変更候補を取り消しました。", 4000);
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
    action(file, "新規", QKeySequence::New, [this] { newDocument(); });
    action(file, "開く…", QKeySequence::Open, [this] { open(); });
    action(file, "SMILESから作成…", {}, [this] { importSmiles(); });
    file->addSeparator();
    action(file, "プロジェクトを保存", QKeySequence::Save, [this] { save(); });
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
            auto next = Document::fromXyz(text);
            edit("XYZを読込", [next](Document &d) { d = next; }, true);
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
    auto *viewMenu = menuBar()->addMenu("表示");
    auto *water = viewMenu->addAction("起動時に水分子を読み込む");
    water->setCheckable(true);
    water->setChecked(settings.value("startupWater", true).toBool());
    connect(water, &QAction::toggled, this, [this](bool on) { settings.setValue("startupWater", on); });
    action(viewMenu, "画面をPNGで保存…", {}, [this] {
        QString p = QFileDialog::getSaveFileName(this, "画面を保存", "molforma.png", "PNG (*.png)");
        if (!p.isEmpty() && !grab().save(p))
            fail("画像を書き出せません。");
    });
    auto *help = menuBar()->addMenu("ヘルプ");
    action(help, "操作ガイド", QKeySequence::HelpContents, [this] {
        QMessageBox::information(
            this, "MolForma の操作",
            "左のパレットを選び、3D画面へクリックまたはドロップして配置します。\n\n原子をクリックするたびに選"
            "択へ追加・解除します。2〜4原子を順に選ぶと距離・角度・二面角を測定できます。\n\n配位子・ペプチド"
            "配列・削除・整形は画面下の「変更を確定」で適用します。Escで候補を取消、Ctrl+"
            "Zで確定済みの変更を戻します。\n\n部品・残基・点電荷を残すにはプロジェクト形式で保存してく"
            "ださい。XYZは座標の交換用です。\n\n詳しい対応状況と操作手順: docs/features.md");
    });
    action(help, "MolFormaについて", {}, [this] {
        QMessageBox::about(
            this, "MolForma",
            "MolForma 0.1\nQt6 / C++ 分子エディタ\nKAGAMI GUI「1 分子」から独立した実装\n部品・元素データ: "
            "KAGAMI (Apache-2.0)\n化学ファイル・力場: Open Babel");
    });
    auto *bar = addToolBar("ファイル");
    bar->setMovable(false);
    for (auto *a : file->actions().mid(0, 5))
        if (!a->isSeparator())
            bar->addAction(a);
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
    edit("新規分子", [](Document &d) { d = Document(); });
    path.clear();
    view->fit();
    refresh();
}
void MainWindow::open() {
    QString p =
        QFileDialog::getOpenFileName(this, "分子を開く", settings.value("directory").toString(),
                                     "分子 (*.kgui.json *.json *.xyz *.mol *.sdf *.pdb *.ent);;すべて (*)");
    if (p.isEmpty() || !confirmDiscard())
        return;
    loadPath(p);
}
void MainWindow::loadPath(const QString &p) {
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
    } else
        next = importStructure(p);
    loadDocument(next);
    path = p.endsWith(".json", Qt::CaseInsensitive) ? p : "";
    settings.setValue("directory", QFileInfo(p).absolutePath());
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
    doc.save(p);
    path = p;
    undo.setClean();
    refresh();
    return true;
}
void MainWindow::exportFile() {
    if (!resolveCandidate())
        return;
    QString p = QFileDialog::getSaveFileName(this, "構造を書き出す", "molecule.xyz",
                                             "XYZ (*.xyz);;SDF (*.sdf);;MOL (*.mol);;PDB (*.pdb)");
    if (!p.isEmpty())
        exportStructure(doc, p);
}
void MainWindow::importSmiles() {
    bool ok;
    QString smiles = QInputDialog::getText(this, "SMILESから作成", "SMILES", QLineEdit::Normal, "", &ok);
    if (ok && !smiles.trimmed().isEmpty())
        runWorker("smiles", smiles);
}
void MainWindow::runWorker(const QString &mode, const QString &smiles, const Ids &moving) {
    if (worker || candidate)
        fail("処理中または変更候補があります。先に完了・取消してください。");
    workdir = std::make_unique<QTemporaryDir>();
    if (!workdir->isValid())
        fail("一時ディレクトリを作成できません。");
    QString input = workdir->filePath("input.json"), output = workdir->filePath("output.json");
    QJsonObject request{
        {"mode", mode}, {"smiles", smiles}, {"moving", idsToJson(moving)}, {"document", doc.toJson()}};
    writeFile(input, QJsonDocument(request).toJson());
    workerBase = identity(doc);
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
                    auto next = Document::fromJson(response["document"].toObject());
                    if (mode == "smiles") {
                        next.data["job"] = doc.data["job"];
                        next.setJobValue("charge", next.formalCharge());
                    }
                    candidate = next;
                    candidateName = mode == "smiles" ? "SMILESから分子を作成" : "構造を整形";
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
    e->accept();
}
void MainWindow::contextMenu(const QPoint &p, const QString &id) {
    if (!id.isEmpty() && !selected.contains(id))
        setSelection(expandedSelection({id}));
    QMenu menu;
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
    QString kind = QInputDialog::getItem(this, "部品の種類", "種類", {"置換基", "配位子"}, 0, false, &ok);
    if (!ok)
        return;
    auto t = makePart(doc, ids, name, kind == "配位子");
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
