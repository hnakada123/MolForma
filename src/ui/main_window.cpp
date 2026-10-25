#include "ui/main_window.h"
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/coordination.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "editing/stereochemistry.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include "nwchem/nwchem_import.h"
#include "ui/advanced_panel.h"
#include "ui/calculation_panel.h"
#include "ui/coordinates_panel.h"
#include "ui/editor_chrome.h"
#include "ui/execution_panel.h"
#include "ui/history_panel.h"
#include "ui/input_preview_panel.h"
#include "ui/periodic_table.h"
#include "ui/results_panel.h"
#include "ui/template_delegate.h"
#include "ui/theme.h"
#include <Eigen/Eigenvalues>
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
    bool remote;         // Made or confirmed over MCP: undo/redo never make its execution settings trusted.
    bool pushed = false; // QUndoStack::push() runs redo() once to apply the edit itself.

  public:
    DocumentCommand(MainWindow *w, const QString &name, const Document &next, bool remote)
        : QUndoCommand(name), window(w), before(w->doc), after(next), remote(remote) {}
    void undo() override {
        window->setState(before, !remote);
        window->announceHistory("元に戻しました", text(), after, before);
    }
    void redo() override {
        window->setState(after, !remote);
        if (pushed)
            window->announceHistory("やり直しました", text(), before, after);
        pushed = true;
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
// An atom as the 3D view labels it ("C14"); atomPlace() gives its place in a residue.
static QString atomName(const Document &d, const QString &id) {
    return d.atom(id).element + QString::number(d.index(id) + 1);
}
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), settings("MolForma", "MolForma") {
    setObjectName("mainWindow");
    setAcceptDrops(true);
    resize(1380, 900);
    setMinimumSize(900, 620);
    customParts = settings.value("parts").toJsonArray();
    buildUi();
    buildMenus();
    // Commands change the document in undo()/redo(); refresh once the stack index (and so the
    // Undo/Redo texts) has moved. Saving only changes the title.
    undo.setUndoLimit(100);
    connect(&undo, &QUndoStack::indexChanged, this, [this] { refresh(); });
    connect(&undo, &QUndoStack::cleanChanged, this, &MainWindow::refreshTitle);
    if (settings.value("startupWater", false).toBool())
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
    undo.disconnect(this); // ~QUndoStack signals while clearing, after later members are gone.
}
void MainWindow::safe(const std::function<void()> &operation) {
    try {
        operation();
    } catch (const std::exception &e) {
        QString message = QString::fromUtf8(e.what());
        statusBar()->showMessage(message, 15000);
        QMessageBox::warning(this, "操作できません", message);
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
    const auto ids = doc.ids();
    const QSet<QString> present(ids.begin(), ids.end());
    selected.removeIf([&](const auto &id) { return !present.contains(id); });
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
    else if (nwchemAdvanced(from) != nwchemAdvanced(to))
        page = advanced;
    else if (from.data["nwchem_execution"] != to.data["nwchem_execution"])
        page = execution;
    else
        page = calculation; // Calculation conditions, charge and multiplicity.
    const bool elsewhere = workspaceTabs && workspaceTabs->currentWidget() != page;
    statusBar()->showMessage(
        QString("%1: %2%3")
            .arg(verb, name,
                 elsewhere ? "（「" + workspaceTabs->tabText(workspaceTabs->indexOf(page)) + "」の内容）"
                           : QString()),
        elsewhere ? 10000 : 5000);
}
bool MainWindow::commit(const QString &name, const Document &next, bool remote) {
    auto errors = next.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    if (identity(doc) == identity(next))
        return false;
    undo.push(new DocumentCommand(this, name, next, remote || mcp.depth > 0));
    if (!keepSelection->isChecked())
        setSelection({});
    statusBar()->showMessage(name, 5000);
    return true;
}
void MainWindow::edit(const QString &name, const std::function<void(Document &)> &operation, bool preview) {
    if (worker)
        fail("整形処理の完了または中止後に編集してください。");
    if (candidate)
        fail("先に変更候補を確定するか、取消してください。");
    Document next = doc;
    operation(next);
    if (!preview) {
        commit(name, next);
        return;
    }
    auto errors = next.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    if (identity(next) == identity(doc))
        return;
    candidate = next;
    candidateName = name;
    candidateRemote = mcp.depth > 0;
    refresh();
    statusBar()->showMessage("候補を確認して確定してください。", 8000);
}
void MainWindow::confirmCandidate() {
    if (!candidate)
        return;
    const Document next = *candidate;
    // A person confirming an MCP candidate does not make the settings it carries trusted. The
    // command clears the candidate; one that fails validation stays for the user to cancel.
    if (!commit(candidateName, next, candidateRemote)) {
        candidate.reset(); // Same as the document: nothing to record.
        refresh();
    }
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
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    setCentralWidget(central);
    workspaceTabs = new QTabWidget;
    workspaceTabs->setObjectName("workspaceTabs");
    workspaceTabs->setDocumentMode(true);
    // The workflow steps of the header switch the tabs, so their own bar stays hidden.
    workspaceTabs->tabBar()->hide();
    outer->addWidget(workspaceTabs, 1);
    auto *root = moleculePage = new QWidget;
    workspaceTabs->addTab(root, "1 分子");
    auto *page = new QHBoxLayout(root);
    page->setContentsMargins(0, 0, 0, 0);
    page->setSpacing(0);
    // The editing tools in a rail at the left edge: icon, name and key, the chosen one tinted.
    auto *rail = new QWidget;
    rail->setObjectName("toolRail");
    rail->setAttribute(Qt::WA_StyledBackground);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(6, 10, 6, 10);
    railLayout->setSpacing(4);
    toolGroup = new QButtonGroup(this);
    for (const auto &[code, label, key, tip] :
         {std::tuple{"select", "選択", "S",
                     "原子を選択します。Shift／Ctrl＋クリックで追加、Shiftドラッグで範囲選択。"},
          {"move", "移動", "M",
           "原子をドラッグして動かします（付いている水素と、選択中なら選択全体が一緒に動きます）。"},
          {"bond", "結合", "B", "2原子を順にクリックして、上の「結合の種類」にします。"},
          {"delete", "削除", "X", "クリックした原子を削除する候補を作ります。"},
          {"place", "配置", "A",
           "パレットで選んだ原子・部品を置きます。空白・原子・水素・配位中心の紫の番号のどれを"
           "クリックするかで置き方が決まります。"},
          {"replace", "置き換え", "R", "クリックした原子を、パレットで選んだ元素・部品に置き換えます。"},
          {"charge", "点電荷", "Q", "空白をクリックして外部点電荷を置きます。"}}) {
        auto *b = new ToolRailButton(key);
        b->setObjectName(QString("tool_") + code);
        b->setText(label);
        b->setIcon(lineIcon(code));
        b->setToolTip(tip);
        b->setProperty("tool", code);
        b->setProperty("toolKey", key);
        railLayout->addWidget(b, 0, Qt::AlignHCenter);
        toolGroup->addButton(b);
    }
    railLayout->addStretch();
    toolGroup->buttons().first()->setChecked(true);
    page->addWidget(rail);
    auto *splitter = new QSplitter;
    splitter->setObjectName("editorSplitter");
    splitter->setChildrenCollapsible(false);
    page->addWidget(splitter, 1);
    // The palette: search, categories, the common elements and the parts as tiles.
    auto *left = new QWidget;
    left->setObjectName("paletteColumn");
    left->setAttribute(Qt::WA_StyledBackground);
    left->setMinimumWidth(230); // Three columns of tiles and the element buttons.
    left->setMaximumWidth(360);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(12, 12, 8, 8);
    ll->setSpacing(8);
    auto *paletteTitle = new QLabel("部品パレット");
    auto titleFont = paletteTitle->font();
    titleFont.setBold(true);
    paletteTitle->setFont(titleFont);
    ll->addWidget(paletteTitle);
    search = new QLineEdit;
    search->setObjectName("paletteSearch");
    search->setPlaceholderText("名前・元素・SMILESを検索");
    search->setClearButtonEnabled(true);
    search->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    ll->addWidget(search);
    auto *categories = new QWidget;
    categories->setObjectName("paletteCategories");
    auto *chips = new FlowLayout(categories, 5, 5);
    chips->setContentsMargins(0, 0, 0, 0);
    categoryGroup = new QButtonGroup(this);
    for (const auto &[label, code, tip] :
         {std::tuple{"すべて", "all",
                     "原子・骨格・置換基・配位子・アミノ酸・配位中心を、見出しを付けて並べます。"},
          {"原子", "atom", "よく使う元素です。ほかの元素は周期表（H〜Og）から選びます。"},
          {"骨格", "skeleton", ""},
          {"置換基", "substituent", "置き換える水素をクリックして付けます。"},
          {"配位中心", "center", "金属の配位中心です。"},
          {"配位子", "ligand", "配位中心の紫の番号（空きサイト）をクリックして付けます。"},
          {"アミノ酸", "amino", "アミノ酸と末端キャップです。"},
          {"マイ部品", "my", "「選択原子をマイ部品に保存」で登録した部品です。"}}) {
        auto *chip = new QToolButton;
        chip->setObjectName(QString("paletteCategory_") + code);
        chip->setText(label);
        chip->setToolTip(tip);
        chip->setCheckable(true);
        chip->setProperty("category", code);
        chips->addWidget(chip);
        categoryGroup->addButton(chip);
    }
    categoryGroup->buttons().first()->setChecked(true);
    ll->addWidget(categories);
    // The common elements of organic molecules, one click each (the atom tool then places them).
    auto *elements = new QWidget;
    elements->setObjectName("quickElements");
    auto *quick = new QGridLayout(elements);
    quick->setContentsMargins(0, 0, 0, 0);
    quick->setSpacing(4);
    for (const auto *symbol : {"H", "C", "N", "O", "F", "P", "S", "Cl", "Br", "I"}) {
        auto *b = new QToolButton;
        b->setObjectName(QString("quickElement_") + symbol);
        b->setText(symbol);
        b->setToolTip(QString("%1（%2）を「配置」で置きます").arg(symbol, element(symbol).nameJa));
        b->setCheckable(true);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setMinimumWidth(16);
        const int i = quick->count();
        quick->addWidget(b, i / 6, i % 6);
        connect(b, &QToolButton::clicked, this, [this, symbol] { safe([&] { chooseElement(symbol); }); });
    }
    periodicTableButton = new QPushButton("周期表…");
    periodicTableButton->setObjectName("openPeriodicTable");
    periodicTableButton->setToolTip("周期表から原子を選択します（H〜Og）。");
    periodicTableButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    quick->addWidget(periodicTableButton, 1, 4, 1, 2);
    for (int c = 0; c < 6; ++c)
        quick->setColumnStretch(c, 1);
    connect(periodicTableButton, &QPushButton::clicked, this, [this] {
        PeriodicTableDialog dialog(paletteElement, search->text(), this);
        if (dialog.exec() == QDialog::Accepted)
            safe([&] { chooseElement(dialog.selectedElement()); });
    });
    ll->addWidget(elements);
    growth = new QCheckBox("アミノ酸の連続追加");
    ll->addWidget(growth);
    palette = new PaletteList;
    palette->setObjectName("templatePalette");
    auto *tiles = new TemplateDelegate(palette);
    tiles->setTiles(true);
    palette->setItemDelegate(tiles);
    palette->setDragEnabled(true);
    palette->setDragDropMode(QAbstractItemView::DragOnly);
    palette->setDefaultDropAction(Qt::CopyAction);
    // Tiles left to right, a group heading taking a row of its own.
    palette->setFlow(QListView::LeftToRight);
    palette->setWrapping(true);
    palette->setResizeMode(QListView::Adjust);
    palette->setSpacing(3);
    palette->setMouseTracking(true);
    palette->setFrameShape(QFrame::NoFrame);
    palette->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    palette->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn); // A fixed width keeps the columns.
    ll->addWidget(palette, 1);
    splitter->addWidget(left);
    auto *middle = new QWidget;
    auto *ml = new QVBoxLayout(middle);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->setSpacing(0);
    // One row above the 3D view: the options of the current tool, or the candidate controls in their
    // place; it keeps the height of the taller one, so the view keeps its size and position.
    topRow = new QStackedWidget;
    topRow->setObjectName("editorTopRow");
    auto *optionBar = new QWidget;
    optionBar->setObjectName("toolOptions");
    optionBar->setAttribute(Qt::WA_StyledBackground);
    auto *options = new FlowLayout(optionBar, 12, 6);
    options->setContentsMargins(10, 7, 10, 7);
    options->setUniformRows(true); // Other tools' options keep the height: the view does not move.
    const auto optionGroup = [&](const QString &name, const QList<QWidget *> &widgets) {
        auto *group = new QWidget;
        group->setObjectName(name);
        auto *row = new QHBoxLayout(group);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        for (auto *w : widgets)
            row->addWidget(w);
        options->addWidget(group);
        toolOptionGroups.insert(name, group);
        return group;
    };
    auto *units = new QFrame;
    units->setObjectName("selectionUnit");
    units->setToolTip(
        "原子をクリックしたときにまとめて選ぶ範囲です（原子・部品・残基・つながった分子・全体）。");
    auto *unitRow = new QHBoxLayout(units);
    unitRow->setContentsMargins(2, 2, 2, 2);
    unitRow->setSpacing(2);
    unitGroup = new QButtonGroup(this);
    for (const auto &[label, code] : {std::pair{"原子", "atom"},
                                      {"結合", "bond"},
                                      {"部品", "part"},
                                      {"残基", "residue"},
                                      {"分子", "component"},
                                      {"全体", "all"}}) {
        auto *b = new QToolButton;
        b->setObjectName(QString("selectionUnit_") + code);
        b->setText(label);
        b->setCheckable(true);
        b->setProperty("unit", code);
        unitRow->addWidget(b);
        unitGroup->addButton(b);
    }
    unitGroup->buttons().first()->setChecked(true);
    auto *unitLabel = new QLabel("選択単位");
    optionGroup("selectionUnitOption", {unitLabel, units});
    toolPart = new QLabel;
    toolPart->setObjectName("toolPart");
    toolPart->setTextFormat(Qt::RichText);
    optionGroup("partOption", {toolPart});
    auto *bondLabel = new QLabel("結合の種類");
    toolBondKind = new QComboBox;
    toolBondKind->setObjectName("toolBondKind");
    toolBondKind->setToolTip("「結合」ツールで作る結合と、「配置」で原子に原子をつなぐ結合の種類です。");
    toolBondKind->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    bondLabel->setBuddy(toolBondKind);
    optionGroup("bondOption", {bondLabel, toolBondKind});
    keepTool = new QCheckBox("部品の配置後もツールを保持");
    keepTool->setToolTip("オフでは、骨格・置換基・アミノ酸などの部品を1つ置くと「選択」ツールに戻ります。"
                         "原子・配位中心や配位サイトへの配位子の配置は、切り替えるまで続きます。");
    keepTool->setChecked(settings.value("keepTool", false).toBool());
    optionGroup("keepToolOption", {keepTool});
    toolDeleteTerminate = new QCheckBox("削除後に水素で終端化");
    toolDeleteTerminate->setObjectName("toolDeleteTerminate");
    toolDeleteTerminate->setChecked(true);
    toolDeleteTerminate->setToolTip("すべての削除に適用します（「編集」タブの同じ項目と共通）。");
    optionGroup("deleteOption", {toolDeleteTerminate});
    auto *chargeLabel = new QLabel("置く点電荷 / e");
    toolChargeValue = new QDoubleSpinBox;
    toolChargeValue->setObjectName("toolChargeValue");
    toolChargeValue->setDecimals(5);
    toolChargeValue->setRange(-10000, 10000);
    toolChargeValue->setToolTip("「電荷」タブの「電荷 / e」と共通です。");
    chargeLabel->setBuddy(toolChargeValue);
    optionGroup("chargeOption", {chargeLabel, toolChargeValue});
    // The structure commands stay at the right end, after the options of the tool.
    options->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));
    autoH = new QCheckBox("水素自動補完");
    autoH->setToolTip("原子の追加・結合の変更・元素の変更のあとに、空いた原子価へ水素を付けます。");
    autoH->setChecked(true);
    auto *h = new QPushButton("水素を補完");
    h->setToolTip("選択原子（未選択なら分子全体）の空いた原子価に水素を付けます。");
    connect(h, &QPushButton::clicked, this, [this] {
        safe([this] { edit("水素を補完", [this](Document &d) { completeHydrogens(d, selected); }); });
    });
    auto *refineButton = new QPushButton("全体整形");
    refineButton->setObjectName("refineButton");
    refineButton->setToolTip("力場（MMFF94、使えない場合UFF）で分子全体の構造を整え、候補として表示します。"
                             "金属錯体・ゴースト原子を含む分子には使えません。");
    connect(refineButton, &QPushButton::clicked, this, [this] { safe([this] { runWorker("refine"); }); });
    cancelWorkerButton = new QPushButton("整形を中止");
    cancelWorkerButton->hide();
    connect(cancelWorkerButton, &QPushButton::clicked, this, &MainWindow::cancelWorker);
    optionGroup("structureOption", {autoH, h, refineButton, cancelWorkerButton});
    topRow->addWidget(optionBar);
    banner = new QFrame;
    banner->setObjectName("candidateBanner");
    topRow->addWidget(banner);
    ml->addWidget(topRow);
    // The 3D view, with the sequence, the view controls and the measurement in its corners.
    view = new MoleculeView;
    auto *viewArea = new OverlayHost(view);
    viewArea->setObjectName("viewArea");
    sequenceChip = new QFrame;
    sequenceChip->setObjectName("sequenceChip");
    auto *sequenceRow = new QHBoxLayout(sequenceChip);
    sequenceRow->setContentsMargins(10, 4, 10, 4);
    sequenceLabel = new QLabel;
    sequenceLabel->setObjectName("sequenceLabel");
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
    sequenceRow->addWidget(sequenceLabel);
    viewArea->addOverlay(sequenceChip, Qt::AlignTop | Qt::AlignLeft);
    viewControls = new ViewControls(view, {{"viewAxisX", "viewAxisY", "viewAxisZ"},
                                           "viewFit",
                                           "rotationButton",
                                           "viewMenuButton",
                                           "rotationPopup",
                                           {"editorYaw", "editorPitch", "editorRoll"}});
    viewControls->setObjectName("viewControls");
    viewArea->addOverlay(viewControls, Qt::AlignTop | Qt::AlignRight);
    // Distance, angle or dihedral of 2–4 atoms selected in order, and the value to set it to.
    measureCard = new QFrame;
    measureCard->setObjectName("measureCard");
    auto *measureLayout = new QVBoxLayout(measureCard);
    measureLayout->setContentsMargins(12, 9, 12, 10);
    measureLayout->setSpacing(5);
    measureTitle = new QLabel;
    measureTitle->setObjectName("measureTitle");
    auto boldFont = measureTitle->font();
    boldFont.setBold(true);
    measureTitle->setFont(boldFont);
    measureLayout->addWidget(measureTitle);
    measurement = new QLabel;
    measurement->setObjectName("measurementLabel");
    measurement->setWordWrap(true);
    measureLayout->addWidget(measurement);
    auto *dimensions = new QHBoxLayout;
    dimensions->setSpacing(6);
    dimensionLabel = new QLabel;
    dimension = new QDoubleSpinBox;
    dimension->setDecimals(5);
    dimension->setRange(-10000, 10000);
    dimension->setObjectName("dimensionValue");
    dimension->setSpecialValueText("—");
    dimensionLabel->setBuddy(dimension);
    dimensions->addWidget(dimensionLabel);
    dimensions->addWidget(dimension, 1);
    auto *applyDimension = new QPushButton("寸法を適用");
    applyDimension->setObjectName("applyDimension");
    applyDimension->setToolTip(
        "選択順の2〜4原子の距離・角度・二面角を指定値にします。後に選んだ原子の側が動きます。");
    dimensions->addWidget(applyDimension);
    enableWithSelection(dimension, 2, 4);
    enableWithSelection(applyDimension, 2, 4);
    measureLayout->addLayout(dimensions);
    connect(applyDimension, &QPushButton::clicked, this, [this] {
        safe([this] {
            auto ids = requireSelection();
            double v = dimension->value();
            edit("寸法を適用", [=](Document &d) { setDimension(d, ids, v); });
        });
    });
    measureCard->hide();
    viewArea->addOverlay(measureCard, Qt::AlignBottom | Qt::AlignLeft);
    ml->addWidget(viewArea, 1);
    // Under the view: what a click does with the tool, and the mouse controls of the view.
    auto *hintBar = new QWidget;
    hintBar->setObjectName("hintBar");
    hintBar->setAttribute(Qt::WA_StyledBackground);
    auto *hints = new QVBoxLayout(hintBar);
    hints->setContentsMargins(12, 4, 12, 5);
    hints->setSpacing(1);
    // One line each (shortened when narrow, in full as a tooltip), so the view never moves.
    mouseHint = new ElidedLabel;
    mouseHint->setObjectName("toolHint");
    hints->addWidget(mouseHint);
    auto *controlsHint = new ElidedLabel(viewControlsHint() + " · Esc: 候補・選択を解除");
    controlsHint->setObjectName("controlsHint");
    controlsHint->setToolTip(controlsHint->text());
    auto hintFont = controlsHint->font();
    hintFont.setPointSizeF(hintFont.pointSizeF() * .9);
    controlsHint->setFont(hintFont);
    hints->addWidget(controlsHint);
    ml->addWidget(hintBar);
    splitter->addWidget(middle);
    // The properties: one page at a time, chosen with segments (their tab bar stays hidden).
    auto *propertyColumn = new QWidget;
    propertyColumn->setObjectName("propertyColumn");
    propertyColumn->setAttribute(Qt::WA_StyledBackground);
    propertyColumn->setMinimumWidth(300);
    propertyColumn->setMaximumWidth(440);
    auto *propertyLayout = new QVBoxLayout(propertyColumn);
    propertyLayout->setContentsMargins(0, 0, 0, 0);
    propertyLayout->setSpacing(0);
    auto *pageBar = new QWidget;
    pageBar->setObjectName("propertyPageBar");
    pageBar->setAttribute(Qt::WA_StyledBackground);
    auto *pageBarLayout = new QHBoxLayout(pageBar);
    pageBarLayout->setContentsMargins(10, 8, 10, 8);
    auto *pages = new QFrame;
    pages->setObjectName("propertyPages");
    pages->setProperty("choice", "segments");
    auto *pageRow = new QHBoxLayout(pages);
    pageRow->setContentsMargins(2, 2, 2, 2);
    pageRow->setSpacing(2);
    pageBarLayout->addWidget(pages, 1);
    propertyLayout->addWidget(pageBar);
    properties = new QTabWidget;
    properties->setObjectName("propertyTabs");
    properties->setDocumentMode(true);
    properties->tabBar()->hide();
    propertyLayout->addWidget(properties, 1);
    splitter->addWidget(propertyColumn);
    buildEditPanel();
    toolBondKind->setModel(bondKind->model());
    connect(toolBondKind, &QComboBox::currentIndexChanged, bondKind, &QComboBox::setCurrentIndex);
    connect(bondKind, &QComboBox::currentIndexChanged, this, [this](int index) {
        toolBondKind->setCurrentIndex(index);
        refreshToolHint();
    });
    connect(toolDeleteTerminate, &QCheckBox::toggled, deleteTerminate, &QCheckBox::setChecked);
    connect(deleteTerminate, &QCheckBox::toggled, toolDeleteTerminate, &QCheckBox::setChecked);
    buildCoordinationPanel();
    buildResiduePanel();
    buildChargePanel();
    propertyGroup = new QButtonGroup(this);
    for (int i = 0; i < properties->count(); ++i) {
        auto *b = new QToolButton;
        b->setObjectName(QString("propertyPage_%1").arg(i));
        b->setText(properties->tabText(i));
        b->setCheckable(true);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        pageRow->addWidget(b);
        propertyGroup->addButton(b, i);
    }
    propertyGroup->button(0)->setChecked(true);
    connect(propertyGroup, &QButtonGroup::idClicked, properties, &QTabWidget::setCurrentIndex);
    connect(properties, &QTabWidget::currentChanged, this, [this](int index) {
        if (auto *b = propertyGroup->button(index))
            b->setChecked(true);
    });
    toolChargeValue->setValue(chargeValue->value());
    connect(toolChargeValue, &QDoubleSpinBox::valueChanged, chargeValue, &QDoubleSpinBox::setValue);
    connect(chargeValue, &QDoubleSpinBox::valueChanged, toolChargeValue, &QDoubleSpinBox::setValue);
    splitter->setSizes({280, 760, 330});
    splitter->setStretchFactor(1, 1);
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
        auto *page = workspaceTabs->widget(index);
        if (page == inputPreview) {
            execution->commitPendingEdits();
            refreshPanel(inputPreview, true);
        } else
            refreshPanel(page);
    });
    buildHeader();
    outer->insertWidget(0, header);
    // Candidate controls: a banner in place of the tool options of "1 分子", and a bar below the
    // tabs for the other tabs. Amber backgrounds come from the chrome style for either theme.
    const auto candidateControls = [this](QWidget *box, QLabel *title, QLabel *&text,
                                          const QString &confirmName, const QString &cancelName) {
        // What the candidate changes, and the buttons beside it or, when narrow, below it.
        auto *flow = new FlowLayout(box, 12, 4);
        flow->setContentsMargins(12, 4, 12, 4);
        auto *about = new QWidget;
        about->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto *row = new QHBoxLayout(about);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(10);
        flow->addWidget(about);
        auto *icon = new QLabel;
        icon->setObjectName("candidateIcon");
        row->addWidget(icon);
        auto *texts = new QVBoxLayout;
        texts->setSpacing(1);
        if (title) {
            auto font = title->font();
            font.setBold(true);
            title->setFont(font);
            title->setWordWrap(true);
            texts->addWidget(title);
        }
        text = new QLabel;
        text->setWordWrap(true);
        text->setTextFormat(Qt::RichText);
        texts->addWidget(text);
        row->addLayout(texts, 1);
        auto *buttons = new QWidget;
        auto *buttonRow = new QHBoxLayout(buttons);
        buttonRow->setContentsMargins(0, 0, 0, 0);
        buttonRow->setSpacing(8);
        flow->addWidget(buttons);
        auto *cancel = new QPushButton("候補を取消（Esc）");
        cancel->setObjectName(cancelName);
        buttonRow->addWidget(cancel);
        auto *confirm = new QPushButton("変更を確定（Ctrl+Enter）");
        confirm->setObjectName(confirmName);
        confirm->setProperty("primary", true);
        buttonRow->addWidget(confirm);
        connect(confirm, &QPushButton::clicked, this, [this] { safe([this] { confirmCandidate(); }); });
        connect(cancel, &QPushButton::clicked, this, &MainWindow::cancelCandidate);
    };
    candidateBar = new QFrame;
    candidateBar->setObjectName("candidateBar");
    candidateControls(candidateBar, nullptr, candidateLabel, "candidateBarConfirm", "candidateBarCancel");
    outer->addWidget(candidateBar);
    candidateTitle = new QLabel;
    candidateTitle->setObjectName("candidateBannerTitle");
    candidateControls(banner, candidateTitle, bannerLabel, "confirmCandidate", "cancelCandidate");
    bannerLabel->setObjectName("candidateBannerText");
    connect(workspaceTabs, &QTabWidget::currentChanged, this, [this] {
        candidateBar->setVisible(candidate.has_value() && workspaceTabs->currentWidget() != moleculePage);
    });
    // The document in brief at the left of the status bar; a message takes its place while shown.
    summary = new QLabel;
    summary->setObjectName("documentSummary");
    statusBar()->addWidget(summary, 1);
    connect(workspaceTabs, &QTabWidget::currentChanged, this, [this] {
        if (!refreshing)
            refreshSummary();
    });
    // View toggles: in the "表示" menu and the "表示" button over the 3D view.
    showHydrogens = new QAction("水素を表示", this);
    showHydrogens->setObjectName("showHydrogensAction");
    showHydrogens->setToolTip("水素原子の表示を切り替えます（H）。隠しても計算・保存には含みます。");
    showHydrogens->setCheckable(true);
    showHydrogens->setChecked(true);
    showIndices = new QAction("番号を表示", this);
    showIndices->setObjectName("showIndices");
    showIndices->setToolTip("ラベルに原子の通し番号（座標表・NWChem入力の順番）を付けます。");
    showIndices->setCheckable(true);
    showIndices->setChecked(true);
    labelModes = new QActionGroup(this);
    const auto labels = settings.value("view/labels", "all").toString();
    for (const auto &[label, mode] : {std::pair{"すべて", "all"},
                                      {"水素以外", "heavy"},
                                      {"選択した原子", "selection"},
                                      {"なし", "none"}}) {
        auto *a = labelModes->addAction(label);
        a->setObjectName(QString("labelMode_") + mode);
        a->setCheckable(true);
        a->setData(mode);
        a->setChecked(labels == mode);
    }
    if (!labelModes->checkedAction())
        labelModes->actions().first()->setChecked(true);
    view->setLabelMode(labelModes->checkedAction()->data().toString());
    connect(toolGroup, &QButtonGroup::buttonToggled, this, [this](QAbstractButton *, bool on) {
        if (!on)
            return;
        view->setTool(currentTool());
        // "選択" does not place the palette part: it is no longer shown as chosen. The tools that use
        // it show again the one chosen last.
        if (currentTool() == "select")
            palette->clearSelection();
        else if ((currentTool() == "place" || currentTool() == "replace") && palette->currentItem())
            palette->currentItem()->setSelected(true);
        refreshToolHint();
    });
    connect(elementBox, &QComboBox::currentIndexChanged, this, &MainWindow::refreshToolHint);
    connect(palette, &QListWidget::currentItemChanged, this, &MainWindow::refreshToolHint);
    connect(unitGroup, &QButtonGroup::buttonToggled, this, [this](QAbstractButton *, bool on) {
        if (on)
            safe([this] {
                setSelection(expandedSelection(selected));
                refreshToolHint();
            });
    });
    connect(showHydrogens, &QAction::toggled, view, &MoleculeView::setShowHydrogens);
    connect(showIndices, &QAction::toggled, view, &MoleculeView::setShowIndices);
    connect(labelModes, &QActionGroup::triggered, this, [this](QAction *a) {
        view->setLabelMode(a->data().toString());
        settings.setValue("view/labels", a->data().toString());
    });
    connect(search, &QLineEdit::textChanged, this, &MainWindow::refreshPalette);
    connect(categoryGroup, &QButtonGroup::buttonToggled, this, [this](QAbstractButton *, bool on) {
        if (on)
            refreshPalette();
    });
    connect(palette, &QListWidget::itemClicked, this, [this] { safe([this] { paletteChosen(); }); });
    // Enter in the search box takes the first match, like a click on it.
    connect(search, &QLineEdit::returnPressed, this, [this] {
        safe([this] {
            selectFirstPart();
            if (palette->currentItem())
                paletteChosen();
        });
    });
    connect(view, &MoleculeView::atomClicked, this,
            [this](auto id, auto mods) { safe([this, id, mods] { atomClicked(id, mods); }); });
    connect(view, &MoleculeView::blankClicked, this, [this](Vec p) {
        safe([this, p] {
            if (currentTool() == "select")
                setSelection({});
            else
                place({}, p);
        });
    });
    connect(view, &MoleculeView::bondClicked, this, [this](auto a, auto b) {
        safe([this, a, b] {
            if (currentTool() == "place" && placementKind() == "atom") {
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
            if (currentTool() == "place" && placementKind() == "ligand") {
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
                if (indices.size() == t["connections"].toArray().size()) {
                    // The next site continues the placement: the ligand placed before is confirmed.
                    const bool chained = candidate && candidateName == "配位子を配置" && !candidateRemote;
                    if (chained)
                        confirmCandidate();
                    edit("配位子を配置", [=](Document &d) { placeLigand(d, c, indices, t); }, true);
                    if (chained)
                        statusBar()->showMessage("前の配位子を確定し、次の配位子を候補にしました。", 8000);
                }
            } else
                siteInput->setText(QString::number(site + 1));
        });
    });
    connect(view, &MoleculeView::terminusClicked, this,
            [this](auto rid, bool n) { safe([this, rid, n] { addAmino(rid, n); }); });
    // The move tool carries the selection (or the clicked unit) and the hydrogens only on it.
    const auto moving = [this](const QString &id) {
        auto ids = selected.contains(id) ? selected : expandedSelection({id});
        const auto &d = displayDocument();
        for (const auto &atom : Ids(ids))
            for (const auto &n : d.neighbors(atom))
                if (!ids.contains(n) && d.atom(n).element == "H" && d.neighbors(n).size() == 1)
                    ids << n;
        return ids;
    };
    connect(view, &MoleculeView::dragStarted, this,
            [this, moving](const QString &id) { safe([&] { view->setDragAtoms(moving(id)); }); });
    connect(view, &MoleculeView::atomDragged, this, [this, moving](auto id, Vec delta) {
        safe([this, id, delta, moving] {
            const auto ids = moving(id);
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
                showHydrogens->toggle();
            else if (key == Qt::Key_Delete) {
                if (hasSelection())
                    deleteAtoms(selected);
            } else if (key >= Qt::Key_1 && key <= Qt::Key_3) {
                if (!hasSelection(2))
                    return;
                auto ids = selected;
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
                    const auto select = [&] {
                        for (int i = 0; i < palette->count(); ++i)
                            if (palette->item(i)->data(Qt::UserRole) == key) {
                                palette->setCurrentRow(i);
                                return true;
                            }
                        return false;
                    };
                    // A part the filtered palette does not list (an MCP drop): show the whole palette;
                    // an unknown part is an error, never the part chosen before.
                    if (!select()) {
                        if (key.startsWith("atom:"))
                            paletteElement = element(key.mid(5)).symbol;
                        search->clear();
                        setPaletteCategory("all");
                        refreshPalette();
                        if (!select())
                            fail("部品が見つかりません: " + key);
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
    refreshToolHint();
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
    documentButton->setToolTip("新規・開く・保存・SMILESから作成");
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
    undoButton = history("元に戻す", "undo", "editorUndo");
    connect(undoButton, &QPushButton::clicked, this, [this] {
        safe([this] {
            if (candidate)
                cancelCandidate();
            else
                undo.undo();
        });
    });
    redoButton = history("やり直す", "redo", "editorRedo");
    connect(redoButton, &QPushButton::clicked, this, [this] {
        safe([this] {
            if (!candidate)
                undo.redo();
        });
    });
    runPill = new QLabel;
    runPill->setObjectName("runStatePill");
    runPill->hide();
    right->addSpacing(4);
    right->addWidget(runPill);
    headerStop = new QToolButton;
    headerStop->setObjectName("headerStop");
    headerStop->setText("停止");
    headerStop->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    headerStop->setToolTip("実行中のNWChemを停止します（確認してから停止）。");
    headerStop->hide();
    right->addWidget(headerStop);
    connect(headerStop, &QToolButton::clicked, this, [this] { execution->confirmStop(); });
    runButton = new QToolButton;
    runButton->setObjectName("runButton");
    runButton->setText("計算を実行");
    runButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    runButton->setToolTip("入力一式を新しいジョブフォルダーに保存し、NWChemを起動します（F5）。");
    right->addSpacing(4);
    right->addWidget(runButton);
    header = new HeaderBar(identity, workflowSteps, actions);
    header->setObjectName("headerBar");
    header->setAttribute(Qt::WA_StyledBackground);
    syncWorkflowSteps();
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
    const QList<QAction *> documentActions{action(file, "新規", QKeySequence::New, [this] { newDocument(); }),
                                           action(file, "開く…", QKeySequence::Open, [this] { open(); }),
                                           action(file, "SMILESから作成…", {}, [this] { importSmiles(); })};
    for (int i = 0; i < documentActions.size(); ++i)
        documentActions[i]->setObjectName(QStringList{"newAction", "openAction", "smilesAction"}[i]);
    recentMenu = file->addMenu("最近使ったファイル");
    recentMenu->setObjectName("recentMenu");
    refreshRecent();
    action(file, "SMILESから断片を追加…", {}, [this] { addFragmentFromSmiles(); });
    action(file, "ファイルから断片を追加…", {}, [this] { addFragmentFromFile(); });
    file->addSeparator();
    auto *saveAction = action(file, "プロジェクトを保存", QKeySequence::Save, [this] { save(); });
    auto *saveAsAction = action(file, "名前を付けて保存…", QKeySequence::SaveAs, [this] { save(true); });
    action(file, "構造を書き出す…", {}, [this] { exportFile(); });
    action(file, "分子全体をXYZでコピー", {}, [this] {
        if (resolveCandidate())
            QApplication::clipboard()->setText(doc.xyz());
    });
    action(file, "XYZを貼り付けて置き換え…", {}, [this] {
        bool ok;
        QString text = QInputDialog::getMultiLineText(
            this, "XYZを貼り付けて置き換え",
            "原子数、コメント、元素と座標（Å）。今の構造を置き換える候補を作ります。",
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
    undoAction = action(editMenu, "元に戻す", QKeySequence::Undo, [this] {
        if (candidate)
            cancelCandidate();
        else
            undo.undo();
    });
    undoAction->setObjectName("undoAction");
    auto *redo = redoAction = action(editMenu, "やり直す", QKeySequence::Redo, [this] {
        if (!candidate)
            undo.redo();
    });
    redo->setObjectName("redoAction");
    redo->setShortcuts({QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")});
    // Candidate keys work in every tab; editing keys only while the focus is in "1 分子", so text
    // fields keep their own keys and Delete in other tables never deletes atoms.
    confirmAction = action(editMenu, "変更を確定", QKeySequence("Ctrl+Return"), [this] {
        if (!candidate)
            fail("確定する変更候補がありません。");
        confirmCandidate();
    });
    confirmAction->setObjectName("confirmAction");
    // Listed as Ctrl+Enter like the candidate buttons; the main Return key works the same.
    confirmAction->setShortcuts({QKeySequence("Ctrl+Enter"), QKeySequence("Ctrl+Return")});
    action(editMenu, "候補を取消・選択を解除", QKeySequence(Qt::Key_Escape), [this] {
        if (candidate)
            cancelCandidate();
        else
            setSelection({});
        growth->setChecked(false);
        growthResidue.clear();
    })->setObjectName("escapeAction");
    editMenu->addSeparator();
    const auto local = [&](QMenu *m, const QString &text, const QKeySequence &key, auto fn,
                           const QString &name) {
        auto *a = action(m, text, key, fn);
        a->setObjectName(name);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        moleculePage->addAction(a);
        return a;
    };
    local(
        editMenu, "選択原子を削除", QKeySequence::Delete,
        [this] {
            if (hasSelection())
                deleteAtoms(selected);
        },
        "deleteAction");
    local(editMenu, "すべて選択", QKeySequence::SelectAll, [this] { selectAll(); }, "selectAllAction");
    local(editMenu, "選択をXYZでコピー", QKeySequence::Copy, [this] { copySelection(); }, "copyAction");
    local(
        editMenu, "XYZを断片として貼り付け", QKeySequence::Paste, [this] { pasteFragment(); }, "pasteAction");
    auto *orders = editMenu->addMenu("選択した2原子の結合次数");
    for (int n = 1; n <= 3; ++n)
        local(
            orders, QStringList{"単結合", "二重結合", "三重結合"}[n - 1], QKeySequence(Qt::Key_0 + n),
            [this, n] {
                if (!hasSelection(2))
                    return;
                auto ids = selected;
                const QString kind = QStringList{"single", "double", "triple"}[n - 1];
                edit("結合次数を変更", [=, this](Document &d) {
                    d.setBond(ids[0], ids[1], kind);
                    if (autoH->isChecked())
                        completeHydrogens(d, ids);
                });
            },
            QString("bondOrder%1Action").arg(n));
    // One key per editing tool while "1 分子" has the focus (text fields keep their letters).
    auto *editingTools = editMenu->addMenu("ツールを選ぶ");
    for (auto *b : toolGroup->buttons()) {
        const auto code = b->property("tool").toString();
        const auto key = b->property("toolKey").toString();
        local(
            editingTools, b->text(), QKeySequence(key), [this, code] { setTool(code); },
            "toolAction_" + code);
        b->setToolTip(b->toolTip() + QString("（%1）").arg(key));
    }
    editMenu->addSeparator();
    action(editMenu, "番号で原子を選択…", {}, [this] { selectNumbers(); });
    auto *attributes =
        action(editMenu, "座標と原子属性の一覧", {}, [this] { coordinates->showAttributes(); });
    attributes->setObjectName("coordinateAttributesAction");
    attributes->setToolTip("「2 座標」に形式電荷・不対電子・原子名の列を表示します。");
    editMenu->addSeparator();
    // Preferences hold the executable, launcher and environment: only the user may change them.
    auto *preferencesAction =
        action(editMenu, "環境設定…", QKeySequence::Preferences, [this] { preferences(); });
    preferencesAction->setObjectName("preferencesAction");
    preferencesAction->setProperty("mcpProtected", true);
    // Checks and transformations of the whole structure.
    auto *toolsMenu = menuBar()->addMenu("ツール");
    action(toolsMenu, "構造を検査…", {}, [this] { showReport(); })->setObjectName("reportAction");
    action(toolsMenu, "点群・対称化…", {}, [this] { symmetryDialog(); })->setObjectName("symmetryAction");
    action(toolsMenu, "Z-matrix（内部座標）…", {}, [this] {
        zMatrixDialog();
    })->setObjectName("zMatrixAction");
    action(toolsMenu, "配座探索…", {}, [this] {
        bool ok;
        const int count =
            QInputDialog::getInt(this, "配座探索",
                                 "回転可能な結合をランダムに回して力場で最適化し、エネルギーの低い"
                                 "配座を探します（MMFF94、使えない場合UFF）。\n残す配座の数",
                                 10, 1, 100, 1, &ok);
        if (ok)
            runWorker("conformers", {}, {}, count);
    })->setObjectName("conformerAction");
    action(toolsMenu, "構造を比較・重ね合わせ…", {}, [this] {
        compareDialog();
    })->setObjectName("compareAction");
    // NWChem runs from any tab, not only from the button in "5 実行設定".
    auto *calculationMenu = menuBar()->addMenu("計算");
    auto *runAction = action(calculationMenu, "計算を実行", QKeySequence(Qt::Key_F5),
                             [this] { execution->startFromMenu(); });
    runAction->setObjectName("runCalculationAction");
    runAction->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    stopAction = action(calculationMenu, "計算を停止…", {}, [this] { execution->confirmStop(); });
    stopAction->setObjectName("stopCalculationAction");
    stopAction->setEnabled(execution->running()); // ExecutionPanel follows the run.
    action(calculationMenu, "ジョブとして保存", {}, [this] {
        workspaceTabs->setCurrentWidget(execution);
        execution->saveBundle();
    })->setObjectName("saveBundleAction");
    calculationMenu->addSeparator();
    action(calculationMenu, "結果ファイルを開く…", {}, [this] {
        workspaceTabs->setCurrentWidget(results);
        if (auto *open = results->findChild<QAbstractButton *>("resultsOpen"))
            open->click();
    });
    action(calculationMenu, "計算の履歴", {},
           [this] { workspaceTabs->setCurrentWidget(calculationHistory); });
    auto *viewMenu = menuBar()->addMenu("表示");
    // H toggles the hydrogens while "1 分子" has the focus, like the other editing keys.
    showHydrogens->setShortcut(QKeySequence(Qt::Key_H));
    showHydrogens->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    moleculePage->addAction(showHydrogens);
    viewMenu->addAction(showHydrogens);
    viewMenu->addAction(showIndices);
    auto *labelMenu = viewMenu->addMenu("ラベル");
    labelMenu->setToolTipsVisible(true);
    labelMenu->menuAction()->setToolTip(
        "元素記号・番号を表示する原子です。ラベルはほかのラベル・原子に重ならない向きに置き、置けないものは"
        "選択した原子・配位中心・水素以外・手前の原子を優先して省きます。");
    labelMenu->addActions(labelModes->actions());
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
    local(viewMenu, "全体を表示", QKeySequence(Qt::Key_F), [this] { view->fit(); }, "fitAction");
    auto *tabsMenu = viewMenu->addMenu("タブを表示");
    for (int i = 0; i < workspaceTabs->count(); ++i)
        action(tabsMenu, workspaceTabs->tabText(i), QKeySequence(QString("Ctrl+%1").arg(i + 1)), [this, i] {
            workspaceTabs->setCurrentIndex(i);
        })->setObjectName(QString("showTab%1Action").arg(i + 1));
    viewMenu->addSeparator();
    action(viewMenu, "画面をPNGで書き出す…", {}, [this] {
        QString p = QFileDialog::getSaveFileName(this, "画面を保存", "molforma.png", "PNG (*.png)");
        if (!p.isEmpty() && !grab().save(p))
            fail("画像を書き出せません。");
    });
    auto *help = menuBar()->addMenu("ヘルプ");
    action(help, "操作ガイド・マニュアル", QKeySequence::HelpContents, [this] {
        showManual();
    })->setObjectName("manualAction");
    action(help, "キーボードショートカット一覧", {}, [this] {
        showShortcuts();
    })->setObjectName("shortcutsAction");
    action(help, "MolFormaについて", {}, [this] {
        QMessageBox::about(
            this, "MolForma",
            QString("MolForma %1\nNWChem向けの分子作成・計算入力・実行・結果表示（Qt6 / C++）\n"
                    "ライセンス: GNU GPL バージョン2以降\n"
                    "化学ファイル・力場: Open Babel\n立体判定・残基認識（RDKit）: %2\n3D描画: %3")
                .arg(MOLFORMA_VERSION, hasStereochemistryBackend() ? "有効" : "無効",
                     view->rendererName() == "vtk" ? "VTK（OpenGL）" : "QPainter（CPU）"));
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
    viewControls->menuButton()->setMenu(viewMenu);
    applyChromeStyle();
}
void MainWindow::showProperties(const QString &title) {
    for (int i = 0; i < properties->count(); ++i)
        if (properties->tabText(i) == title)
            properties->setCurrentIndex(i);
}
void MainWindow::refreshPalette() {
    QString cat = paletteCategory(), needle = search->text().trimmed();
    periodicTableButton->setVisible(cat == "all" || cat == "atom");
    // The element the atom tool places is marked among the common ones.
    for (auto *b : findChild<QWidget *>("quickElements")->findChildren<QToolButton *>())
        b->setChecked(b->text() == paletteElement);
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
        // Atoms are chosen below by symbol, name or number; the other parts by their text.
        if (!id.startsWith("atom:") && !id.startsWith("smiles:") && !needle.isEmpty() &&
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
    // "すべて" lists the groups in the order of building a molecule, each under a heading (not while
    // searching, where the matches are few).
    const bool headings = cat == "all" && needle.isEmpty();
    const auto heading = [&](const QString &text) {
        if (!headings)
            return;
        auto *item = new QListWidgetItem(text, palette);
        item->setFlags(Qt::NoItemFlags);
        item->setData(TemplateDelegate::HeadingRole, true);
    };
    if (cat == "all" || cat == "atom") {
        // Without a search: the chosen element ("原子" also lists the common ones). With one: the
        // element with that symbol or number first, then those whose name contains it.
        QStringList symbols;
        if (needle.isEmpty()) {
            symbols << paletteElement;
            if (cat == "atom")
                for (const auto *common : {"H", "C", "N", "O", "F", "P", "S", "Cl", "Br", "I"})
                    if (!symbols.contains(common))
                        symbols << common;
        } else {
            bool numeric = false;
            const int number = needle.toInt(&numeric);
            for (const auto &e : elements())
                if (numeric ? e.number == number : e.symbol.compare(needle, Qt::CaseInsensitive) == 0)
                    symbols << e.symbol;
            for (const auto &e : elements())
                if (!numeric && !symbols.contains(e.symbol) &&
                    (e.nameJa.contains(needle) ||
                     (needle.size() > 2 && e.nameEn.contains(needle, Qt::CaseInsensitive))))
                    symbols << e.symbol;
            symbols = symbols.mid(0, cat == "atom" ? 40 : 6);
        }
        heading("原子（元素はH〜Iのボタン・周期表で変更）");
        for (const auto &symbol : std::as_const(symbols)) {
            const auto &e = element(symbol);
            add(e.nameJa, "atom:" + e.symbol,
                QString("%1  %2  %3\n原子番号 %4").arg(e.symbol, e.nameJa, e.nameEn).arg(e.number));
        }
    }
    const auto addTemplates = [&](const std::function<bool(const QJsonObject &)> &belongs,
                                  const QString &title) {
        QJsonArray all = templates();
        for (auto t : customParts)
            all.append(t);
        bool first = true;
        for (auto v : all) {
            auto t = v.toObject();
            if (!belongs(t))
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
            if (first)
                heading(title);
            first = false;
            add(label, t["id"].toString(), text, t);
        }
    };
    const auto builtIn = [](const QJsonObject &t, const QStringList &categories) {
        return !t["id"].toString().startsWith("my:") && categories.contains(t["category"].toString());
    };
    for (const auto &[group, title] : {std::pair{"skeleton", "骨格"},
                                       {"substituent", "置換基"},
                                       {"ligand", "配位子"},
                                       {"amino", "アミノ酸・末端キャップ"}})
        if (cat == "all" || cat == group)
            addTemplates(
                [&, group = QString(group)](const QJsonObject &t) {
                    return builtIn(t, group == "amino" && cat == "all" ? QStringList{"amino", "cap"}
                                                                       : QStringList{group});
                },
                title);
    if (cat == "all" || cat == "center") {
        heading("配位中心（金属）");
        for (auto s :
             QStringList{"Li", "Na", "Mg", "Al", "K",  "Ca", "Ti", "V",  "Cr", "Mn", "Fe", "Co", "Ni",
                         "Cu", "Zn", "Ru", "Rh", "Pd", "Ag", "Cd", "Pt", "Au", "La", "Ce", "Gd"}) {
            const auto &e = element(s);
            add(e.nameJa, "center:" + s,
                QString("%1  %2  %3\n金属 配位中心 · 原子番号 %4")
                    .arg(e.symbol, e.nameJa, e.nameEn)
                    .arg(e.number));
        }
    }
    if (cat == "all" || cat == "my")
        addTemplates([](const QJsonObject &t) { return t["id"].toString().startsWith("my:"); }, "マイ部品");
    // A search that names no part may be a SMILES: offer to build it (a fragment beside the molecule).
    static const QRegularExpression smilesText(R"(^[A-Za-z0-9@+\-\[\]()=#$/\\%.:*]+$)");
    if (cat == "all" && !palette->count() && smilesText.match(needle).hasMatch())
        add(QString("SMILES「%1」から3Dで作成").arg(needle), "smiles:" + needle,
            "SMILESとして読み込み、今の分子の横に置く候補を作ります。");
    if (!palette->currentItem())
        selectFirstPart();
}
void MainWindow::selectFirstPart() {
    for (int i = 0; i < palette->count(); ++i)
        if (palette->item(i)->flags() & Qt::ItemIsSelectable) {
            palette->setCurrentRow(i);
            return;
        }
}
QString MainWindow::paletteCategory() const {
    return categoryGroup->checkedButton() ? categoryGroup->checkedButton()->property("category").toString()
                                          : QString("all");
}
void MainWindow::setPaletteCategory(const QString &code) {
    for (auto *b : categoryGroup->buttons())
        if (b->property("category") == code)
            b->setChecked(true);
}
QString MainWindow::selectionUnit() const {
    return unitGroup->checkedButton() ? unitGroup->checkedButton()->property("unit").toString()
                                      : QString("atom");
}
void MainWindow::chooseElement(const QString &symbol) {
    paletteElement = element(symbol).symbol;
    search->clear();
    refreshPalette();
    for (int i = 0; i < palette->count(); ++i)
        if (palette->item(i)->data(Qt::UserRole) == "atom:" + paletteElement)
            palette->setCurrentRow(i);
    paletteChosen();
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
    if (id.startsWith("smiles:")) {
        const auto smiles = id.mid(7);
        search->clear();
        runWorker(doc.atoms.isEmpty() ? "smiles" : "fragment", smiles);
        return;
    }
    if (id.startsWith("atom:")) {
        paletteElement = id.mid(5);
        elementBox->setCurrentIndex(elementBox->findData(id.mid(5)));
    } else if (id.startsWith("center:")) {
        elementBox->setCurrentIndex(elementBox->findData(id.mid(7)));
        showProperties("配位");
    } else if (currentTemplate()["category"] == "amino") {
        showProperties("残基");
        if (growth->isChecked() && !growthResidue.isEmpty() && !candidate)
            addAmino(growthResidue, growthN);
    }
    // The replace tool keeps working with whatever is chosen: it replaces atoms with that part.
    if (currentTool() != "replace")
        setTool("place");
    refreshToolHint();
    view->setAminoHandles(id.startsWith("amino:"));
}
bool MainWindow::hasSelection(int count) {
    if (!selected.isEmpty() && (!count || selected.size() == count))
        return true;
    statusBar()->showMessage(
        count ? QString("%1原子を選択してください。").arg(count) : QString("原子を選択してください。"), 5000);
    return false;
}
void MainWindow::enableWithSelection(QWidget *widget, int minimum, int maximum) {
    selectionControls.append({widget, minimum, maximum, widget->toolTip()});
}
void MainWindow::refreshSelectionControls() {
    const int count = selected.size();
    for (const auto &c : std::as_const(selectionControls)) {
        const bool ready = count >= c.minimum && (!c.maximum || count <= c.maximum);
        c.widget->setEnabled(ready);
        const QString need =
            c.minimum == c.maximum
                ? (c.minimum == 1 ? QString("原子を1つ選択すると使えます。")
                                  : QString("%1原子を順に選択すると使えます。").arg(c.minimum))
            : c.maximum ? QString("%1〜%2原子を順に選択すると使えます。").arg(c.minimum).arg(c.maximum)
                        : QString("原子を選択すると使えます。");
        c.widget->setToolTip(ready ? c.tooltip : c.tooltip.isEmpty() ? need : c.tooltip + "\n" + need);
    }
}
Ids MainWindow::requireSelection(int count) const {
    if (selected.isEmpty() || (count && selected.size() != count))
        fail(count ? QString("%1原子を選択してください。").arg(count) : "原子を選択してください。");
    return selected;
}
Ids MainWindow::expandedSelection(const Ids &ids) const {
    const auto &d = displayDocument();
    QString mode = selectionUnit();
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
    const auto present = displayDocument().ids();
    const QSet<QString> atoms(present.begin(), present.end());
    selected = ids;
    selected.removeIf([&](const auto &id) { return !atoms.contains(id); });
    view->setSelection(selected);
    refreshSelection();
}
void MainWindow::atomClicked(const QString &id, Qt::KeyboardModifiers mods) {
    QString mode = currentTool();
    if (mode == "select" || mode == "move") {
        auto group = expandedSelection({id});
        // A click selects; Shift/Ctrl+click adds or removes, keeping the order for measurements.
        if (!(mods & (Qt::ShiftModifier | Qt::ControlModifier))) {
            setSelection(group);
            return;
        }
        auto next = selected;
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
    QString mode = currentTool();
    if (mode == "place")
        mode = placementKind();
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
        deleteAtoms(expandedSelection({id}));
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
    } else if (mode == "smiles")
        fail("SMILESの項目は、パレットでクリックすると3D構造を作ります。");
    else if (mode == "ligand" && !id.isEmpty())
        fail("配位中心の紫の番号（空きサイト）をクリックしてください。空白をクリックすると単独で置きます。");
    // A ligand clicked on a blank is put down on its own, like a skeleton.
    else if (mode == "part" || mode == "substituent" || mode == "ligand") {
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
            setTool("select");
    } else
        return;
    view->keepInView();
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
    // Candidate changes: atoms removed (shown where they were), added and moved.
    const auto changes = candidate ? atomChanges(doc, *candidate) : AtomChanges{};
    const auto &addedAtoms = changes.added, &movedAtoms = changes.moved;
    QVector<std::pair<Vec, QString>> removedAtoms;
    for (const auto &a : changes.removed)
        removedAtoms.append({a.position, a.element});
    view->setChanges(removedAtoms, addedAtoms, movedAtoms);
    const auto counts = QString("追加 %1・削除 %2・移動 %3 原子")
                            .arg(addedAtoms.size())
                            .arg(removedAtoms.size())
                            .arg(movedAtoms.size());
    const auto marker = [](const char *color, const QString &mark) {
        return QString("<span style=\"color:%1\"><b>%2</b></span>").arg(color, mark);
    };
    // The counts with the marks the 3D view uses for them.
    candidateTitle->setText("変更候補: " + candidateName);
    bannerLabel->setText(QString("%1 追加 %2　%3 削除 %4　%5 移動 %6 原子")
                             .arg(marker("#25a35a", "○"))
                             .arg(addedAtoms.size())
                             .arg(marker("#d23c3c", "×"))
                             .arg(removedAtoms.size())
                             .arg(marker("#e08a1e", "○"))
                             .arg(movedAtoms.size()));
    candidateLabel->setText(QString("<b>変更候補: %1</b>　%2").arg(candidateName.toHtmlEscaped(), counts));
    for (auto *label : {candidateLabel, bannerLabel})
        label->setToolTip("3D画面の印: 緑の円は追加、赤の×は削除（元の位置）、橙の円は移動する原子です。");
    // The same changes listed atom by atom in the "編集" tab, beside the selection.
    candidateCard->setVisible(candidate.has_value());
    if (candidate) {
        QStringList lines;
        int listed = 0, total = 0;
        const auto line = [&](const char *color, const QString &mark, const QString &name,
                              const QString &text) {
            if (++total > 8)
                return;
            ++listed;
            lines << QString("%1&nbsp; <b>%2</b>&nbsp; %3")
                         .arg(marker(color, mark), name.toHtmlEscaped(), text.toHtmlEscaped());
        };
        for (const auto &a : changes.removed) {
            const auto place = atomPlace(doc, a.id);
            line("#d23c3c", "×", atomName(doc, a.id), place.isEmpty() ? "削除" : place + "　削除");
        }
        for (const auto &a : candidate->atoms)
            if (addedAtoms.contains(a.id)) {
                QString partner;
                for (const auto &n : candidate->neighbors(a.id))
                    if (doc.contains(n)) {
                        partner = atomName(doc, n);
                        break;
                    }
                line("#25a35a", "○", a.element, partner.isEmpty() ? "追加" : partner + " に結合　追加");
            }
        if (!movedAtoms.isEmpty())
            line("#e08a1e", "○", QString("%1 原子").arg(movedAtoms.size()), "移動");
        if (total > listed)
            lines << QString("ほか %1 件").arg(total - listed);
        candidateChanges->setText(lines.join("<br>"));
    }
    candidateBar->setVisible(candidate.has_value() && workspaceTabs->currentWidget() != moleculePage);
    topRow->setCurrentIndex(candidate ? 1 : 0);
    // Undo/Redo show the operation they act on, and are off while there is none.
    undoButton->setEnabled(candidate || undo.canUndo());
    redoButton->setEnabled(!candidate && undo.canRedo());
    undoButton->setToolTip(candidate        ? QString("候補を取消")
                           : undo.canUndo() ? "元に戻す: " + undo.undoText()
                                            : QString("元に戻す操作はありません"));
    redoButton->setToolTip(undo.canRedo() ? "やり直す: " + undo.redoText()
                                          : QString("やり直す操作はありません"));
    if (undoAction) {
        undoAction->setEnabled(candidate || undo.canUndo());
        redoAction->setEnabled(!candidate && undo.canRedo());
        confirmAction->setEnabled(candidate.has_value());
        undoAction->setText(candidate        ? QString("候補を取消")
                            : undo.canUndo() ? "元に戻す: " + undo.undoText()
                                             : QString("元に戻す"));
        redoAction->setText(undo.canRedo() ? "やり直す: " + undo.redoText() : QString("やり直す"));
    }
    refreshSummary();
    refreshTitle();
    QStringList seq, plain;
    residueList->clear();
    // Residue links colored in the markup, in the accent rather than the theme's link color.
    const auto link = ChromeColors(QMainWindow::palette()).accentText.name();
    for (auto v : d.data["residues"].toArray()) {
        auto r = v.toObject();
        QString label = r["code"].toString() + " " + QString::number(r["number"].toInt());
        seq << QString("<a href=\"%1\"><span style=\"color:%3\">%2</span></a>")
                   .arg(r["id"].toString(), label.toHtmlEscaped(), link);
        plain << label;
        auto *item = new QListWidgetItem(
            label + "  " + r["n_terminal"].toString() + " / " + r["c_terminal"].toString(), residueList);
        item->setData(Qt::UserRole, r["id"].toString());
    }
    // Long chains show their first residues; the "残基" tab lists them all.
    const int residues = seq.size();
    if (residues > 24) {
        seq = seq.mid(0, 24) << QString("…（全 %1 残基）").arg(residues);
        plain = plain.mid(0, 24) << seq.last();
    }
    sequenceLabel->setText(seq.isEmpty() ? QString() : QString("配列 N → C　") + seq.join(" · "));
    // One line when the view is wide enough, wrapping below the view controls otherwise.
    const auto margins = sequenceChip->layout()->contentsMargins();
    sequenceChip->setMaximumWidth(
        sequenceLabel->fontMetrics().horizontalAdvance("配列 N → C　" + plain.join(" · ")) + margins.left() +
        margins.right() + 8);
    sequenceLabel->setToolTip(seq.isEmpty() ? QString()
                                            : "残基をクリックすると、その残基の原子を選択します。");
    sequenceChip->setVisible(!seq.isEmpty());
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
    refreshPanels();
}
void MainWindow::refreshSummary() {
    const auto &d = displayDocument();
    // In the result viewer and the history the molecule is not the one shown: name it.
    const auto *page = workspaceTabs->currentWidget();
    summary->setText(QString("%1%2 · %3 原子 · %4 結合 · 電荷 %5 · 多重度 %6 · 点電荷 %7%8")
                         .arg(page == results || page == calculationHistory ? "編集中の分子: " : "")
                         .arg(d.atoms.isEmpty() ? QString("原子なし") : d.formula())
                         .arg(d.atoms.size())
                         .arg(d.bonds.size())
                         .arg(d.job()["charge"].toInt())
                         .arg(d.job()["multiplicity"].toInt(1))
                         .arg(d.data["point_charges"].toArray().size())
                         .arg(candidate ? " · 変更候補あり" : ""));
}
void MainWindow::refreshTitle() {
    const auto name = path.isEmpty() ? QString("新規分子") : QFileInfo(path).fileName();
    const auto mark = undo.isClean() && !candidate ? "" : " *";
    setWindowTitle(QString("%1%2 — MolForma").arg(name, mark));
    if (documentButton) {
        documentButton->setText(documentButton->fontMetrics().elidedText(name, Qt::ElideMiddle, 220) + mark);
        documentButton->setToolTip((path.isEmpty() ? name : QDir::toNativeSeparators(path)) +
                                   "\n新規・開く・保存・SMILESから作成");
    }
}
void MainWindow::showRun(bool active, const QString &text, bool stoppable) {
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
        b->setToolTip(QString("「%1」を表示します（Ctrl+%2）%3")
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
        QLabel#runStatePill { background: @tint; color: @accentText; border-radius: 12px; padding: 4px 11px;
                              font-weight: bold; }
        QToolButton#headerStop { border: 1px solid @error; color: @error; font-weight: bold; padding: 5px 12px; }
        QToolButton#headerStop:hover { background: @badTint; }
    )");
    runButton->setIcon(lineIcon("play", c.onAccent));
    headerStop->setIcon(lineIcon("stop", c.bad));
    style(findChild<QWidget *>("toolRail"), R"(
        #toolRail { background: @base; border-right: 1px solid @line; }
        QToolButton { border: none; border-radius: 8px; color: @text; padding-top: 3px; }
        QToolButton:hover { background: @hover; }
        QToolButton:checked { background: @tint; color: @accentText; font-weight: bold; }
    )");
    style(findChild<QWidget *>("paletteColumn"), R"(
        #paletteColumn { background: @base; }
        #paletteCategories QToolButton { border: 1px solid @line; border-radius: 11px; padding: 2px 7px;
                                         background: @base; color: @text; }
        #paletteCategories QToolButton:hover { background: @hover; }
        #paletteCategories QToolButton:checked { background: @accent; border-color: @accent; color: @onAccent; }
        #quickElements QToolButton, #quickElements QPushButton { border: 1px solid @line; border-radius: 5px;
                                         background: @base; color: @text; min-height: 24px; padding: 1px 2px; }
        #quickElements QToolButton:hover, #quickElements QPushButton:hover { background: @hover; }
        #quickElements QToolButton:checked { background: @accent; border-color: @accent; color: @onAccent;
                                             font-weight: bold; }
        QListWidget#templatePalette { background: @base; border: none; }
    )");
    style(findChild<QWidget *>("toolOptions"), R"(
        #toolOptions { background: @base; border-bottom: 1px solid @line; }
        QLabel { color: @muted; }
        QLabel#toolPart { color: @text; }
        #selectionUnit { background: @segment; border-radius: 7px; }
        #selectionUnit QToolButton { border: none; border-radius: 5px; padding: 3px 7px; color: @muted;
                                     background: transparent; }
        #selectionUnit QToolButton:hover { color: @text; }
        #selectionUnit QToolButton:checked { background: @base; color: @text; font-weight: bold; }
    )");
    const QString candidateRules = R"(
        #candidateBanner, #candidateBar { background: @candidate; border: none; }
        #candidateBanner { border-bottom: 1px solid @candidateLine; }
        #candidateBar { border-top: 1px solid @candidateLine; }
        QLabel { color: @candidateText; }
        QPushButton { background: @base; color: @candidateText; border: 1px solid @candidateLine;
                      border-radius: 6px; padding: 5px 12px; }
        QPushButton:hover { background: @hover; }
        QPushButton[primary="true"] { background: @accent; color: @onAccent; border: none; font-weight: bold; }
        QPushButton[primary="true"]:hover { background: @accentHover; }
    )";
    style(banner, candidateRules);
    style(candidateBar, candidateRules);
    for (auto *icon : findChildren<QLabel *>("candidateIcon"))
        icon->setPixmap(lineIcon("layers", c.candidateText).pixmap(QSize(22, 22)));
    const QString overlay = R"(
        #sequenceChip, #measureCard { background: @base; border: 1px solid @line; border-radius: 8px; }
        QToolButton { border: none; border-radius: 5px; padding: 3px 7px; color: @text; }
        QToolButton:hover { background: @hover; }
        QLabel#measurementLabel { color: @text; }
    )";
    for (const auto *name : {"sequenceChip", "measureCard"})
        style(findChild<QWidget *>(name), overlay);
    viewControls->applyStyle(QMainWindow::palette());
    style(findChild<QWidget *>("hintBar"), R"(
        #hintBar { background: @base; border-top: 1px solid @line; }
        QLabel { color: @muted; }
        QLabel#toolHint { color: @text; }
    )");
    // The pages of properties: the cards and buttons of the settings tabs.
    style(findChild<QWidget *>("propertyColumn"), settingsRules() + R"(
        #propertyColumn { background: @window; border-left: 1px solid @line; }
        #propertyPageBar { background: @base; border-bottom: 1px solid @line; }
        QFrame#propertyPages QToolButton { padding: 4px 4px; }
        QTabWidget#propertyTabs::pane { border: none; }
        QLabel#moleculeChargeState, QLabel#moleculePointChargeState { color: @text; }
    )");
    style(candidateCard, R"(
        #candidateCard { background: @candidate; border: 1px solid @candidateLine; border-radius: 8px; }
        QLabel { color: @candidateText; }
    )");
    style(statusBar(), R"(
        QStatusBar { background: @base; border-top: 1px solid @line; color: @text; }
        QStatusBar::item { border: none; }
        QLabel#documentSummary { color: @muted; padding: 2px 6px; }
    )");
    if (coordinates)
        coordinates->applyChromeStyle();
    if (calculation)
        calculation->applyChromeStyle();
    if (advanced)
        advanced->applyChromeStyle();
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
    return {coordinates, calculation, advanced, execution, inputPreview};
}
void MainWindow::refreshPanels() {
    for (auto *panel : workspacePanels()) {
        if (!panel)
            continue;
        // Hidden tabs of a shown window catch up when opened (or before MCP reads the GUI).
        if (isVisible() && workspaceTabs->currentWidget() != panel) {
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
    else if (panel == advanced)
        advanced->refresh();
    else if (panel == execution)
        execution->refresh();
    else if (panel == inputPreview)
        inputPreview->refresh();
}
void MainWindow::refreshStalePanels() {
    for (auto *panel : workspacePanels())
        refreshPanel(panel);
}
void MainWindow::refreshSelection() {
    const auto &d = displayDocument();
    QHash<QString, int> index;
    for (int i = 0; !selected.isEmpty() && i < d.atoms.size(); ++i)
        index.insert(d.atoms[i].id, i);
    QStringList labels;
    for (const auto &id : selected)
        if (const int i = index.value(id, -1); i >= 0)
            labels << d.atoms[i].element + QString::number(i + 1);
    // 2–4 atoms in order: their distance, angle or dihedral in the card over the 3D view. Atoms
    // that a candidate removes are left out until it is confirmed or cancelled.
    Ids shown;
    for (const auto &id : selected)
        if (index.contains(id))
            shown << id;
    const int count = shown.size();
    const bool measuring = count >= 2 && count <= 4 && count == selected.size();
    QString value;
    if (measuring) {
        try {
            double m = measure(d, selected);
            value = QString("%1 %2").arg(m, 0, 'f', 4).arg(count == 2 ? "Å" : "°");
            dimension->setValue(m);
        } catch (const std::exception &e) {
            value = QString::fromUtf8(e.what());
        }
        measureTitle->setText(QStringList{"距離", "角度", "二面角"}[count - 2]);
        dimensionLabel->setText(QStringList{"距離 / Å", "角度 / °", "二面角 / °"}[count - 2]);
        measurement->setText(labels.join(" – ") + " = " + value);
    }
    measureCard->setVisible(measuring);
    // The selection in click order at the top of the "編集" tab.
    selectionTitle->setText(shown.isEmpty() ? QString("選択なし") : QString("選択 %1 原子").arg(count));
    if (shown.isEmpty())
        selectionList->setText("3D画面で原子をクリックして選択します。Shift／Ctrl＋クリックで順に追加し、"
                               "2〜4原子で距離・角度・二面角を測定します。");
    else {
        QStringList rows;
        for (int n = 0; n < std::min<int>(count, 8); ++n) {
            const auto place = atomPlace(d, shown[n]);
            rows << QString("<b>%1</b>&nbsp; %2%3")
                        .arg(n + 1)
                        .arg(atomName(d, shown[n]))
                        .arg(place.isEmpty() ? QString() : "&nbsp;&nbsp;" + place.toHtmlEscaped());
        }
        if (count > 8)
            rows << QString("ほか %1 原子").arg(count - 8);
        selectionList->setText(rows.join("<br>"));
    }
    // Fields that show the selection are blank ("—") without one, never left from an earlier atom.
    if (selected.size() < 2 || selected.size() > 4)
        dimension->setValue(dimension->minimum());
    if (selected.size() == 1 && d.contains(selected.first())) {
        const auto &a = d.atom(selected.first());
        for (int i = 0; i < 3; ++i)
            positionFields[i]->setValue(a.position[i]);
        formal->setValue(a.charge);
        radical->setValue(a.radical);
        mass->setText(a.isotope    ? QString::number(a.isotope)
                      : a.mass > 0 ? QString::number(a.mass, 'g', 12)
                                   : "");
        mass->setPlaceholderText("整数: 質量数 / 小数: amu");
    } else {
        for (auto *field : positionFields)
            field->setValue(field->minimum());
        formal->setValue(-99);
        radical->setValue(-1);
        mass->clear();
        mass->setPlaceholderText(selected.isEmpty() ? QString("原子を選択してください")
                                                    : QString("混在（空欄は変更なし）"));
    }
    refreshSelectionControls();
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
    // A new, empty project like a freshly opened file: nothing to save until it is edited.
    Document next;
    applyExecutionPreferences(next);
    path.clear();
    loadDocument(next);
    removeAutosave();
    view->viewFrom(2); // A new molecule starts from the front, whatever the last one was turned to.
    workspaceTabs->setCurrentWidget(moleculePage);
    statusBar()->showMessage("新しい分子を作成しました。", 4000);
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
    // Reading errors name the file (the messages themselves give the line where they can).
    const auto reading = [&](const std::function<Document()> &read) -> Document {
        try {
            return read();
        } catch (const std::exception &e) {
            fail(QString("%1 を読み込めません。\n%2")
                     .arg(QFileInfo(p).fileName(), QString::fromUtf8(e.what())));
        }
    };
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
        next = reading([&] { return importStructure(p, model.toInt(), alt); });
    } else if (QFileInfo(p).suffix().toLower() == "xyz") {
        const int frame = chooseXyzFrame(p);
        next = reading([&] { return importStructure(p, 1, {}, frame); });
    } else if (QFileInfo(p).suffix().toLower() == "nw") {
        NwchemImport imported;
        reading([&] {
            imported = importNwchemInput(QString::fromUtf8(readFile(p)));
            return imported.document;
        });
        next = imported.document;
        if (!imported.notes.isEmpty())
            QMessageBox::information(this, "NWChem入力の取込み",
                                     "構造・電荷・多重度と、MolFormaで扱える計算条件を取り込みました。\n\n"
                                     "取り込まなかった項目:\n・" +
                                         imported.notes.mid(0, 20).join("\n・"));
    } else
        next = reading([&] { return importStructure(p); });
    if (!p.endsWith(".json", Qt::CaseInsensitive))
        applyExecutionPreferences(next); // Structure files carry no execution settings.
    loadDocument(next);
    path = p.endsWith(".json", Qt::CaseInsensitive) ? p : "";
    settings.setValue("directory", QFileInfo(p).absolutePath());
    addRecent(p);
    refresh();
    // A three-dimensional structure is seen at an angle like in the result viewer, so atoms behind
    // each other along an axis stay visible; flat molecules keep the front view for editing.
    if (doc.atoms.size() >= 4) {
        Vec mean = Vec::Zero();
        for (const auto &a : doc.atoms)
            mean += a.position;
        mean /= doc.atoms.size();
        Mat spread = Mat::Zero();
        for (const auto &a : doc.atoms)
            spread += (a.position - mean) * (a.position - mean).transpose();
        if (Eigen::SelfAdjointEigenSolver<Mat>(spread / doc.atoms.size()).eigenvalues()(0) > 0.1) {
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
    if (auto *focused = QApplication::focusWidget();
        focused && (calculation->isAncestorOf(focused) || advanced->isAncestorOf(focused)))
        focused->clearFocus(); // Include the active calculation field when saving via Ctrl+S.
    if (!resolveCandidate())
        return false;
    QString p = path;
    if (as || p.isEmpty())
        p = QFileDialog::getSaveFileName(
            this, "プロジェクトを保存",
            p.isEmpty() ? (doc.atoms.isEmpty() ? QString("molecule") : doc.formula()) + ".kgui.json" : p,
            "MolFormaプロジェクト (*.kgui.json)");
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
QString MainWindow::askSmiles(const QString &title, const QString &label, const QString &initial) {
    QInputDialog dialog(this);
    dialog.setObjectName("smilesDialog");
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextValue(initial);
    dialog.setOkButtonText("3Dで作成");
    dialog.resize(560, dialog.sizeHint().height());
    return dialog.exec() == QDialog::Accepted ? dialog.textValue().trimmed() : QString();
}
void MainWindow::addFragmentFromSmiles(const QString &initial) {
    const auto smiles = askSmiles("SMILESから断片を追加",
                                  "追加する分子のSMILES（例: CCO＝エタノール、c1ccccc1＝ベンゼン）\n"
                                  "今の分子の+X側に置いた候補を作ります。確認してから確定してください。",
                                  initial);
    if (!smiles.isEmpty())
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
    view->keepInView();
}
void MainWindow::importSmiles(const QString &initial) {
    const auto smiles = askSmiles("SMILESから作成",
                                  "SMILES（例: CCO＝エタノール、c1ccccc1＝ベンゼン、[NH4+]＝アンモニウム）\n"
                                  "今の分子を置き換える候補を作ります。確認してから確定してください。",
                                  initial);
    if (!smiles.isEmpty())
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
            [this, running, output, mode, smiles](int code, QProcess::ExitStatus status) {
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
                        double moved = 0;
                        for (int i = 0; i < next.atoms.size() && i < doc.atoms.size(); ++i)
                            moved = std::max(moved, (next.atoms[i].position - doc.atoms[i].position).norm());
                        if (moved < 1e-3) {
                            // Nothing to confirm: say so instead of an empty candidate.
                            statusBar()->showMessage("配座探索: 別の配座は見つからず、現在の構造のままです"
                                                     "（回転できる結合がない分子など）。",
                                                     10000);
                        } else {
                            candidate = next;
                            candidateName = "配座を適用";
                            candidateRemote = workerRemote;
                            statusBar()->showMessage(
                                QString("配座 %1/%2 を候補にしました。「変更を確定」で適用します。")
                                    .arg(chosen + 1)
                                    .arg(list.size()),
                                10000);
                        }
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
                const bool remote = workerRemote;
                running->deleteLater();
                cancelWorkerButton->hide();
                workdir.reset();
                refresh();
                // A new molecule from SMILES is shown whole; refined or added parts only if needed.
                if (mode == "smiles")
                    view->fit();
                else
                    view->keepInView();
                if (error.isEmpty()) {
                    if (mode != "conformers")
                        statusBar()->showMessage(
                            QString("「%1」の候補を作成しました。確認して「変更を確定」してください。")
                                .arg(candidateName),
                            10000);
                    return;
                }
                statusBar()->clearMessage();
                const bool fromSmiles = mode == "smiles" || mode == "fragment";
                QMessageBox box(QMessageBox::Warning,
                                fromSmiles             ? QString("SMILESから作成できません")
                                : mode == "conformers" ? QString("配座探索できません")
                                                       : QString("構造を整形できません"),
                                error, QMessageBox::Close, this);
                // A person may correct the SMILES they typed; MCP clients send a new request.
                auto *retry =
                    fromSmiles && !remote ? box.addButton("SMILESを修正…", QMessageBox::AcceptRole) : nullptr;
                box.exec();
                if (retry && box.clickedButton() == retry)
                    safe([&] { mode == "smiles" ? importSmiles(smiles) : addFragmentFromSmiles(smiles); });
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
        refreshPanels();
        statusBar()->clearMessage();
        QMessageBox::warning(this, "処理を起動できません", message);
    });
    running->start();
    refreshChargePanel();
    refreshPanels();
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
    if (workspaceTabs)
        refreshPanels();
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
void MainWindow::deleteAtoms(const Ids &chosen) {
    const bool terminate = deleteTerminate && deleteTerminate->isChecked();
    edit(
        "選択原子を削除",
        [=](Document &d) {
            Ids ids = chosen;
            // Hydrogens bonded only to deleted atoms go with them.
            for (const auto &id : chosen)
                for (const auto &n : d.neighbors(id))
                    if (d.atom(n).element == "H" && !ids.contains(n)) {
                        const auto partners = d.neighbors(n);
                        if (std::all_of(partners.begin(), partners.end(),
                                        [&](const auto &p) { return ids.contains(p); }))
                            ids << n;
                    }
            Ids cut;
            for (const auto &id : ids)
                if (d.atom(id).element != "H")
                    for (const auto &n : d.neighbors(id))
                        if (!ids.contains(n) && !cut.contains(n))
                            cut << n;
            d.remove(ids);
            // Only heavy atoms leave cut bonds to cap; an empty list would mean the whole molecule.
            if (terminate && !cut.isEmpty())
                completeHydrogens(d, cut);
        },
        true);
}
void MainWindow::selectAll() {
    setSelection(displayDocument().ids());
}
void MainWindow::copySelection() {
    if (!resolveCandidate())
        return;
    Document part = doc;
    if (!selected.isEmpty()) {
        Ids others;
        for (const auto &a : doc.atoms)
            if (!selected.contains(a.id))
                others << a.id;
        for (auto &a : part.atoms)
            a.fixed = false; // Only the copy loses atoms; fixing applies to the document.
        part.remove(others);
    }
    QApplication::clipboard()->setText(part.xyz());
    statusBar()->showMessage(QString("%1原子をXYZでコピーしました。").arg(part.atoms.size()), 5000);
}
void MainWindow::pasteFragment() {
    const auto text = QApplication::clipboard()->text();
    if (text.trimmed().isEmpty())
        fail("クリップボードにXYZがありません。");
    const auto fragment = xyzStructure(text);
    edit("XYZを断片として貼り付け", [&](Document &d) { insertFragment(d, fragment); }, true);
    view->keepInView();
}
QString MainWindow::currentTool() const {
    return toolGroup->checkedButton() ? toolGroup->checkedButton()->property("tool").toString()
                                      : QString("select");
}
void MainWindow::setTool(const QString &code) {
    for (auto *b : toolGroup->buttons())
        if (b->property("tool") == code)
            b->setChecked(true);
}
QString MainWindow::placementKind() const {
    const auto *item = palette->currentItem();
    const auto key = item ? item->data(Qt::UserRole).toString() : QString();
    if (key.isEmpty() || key.startsWith("atom:"))
        return "atom";
    if (key.startsWith("center:") || key.startsWith("smiles:"))
        return key.section(':', 0, 0);
    const auto category = item->data(TemplateDelegate::TemplateRole).toJsonObject()["category"].toString();
    return category == "substituent" || category == "ligand" ? category : "part";
}
void MainWindow::refreshToolHint() {
    if (!mouseHint || !palette)
        return;
    // What a click does with the current tool, naming the element or part it places.
    const auto mode = currentTool();
    const auto part = palette->currentItem() ? palette->currentItem()->text() : QString("パレットで選択");
    const auto symbol = value(elementBox);
    const QMap<QString, QString> places{
        {"atom", QString("配置（%1）: 空白をクリックで配置 · 原子をクリックで結合した%1を追加 · "
                         "結合をクリックで間に挿入")
                     .arg(symbol)},
        {"part", QString("配置（%1）: 空白をクリックで配置 · 原子・水素をクリックで接続").arg(part)},
        {"substituent", QString("配置（%1）: 置き換える水素をクリック（ドロップも可）").arg(part)},
        {"center", QString("配置（配位中心 %1）: 空白をクリックで配置 · 配位形状は「配位」タブ").arg(symbol)},
        {"ligand",
         QString("配置（%1）: 配位中心の紫の番号（空きサイト）をクリック · 空白なら単独で配置").arg(part)},
        {"smiles", QString("配置: パレットの「%1」をクリックすると3D構造を作ります").arg(part)}};
    const QMap<QString, QString> hints{
        {"select",
         "クリック: 選択 · Shift／Ctrl＋クリック: 追加・解除（順番を保持） · Shiftドラッグ: 範囲選択"},
        {"place", places.value(placementKind())},
        {"bond", QString("結合（%1）: 2原子を順にクリック · 種類は左の「結合の種類」で選択")
                     .arg(bondKind->currentText())},
        {"move", "移動: 原子をドラッグ（付いている水素と、選択中なら選択全体も動く） · Altドラッグ: 回転"},
        {"replace",
         QString("置き換え（%1）: 置き換える原子をクリック").arg(palette->currentItem() ? part : symbol)},
        {"delete", "削除: 原子をクリックすると削除の候補を作ります"},
        {"charge", "点電荷: 空白をクリックで配置 · 電荷の値は「電荷」タブ"}};
    // Delete and replace act on the selection unit too, which is set with the select tool.
    auto hint = hints.value(mode);
    if ((mode == "delete" || mode == "replace") && selectionUnit() != "atom")
        hint += QString("（選択単位: %1）").arg(unitGroup->checkedButton()->text());
    mouseHint->setText(hint);
    mouseHint->setToolTip(hint);
    // The option bar shows what the tool uses: the selection unit for the tools that pick atoms,
    // the bond kind for bonds and atoms put on atoms, and what the palette has chosen.
    const auto kind = placementKind();
    const QHash<QString, bool> shown{
        {"selectionUnitOption", mode == "select" || mode == "move"},
        {"partOption", mode == "place" || mode == "replace"},
        {"bondOption", mode == "bond" || (mode == "place" && kind == "atom")},
        {"keepToolOption", mode == "place" && QStringList{"part", "substituent", "ligand"}.contains(kind)},
        {"deleteOption", mode == "delete"},
        {"chargeOption", mode == "charge"}};
    for (auto it = shown.begin(); it != shown.end(); ++it)
        if (auto *group = toolOptionGroups.value(it.key()))
            group->setVisible(it.value());
    const auto &e = element(symbol);
    const auto chosen = mode == "replace" && (kind == "atom" || kind == "center") ? e.nameJa
                        : kind == "atom"   ? QString("%1（%2）").arg(e.nameJa, e.symbol)
                        : kind == "center" ? QString("配位中心 %1（%2）").arg(e.nameJa, e.symbol)
                                           : part;
    toolPart->setText(QString("%1 <b>%2</b>")
                          .arg(mode == "replace" ? "置き換え先" : "配置する部品", chosen.toHtmlEscaped()));
}
void MainWindow::showShortcuts() {
    QDialog dialog(this);
    dialog.setObjectName("shortcutsDialog");
    dialog.setWindowTitle("キーボードショートカット");
    dialog.resize(560, 600);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(0, 3);
    table->setObjectName("shortcutsTable");
    table->setHorizontalHeaderLabels({"キー", "操作", "有効な場所"});
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
        const auto text = a == undoAction   ? QString("元に戻す（候補があれば取消）")
                          : a == redoAction ? QString("やり直す")
                                            : QString(a->text()).remove('&');
        row(keys.join(" / "), text,
            a->shortcutContext() == Qt::WidgetWithChildrenShortcut ? "「1 分子」" : "全体");
    }
    for (const auto &[key, text] : {std::pair{"左ドラッグ", "回転（画面の縦・横軸まわり）"},
                                    {"右ドラッグ", "平行移動"},
                                    {"ホイール", "カーソル位置を中心に拡大・縮小"},
                                    {"クリック", "原子を選択（選択単位に従う）"},
                                    {"Shift／Ctrl＋クリック", "選択へ追加・解除（順番を保持）"},
                                    {"Shift／Ctrlドラッグ", "範囲選択（Ctrlは追加）"},
                                    {"Ctrl+Tab", "次のタブ（タブ内にフォーカスがあるとき）"}})
        row(key, text, "3D画面・タブ");
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
    dialog.setWindowTitle("MolForma 操作ガイド・マニュアル");
    dialog.resize(900, 720);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tips = new QLabel(
        "左のパレットで部品・原子を選び、3D画面をクリックするかドロップして配置します。"
        "左端の列でツールを切り替え、3D画面の上にツールの設定、下にクリックしたときの動作を表示します。"
        "原子をクリックで選択、Shift／Ctrl＋クリックで順に追加し、2〜4原子で距離・角度・二面角を"
        "測定できます。配位子・削除・整形などは「変更を確定」（Ctrl+Enter）で適用し、Escで"
        "候補を取消、Ctrl+Zで確定済みの変更を戻します。計算は「計算 → 計算を実行」（F5）で"
        "始めます。キー操作の一覧は「ヘルプ → キーボードショートカット一覧」にあります。");
    tips->setWordWrap(true);
    layout->addWidget(tips);
    auto *bar = new QHBoxLayout;
    layout->addLayout(bar);
    auto *pages = new QComboBox;
    pages->setObjectName("manualPage");
    pages->addItem("機能と操作（はじめに・各タブの説明）", ":/docs/features.md");
    pages->addItem("MCPサーバー（AIクライアントからの操作）", ":/docs/mcp.md");
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
    const auto add = [&](QMenu *to, const QString &text, auto fn) {
        auto *a = to->addAction(text);
        connect(a, &QAction::triggered, this, [this, fn] { safe(fn); });
    };
    if (!selected.isEmpty()) {
        add(&menu, "選択原子を削除（候補）", [this] { deleteAtoms(requireSelection()); });
        add(&menu, QString("元素を %1 に変更").arg(value(elementBox)), [this] {
            auto ids = requireSelection();
            auto symbol = value(elementBox);
            edit("元素を変更", [=](Document &d) { replaceElement(d, ids, symbol, false); });
        });
        // As "2 座標": fixed only when every selected atom is, else all of them get fixed.
        const bool allFixed = std::all_of(selected.begin(), selected.end(), [this](const QString &a) {
            return displayDocument().atom(a).fixed;
        });
        add(&menu, allFixed ? "固定を解除" : "選択原子を固定", [this, allFixed] {
            auto ids = requireSelection();
            edit(allFixed ? "選択原子の固定を解除" : "選択原子を固定", [=](Document &d) {
                for (auto a : ids)
                    d.atom(a).fixed = !allFixed;
            });
        });
        if (selected.size() <= 3)
            add(&menu, "選択順に向きをそろえる（原点・+X・XY面）", [this] {
                auto ids = requireSelection();
                edit("向きをそろえる", [=](Document &d) { orient(d, ids); });
                view->keepInView();
            });
        if (selected.size() == 2) {
            auto *bonds = menu.addMenu("2原子の結合");
            for (const auto &[label, kind] : {std::pair{"単結合", "single"},
                                              {"二重結合", "double"},
                                              {"三重結合", "triple"},
                                              {"芳香族", "aromatic"},
                                              {"配位（供与→中心）", "dative"},
                                              {"結合を削除", "none"}})
                add(bonds, label, [this, kind = QString(kind)] {
                    auto ids = requireSelection(2);
                    edit("結合を変更", [=](Document &d) { d.setBond(ids[0], ids[1], kind); });
                });
        }
        menu.addSeparator();
    }
    add(&menu, "全体を表示", [this] { view->fit(); });
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
    dialog.setProperty("canvas", true);
    dialog.setWindowTitle("環境設定");
    dialog.resize(640, 700);
    // The cards and controls of "5 実行設定", for the settings new documents start with.
    dialog.setStyleSheet(
        chromeRules(settingsRules() + "QDialog { background: @window; }", QMainWindow::palette()));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(12);
    auto *intro = new QLabel("新規作成、およびプロジェクト以外の構造ファイル（XYZ・SDF・PDB等）を開いたときの"
                             "実行設定です。開いているプロジェクトの設定は「5 実行設定」で変更します。");
    intro->setWordWrap(true);
    intro->setProperty("muted", true);
    layout->addWidget(intro);
    const auto saved = executionPreferences();
    const auto card = [&](const QString &title) {
        auto *c = new SettingsCard(title);
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(8);
        c->body()->addLayout(form);
        layout->addWidget(c);
        return form;
    };
    auto *files = card("実行ファイル・保存先");
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
        files->addRow(label, row);
        return line;
    };
    auto *executable = path("NWChem実行ファイル", "executable", false);
    auto *mode = new QComboBox;
    mode->setObjectName("preferences_mode");
    mode->addItem("MPIで起動", "mpi");
    mode->addItem("直接実行（逐次版など）", "direct");
    mode->setCurrentIndex(std::max(0, mode->findData(saved["mode"].toString())));
    files->addRow("起動方式", new ChoiceButtons(mode, ChoiceButtons::Segments));
    auto *launcher = new QLineEdit(saved["launcher"].toString());
    launcher->setObjectName("preferences_launcher");
    files->addRow("MPIランチャー", launcher);
    auto *root = path("ジョブ保存先", "job_root", true);
    auto *resources = card("計算資源");
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
    auto *ranks = spin("MPI起動プロセス数", "mpi_ranks", 1, 4096);
    auto *omp = spin("OpenMPスレッド / プロセス", "omp_threads", 1, 1024);
    auto *blas = spin("BLASスレッド / プロセス", "blas_threads", 1, 1024);
    auto *stack = new QLineEdit(saved["omp_stacksize"].toString());
    stack->setObjectName("preferences_omp_stacksize");
    stack->setPlaceholderText("空欄: 継承（例: 64M）");
    resources->addRow("OpenMPスタック", stack);
    auto *timeout = spin("制限時間（0: なし）", "timeout_seconds", 0, 604800, " 秒");
    // The environment as the table of "5 実行設定": rows that are off are kept.
    auto *environmentCard = new SettingsCard("追加の環境変数");
    auto *environment = new QTableWidget(0, 3);
    environment->setObjectName("preferences_environment");
    environment->setHorizontalHeaderLabels({"有効", "名前", "値"});
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
    auto *addRow = new QPushButton("追加");
    addRow->setIcon(lineIcon("plus"));
    auto *removeRow = new QPushButton("選択行を削除");
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
    auto *environmentNote = new QLabel("有効な行だけを起動環境へ上書きします（例: NWCHEM_BASIS_LIBRARY）。");
    environmentNote->setProperty("caption", true);
    environmentNote->setWordWrap(true);
    environmentCard->body()->addWidget(environmentNote);
    layout->addWidget(environmentCard);
    auto *startup = card("起動と3D表示");
    auto *water = new QCheckBox("起動時に水分子を読み込む");
    water->setObjectName("preferences_startupWater");
    water->setChecked(settings.value("startupWater", false).toBool());
    startup->addRow("新しいウィンドウ", water);
    auto *renderer = new QComboBox;
    renderer->addItem("自動（VTKを優先）", "auto");
    renderer->addItem("QPainter（CPU）", "painter");
    renderer->setCurrentIndex(settings.value("view/renderer").toString() == "painter" ? 1 : 0);
    startup->addRow("3D描画（再起動後に反映）", renderer);
    layout->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
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
            settings.setValue("startupWater", water->isChecked());
            settings.setValue("view/renderer", renderer->currentData().toString());
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
    setPaletteCategory("my");
    refreshPalette();
}
} // namespace mf
