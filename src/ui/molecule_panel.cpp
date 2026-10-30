// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/molecule_panel.h"
#include "editing/atom_properties.h"
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/coordination.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "editing/stereochemistry.h"
#include "io/structure_io.h"
#include "model/i18n.h"
#include "model/topology.h"
#include "nwchem/nwchem.h"
#include "ui/candidate_controls.h"
#include "ui/coordinates_panel.h"
#include "ui/editor_chrome.h"
#include "ui/main_window.h"
#include "ui/periodic_table.h"
#include "ui/structure_import.h"
#include "ui/template_delegate.h"
#include "ui/theme.h"
#include <QDrag>
#include <QJsonDocument>
#include <QMimeData>
#include <QtWidgets>

namespace mf {
// The ways to lay out "1 分子" ("view", "both", "table"): the segments under the view and "表示".
static QStringList moleculeLayoutNames() {
    return {"3D", tr("3D＋表"), tr("表")};
}
// Lays the page buttons of the right column out in one row while their names fit, else in two
// rows (the English names are wider than the column).
class PageRows : public QObject {
    QFrame *frame;
    QGridLayout *grid;
    int columns = 0;

  public:
    PageRows(QFrame *frame, QGridLayout *grid) : QObject(frame), frame(frame), grid(grid) {
        frame->installEventFilter(this);
    }
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Resize)
            arrange();
        return false;
    }
    void arrange() {
        QList<QWidget *> buttons;
        int width = grid->contentsMargins().left() + grid->contentsMargins().right();
        for (int i = 0; i < grid->count(); ++i) {
            buttons << grid->itemAt(i)->widget();
            width += buttons.last()->sizeHint().width() + (i ? grid->horizontalSpacing() : 0);
        }
        const int wanted = width <= frame->width() ? buttons.size() : (buttons.size() + 1) / 2;
        if (wanted == columns || buttons.isEmpty())
            return;
        columns = wanted;
        for (auto *b : buttons)
            grid->removeWidget(b);
        for (int i = 0; i < buttons.size(); ++i) {
            grid->addWidget(buttons[i], i / columns, i % columns);
            grid->setColumnStretch(i, i < columns ? 1 : 0);
        }
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
// An atom as the 3D view labels it ("C14"); atomPlace() gives its place in a residue.
static QString atomName(const Document &d, const QString &id) {
    return d.atom(id).element + QString::number(d.index(id) + 1);
}

MoleculePanel::MoleculePanel(MainWindow &owner, const EditorSession &editor, QSettings &preferences)
    : QWidget(&owner), window(owner), session(editor), settings(preferences) {
    customParts = settings.value("parts").toJsonArray();
    auto *root = this;
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
    for (const auto &[tool, label, key, tip] :
         {std::tuple{EditorTool::Select, tr("選択"), "S",
                     tr("原子を選択します。Shift／Ctrl＋クリックで追加、Shiftドラッグで範囲選択。")},
          {EditorTool::Move, tr("移動"), "M",
           tr("原子をドラッグして動かします（付いている水素と、選択中なら選択全体が一緒に動きます）。")},
          {EditorTool::Bond, tr("結合"), "B", tr("2原子を順にクリックして、上の「結合の種類」にします。")},
          {EditorTool::Delete, tr("削除"), "X", tr("クリックした原子を削除する候補を作ります。")},
          {EditorTool::Place, tr("配置"), "A",
           tr("パレットで選んだ原子・部品を置きます。空白・原子・水素・配位中心の紫の番号のどれを"
              "クリックするかで置き方が決まります。")},
          {EditorTool::Replace, tr("置き換え"), "R",
           tr("クリックした原子を、パレットで選んだ元素・部品に置き換えます。")},
          {EditorTool::Charge, tr("点電荷"), "Q", tr("空白をクリックして外部点電荷を置きます。")}}) {
        auto *b = new ToolRailButton(key);
        b->setObjectName("tool_" + editorToolKey(tool));
        b->setText(label);
        b->setIcon(lineIcon(editorToolKey(tool)));
        b->setToolTip(tip);
        b->setProperty("toolKey", key);
        railLayout->addWidget(b, 0, Qt::AlignHCenter);
        toolGroup->addButton(b, int(tool));
    }
    railLayout->addStretch();
    // The palette folds away for a wider view and table; the tools that place from it open it.
    paletteToggle = new ToolRailButton({});
    paletteToggle->setObjectName("paletteToggle");
    paletteToggle->setText(tr("パレット"));
    paletteToggle->setIcon(lineIcon("layers"));
    paletteToggle->setToolTip(tr("部品パレットを表示・畳みます。畳むと3D画面と座標の表が広がります（「配置」"
                                 "「置き換え」を選ぶと開きます）。"));
    railLayout->addWidget(paletteToggle, 0, Qt::AlignHCenter);
    connect(paletteToggle, &QToolButton::clicked, this, [this](bool on) { showPalette(on); });
    toolGroup->buttons().first()->setChecked(true);
    page->addWidget(rail);
    auto *splitter = new QSplitter;
    splitter->setObjectName("editorSplitter");
    splitter->setChildrenCollapsible(false);
    page->addWidget(splitter, 1);
    // The palette: search, categories, the common elements and the parts as tiles.
    auto *left = paletteColumn = new QWidget;
    left->setObjectName("paletteColumn");
    left->setAttribute(Qt::WA_StyledBackground);
    left->setMinimumWidth(230); // Three columns of tiles and the element buttons.
    left->setMaximumWidth(360);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(12, 12, 8, 8);
    ll->setSpacing(8);
    auto *paletteTitle = new QLabel(tr("部品パレット"));
    auto titleFont = paletteTitle->font();
    titleFont.setBold(true);
    paletteTitle->setFont(titleFont);
    ll->addWidget(paletteTitle);
    search = new QLineEdit;
    search->setObjectName("paletteSearch");
    search->setPlaceholderText(tr("名前・元素・SMILESを検索"));
    search->setClearButtonEnabled(true);
    search->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    ll->addWidget(search);
    auto *categories = new QWidget;
    categories->setObjectName("paletteCategories");
    auto *chips = new FlowLayout(categories, 5, 5);
    chips->setContentsMargins(0, 0, 0, 0);
    categoryGroup = new QButtonGroup(this);
    for (const auto &[label, code, tip] :
         {std::tuple{tr("すべて"), "all",
                     tr("原子・骨格・置換基・配位子・アミノ酸・配位中心を、見出しを付けて並べます。")},
          {tr("原子"), "atom", tr("よく使う元素です。ほかの元素は周期表（H〜Og）から選びます。")},
          {tr("骨格"), "skeleton", ""},
          {tr("置換基"), "substituent", tr("置き換える水素をクリックして付けます。")},
          {tr("配位中心"), "center",
           tr("配位中心です。金属以外（S・P・Xeなど）は元素ボタン・周期表・検索で選びます。")},
          {tr("配位子"), "ligand", tr("配位中心の紫の番号（空きサイト）をクリックして付けます。")},
          {tr("アミノ酸"), "amino", tr("アミノ酸と末端キャップです。")},
          {tr("マイ部品"), "my", tr("選択した原子から保存した部品と、ファイルから読み込んだ部品です。")}}) {
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
        b->setToolTip(tr("%1（%2）を「配置」で置きます").arg(symbol, element(symbol).name()));
        b->setCheckable(true);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setMinimumWidth(16);
        const int i = quick->count();
        quick->addWidget(b, i / 6, i % 6);
        connect(b, &QToolButton::clicked, this, [this, symbol] { safe([&] { chooseElement(symbol); }); });
    }
    periodicTableButton = new QPushButton(tr("周期表…"));
    periodicTableButton->setObjectName("openPeriodicTable");
    periodicTableButton->setToolTip(tr("周期表から原子を選択します（H〜Og）。"));
    periodicTableButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    quick->addWidget(periodicTableButton, 1, 4, 1, 2);
    for (int c = 0; c < 6; ++c)
        quick->setColumnStretch(c, 1);
    connect(periodicTableButton, &QPushButton::clicked, this, [this] {
        PeriodicTableDialog dialog(paletteCategory() == "center" && !centerElement.isEmpty() ? centerElement
                                                                                             : paletteElement,
                                   search->text(), this);
        if (dialog.exec() == QDialog::Accepted)
            safe([&] { chooseElement(dialog.selectedElement()); });
    });
    ll->addWidget(elements);
    growth = new QCheckBox(tr("アミノ酸の連続追加"));
    ll->addWidget(growth);
    // The user's parts: saved from the selection or read from a file, with their category.
    myPartsBar = new QWidget;
    myPartsBar->setObjectName("myPartsBar");
    auto *myParts = new FlowLayout(myPartsBar, 5, 5);
    myParts->setContentsMargins(0, 0, 0, 0);
    auto *savePartButton = new QPushButton(tr("選択原子を保存…"));
    savePartButton->setObjectName("savePart");
    savePartButton->setToolTip(
        tr("選択した原子を部品としてマイ部品に登録します（接続点・種類を選びます）。"));
    enableWithSelection(savePartButton, 1);
    auto *loadPartButton = new QPushButton(tr("読み込む…"));
    loadPartButton->setObjectName("loadPart");
    loadPartButton->setToolTip(tr("部品ファイル（.molforma-part.json）をマイ部品に読み込みます。"));
    auto *removePartButton = new QPushButton(tr("一覧から外す"));
    removePartButton->setObjectName("removePart");
    removePartButton->setToolTip(tr("パレットで選んだマイ部品を一覧から外します（ファイルは消しません）。"));
    for (auto *b : {savePartButton, loadPartButton, removePartButton})
        myParts->addWidget(b);
    myPartsEmpty = new QLabel(tr("マイ部品はまだありません。3D画面で原子を選んで「選択原子を保存…」を押すか、"
                                 "部品ファイルを「読み込む…」で追加します。"));
    myPartsEmpty->setObjectName("myPartsEmpty");
    myPartsEmpty->setWordWrap(true);
    myPartsEmpty->hide();
    connect(savePartButton, &QPushButton::clicked, this, [this] { safe([this] { savePart(); }); });
    connect(loadPartButton, &QPushButton::clicked, this, [this] { safe([this] { loadPart(); }); });
    connect(removePartButton, &QPushButton::clicked, this, [this] { safe([this] { removePart(); }); });
    myPartsBar->hide();
    ll->addWidget(myPartsBar);
    ll->addWidget(myPartsEmpty);
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
    // Folded in an earlier session, it waits for the rail's "パレット" or a tool that uses it.
    const bool paletteShown = settings.value("molecule/palette", true).toBool();
    left->setVisible(paletteShown);
    paletteToggle->setChecked(paletteShown);
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
        tr("原子をクリックしたときにまとめて選ぶ範囲です（原子・部品・残基・つながった分子・全体）。"));
    auto *unitRow = new QHBoxLayout(units);
    unitRow->setContentsMargins(2, 2, 2, 2);
    unitRow->setSpacing(2);
    unitGroup = new QButtonGroup(this);
    for (const auto &[label, code] : {std::pair{tr("原子"), "atom"},
                                      {tr("結合"), "bond"},
                                      {tr("部品"), "part"},
                                      {tr("残基"), "residue"},
                                      {tr("分子"), "component"},
                                      {tr("全体"), "all"}}) {
        auto *b = new QToolButton;
        b->setObjectName(QString("selectionUnit_") + code);
        b->setText(label);
        b->setCheckable(true);
        b->setProperty("unit", code);
        unitRow->addWidget(b);
        unitGroup->addButton(b);
    }
    unitGroup->buttons().first()->setChecked(true);
    auto *unitLabel = new QLabel(tr("選択単位"));
    optionGroup("selectionUnitOption", {unitLabel, units});
    toolPart = new QLabel;
    toolPart->setObjectName("toolPart");
    toolPart->setTextFormat(Qt::RichText);
    optionGroup("partOption", {toolPart});
    auto *bondLabel = new QLabel(tr("結合の種類"));
    toolBondKind = new QComboBox;
    toolBondKind->setObjectName("toolBondKind");
    toolBondKind->setToolTip(tr("「結合」ツールで作る結合と、「配置」で原子に原子をつなぐ結合の種類です。"));
    toolBondKind->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    bondLabel->setBuddy(toolBondKind);
    optionGroup("bondOption", {bondLabel, toolBondKind});
    keepTool = new QCheckBox(tr("部品の配置後もツールを保持"));
    keepTool->setToolTip(tr("オフでは、骨格・置換基・アミノ酸などの部品を1つ置くと「選択」ツールに戻ります。"
                            "原子・配位中心や配位サイトへの配位子の配置は、切り替えるまで続きます。"));
    keepTool->setChecked(settings.value("keepTool", false).toBool());
    optionGroup("keepToolOption", {keepTool});
    toolDeleteTerminate = new QCheckBox(tr("削除後に水素で終端化"));
    toolDeleteTerminate->setObjectName("toolDeleteTerminate");
    toolDeleteTerminate->setChecked(true);
    toolDeleteTerminate->setToolTip(tr("すべての削除に適用します（「編集」ページの同じ項目と共通）。"));
    optionGroup("deleteOption", {toolDeleteTerminate});
    auto *chargeLabel = new QLabel(tr("置く点電荷 / e"));
    toolChargeValue = new QDoubleSpinBox;
    toolChargeValue->setObjectName("toolChargeValue");
    toolChargeValue->setDecimals(5);
    toolChargeValue->setRange(-10000, 10000);
    toolChargeValue->setToolTip(tr("「電荷」ページの「電荷 / e」と共通です。"));
    chargeLabel->setBuddy(toolChargeValue);
    optionGroup("chargeOption", {chargeLabel, toolChargeValue});
    // The structure commands stay at the right end, after the options of the tool.
    options->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum));
    autoH = new QCheckBox(tr("水素自動補完"));
    autoH->setToolTip(tr("原子の追加・結合の変更・元素の変更のあとに、空いた原子価へ水素を付けます。"));
    autoH->setChecked(true);
    auto *h = new QPushButton(tr("水素を補完"));
    h->setToolTip(tr("選択原子（未選択なら分子全体）の空いた原子価に水素を付けます。"));
    connect(h, &QPushButton::clicked, this, [this] {
        safe([this] { edit(tr("水素を補完"), [this](Document &d) { completeHydrogens(d, selected); }); });
    });
    auto *refineButton = new QPushButton(tr("全体整形"));
    refineButton->setObjectName("refineButton");
    refineButton->setToolTip(
        tr("力場（MMFF94、使えない場合UFF）で分子全体の構造を整え、候補として表示します。"
           "金属錯体・ゴースト原子を含む分子には使えません。選択原子だけを整えるときは"
           "「編集」の「局所整形」を使います。"));
    connect(refineButton, &QPushButton::clicked, this, [this] { safe([this] { runWorker("refine"); }); });
    cancelWorkerButton = new QPushButton(tr("整形を中止"));
    cancelWorkerButton->hide();
    connect(cancelWorkerButton, &QPushButton::clicked, this, &MoleculePanel::cancelWorker);
    optionGroup("structureOption", {autoH, h, refineButton, cancelWorkerButton});
    topRow->addWidget(optionBar);
    banner = new CandidateControls(true);
    connect(banner, &CandidateControls::confirmRequested, this,
            [this] { safe([this] { confirmCandidate(); }); });
    connect(banner, &CandidateControls::cancelRequested, this, &MoleculePanel::cancelCandidate);
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
        safe([this, rid] { setSelection(residue(displayDocument(), rid).atoms.values()); });
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
    auto *applyDimension = new QPushButton(tr("寸法を適用"));
    applyDimension->setObjectName("applyDimension");
    applyDimension->setToolTip(
        tr("選択順の2〜4原子の距離・角度・二面角を指定値にします。後に選んだ原子の側が動きます。"));
    dimensions->addWidget(applyDimension);
    enableWithSelection(dimension, 2, 4);
    enableWithSelection(applyDimension, 2, 4);
    measureLayout->addLayout(dimensions);
    connect(applyDimension, &QPushButton::clicked, this, [this] {
        safe([this] {
            auto ids = requireSelection();
            double v = dimension->value();
            edit(tr("寸法を適用"), [=](Document &d) { setDimension(d, ids, v); });
        });
    });
    measureCard->hide();
    viewArea->addOverlay(measureCard, Qt::AlignBottom | Qt::AlignLeft);
    // The 3D view over the table of the coordinates, or either of them alone.
    moleculeSplit = new QSplitter(Qt::Vertical);
    moleculeSplit->setObjectName("moleculeSplit");
    moleculeSplit->setChildrenCollapsible(false);
    // The view gives way to four rows of the table, under its bars wrapped where the window is narrow.
    viewArea->setMinimumHeight(120);
    moleculeSplit->addWidget(viewArea);
    coordinates = new CoordinatesPanel(&window);
    moleculeSplit->addWidget(coordinates);
    // 3D:表 = 6:4 until the border is dragged (applyMoleculeLayout() restores the drag). Without
    // stretch factors the sizes are shares, kept as the window is resized.
    moleculeSplit->setSizes({600, 400});
    connect(moleculeSplit, &QSplitter::splitterMoved, this,
            [this] { settings.setValue("molecule/split", moleculeSplit->saveState()); });
    ml->addWidget(moleculeSplit, 1);
    // Under them: what a click does with the tool and the mouse controls of the view, and the layout.
    auto *hintBar = new QWidget;
    hintBar->setObjectName("hintBar");
    hintBar->setAttribute(Qt::WA_StyledBackground);
    auto *hintRow = new QHBoxLayout(hintBar);
    hintRow->setContentsMargins(12, 4, 8, 5);
    hintRow->setSpacing(12);
    auto *hints = new QVBoxLayout;
    hints->setSpacing(1);
    hintRow->addLayout(hints, 1);
    // One line each (shortened when narrow, in full as a tooltip), so the view never moves.
    mouseHint = new ElidedLabel;
    mouseHint->setObjectName("toolHint");
    hints->addWidget(mouseHint);
    auto *controlsHint = new ElidedLabel(tr("%1 · Esc: 候補・選択を解除").arg(viewControlsHint()));
    controlsHint->setObjectName("controlsHint");
    controlsHint->setToolTip(controlsHint->text());
    auto hintFont = controlsHint->font();
    hintFont.setPointSizeF(hintFont.pointSizeF() * .9);
    controlsHint->setFont(hintFont);
    hints->addWidget(controlsHint);
    moleculeLayout = new QComboBox;
    moleculeLayout->setObjectName("moleculeLayout");
    moleculeLayout->addItem(tr("3D表示"), "view");
    moleculeLayout->addItem(tr("3D表示と座標の表"), "both");
    moleculeLayout->addItem(tr("座標の表"), "table");
    auto *layouts = new ChoiceButtons(moleculeLayout, ChoiceButtons::Segments, moleculeLayoutNames());
    layouts->setObjectName("moleculeLayouts");
    layouts->setToolTip(
        tr("3D表示と座標の表の並べ方です。表では座標・質量・ゴースト・固定を直接編集します。"));
    hintRow->addWidget(layouts, 0, Qt::AlignVCenter);
    moleculeLayout->setCurrentIndex(
        std::max(0, moleculeLayout->findData(settings.value("molecule/layout", "view").toString())));
    connect(moleculeLayout, &QComboBox::currentIndexChanged, this, [this] {
        settings.setValue("molecule/layout", moleculeLayout->currentData().toString());
        applyMoleculeLayout();
    });
    ml->addWidget(hintBar);
    splitter->addWidget(middle);
    applyMoleculeLayout();
    // The properties: one page at a time, chosen with segments (their tab bar stays hidden).
    propertyColumn = new QWidget;
    propertyColumn->installEventFilter(this);
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
    auto *pageRow = new QGridLayout(pages);
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
        pageRow->addWidget(b, 0, i);
        propertyGroup->addButton(b, i);
    }
    (new PageRows(pages, pageRow))->arrange();
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
    // View toggles: in the "表示" menu and the "表示" button over the 3D view.
    showHydrogens = new QAction(tr("水素を表示"), this);
    showHydrogens->setObjectName("showHydrogensAction");
    showHydrogens->setToolTip(tr("水素原子の表示を切り替えます（H）。隠しても計算・保存には含みます。"));
    showHydrogens->setCheckable(true);
    showHydrogens->setChecked(true);
    showIndices = new QAction(tr("番号を表示"), this);
    showIndices->setObjectName("showIndices");
    showIndices->setToolTip(tr("ラベルに原子の通し番号（座標表・NWChem入力の順番）を付けます。"));
    showIndices->setCheckable(true);
    showIndices->setChecked(true);
    labelModes = new QActionGroup(this);
    const auto labels = settings.value("view/labels", "all").toString();
    for (const auto &[label, mode] : {std::pair{tr("すべて"), "all"},
                                      {tr("水素以外"), "heavy"},
                                      {tr("選択した原子"), "selection"},
                                      {tr("なし"), "none"}}) {
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
        view->setTool(tool());
        // "選択" does not place the palette part: it is no longer shown as chosen. The tools that use
        // it show again the one chosen last.
        const bool placing = tool() == EditorTool::Place || tool() == EditorTool::Replace;
        if (tool() == EditorTool::Select)
            palette->clearSelection();
        else if (placing && palette->currentItem())
            palette->currentItem()->setSelected(true);
        if (placing)
            showPalette(true);
        refreshToolHint();
    });
    connect(elementBox, &QComboBox::currentIndexChanged, this, &MoleculePanel::refreshToolHint);
    connect(palette, &QListWidget::currentItemChanged, this, &MoleculePanel::refreshToolHint);
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
    connect(search, &QLineEdit::textChanged, this, &MoleculePanel::refreshPalette);
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
            if (tool() == EditorTool::Select)
                setSelection({});
            else
                place({}, p);
        });
    });
    connect(view, &MoleculeView::bondClicked, this, [this](auto a, auto b) {
        safe([this, a, b] {
            if (tool() == EditorTool::Place && paletteChoice().kind == PaletteKind::Atom) {
                edit(tr("結合間に原子を挿入"),
                     [=, this](Document &d) { insertAtom(d, a, b, value(elementBox), autoH->isChecked()); });
            } else
                setSelection({a, b});
        });
    });
    connect(view, &MoleculeView::siteClicked, this, [this](auto c, int site) {
        safe([this, c, site] {
            setSelection({c});
            if (tool() == EditorTool::Place && paletteChoice().kind == PaletteKind::Ligand) {
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
                    // The next site continues the placement: the ligand placed before is confirmed. A
                    // person's click never confirms an MCP candidate, but MCP may continue its own.
                    const bool chained = session.candidate() &&
                                         session.candidateSerial() == ligandCandidate &&
                                         (!session.candidateRemote() || window.remoteRequest());
                    if (chained)
                        confirmCandidate();
                    proposeLigand(c, indices, t);
                    if (chained)
                        statusBar()->showMessage(tr("前の配位子を確定し、次の配位子を候補にしました。"),
                                                 8000);
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
        const Topology topology(d);
        QVector<bool> carried(d.atoms.size());
        for (const auto &atom : ids)
            if (const int i = topology.index(atom); i >= 0)
                carried[i] = true;
        for (const auto &atom : Ids(ids))
            if (const int i = topology.index(atom); i >= 0)
                for (int n : topology.neighbors(i))
                    if (!carried[n] && d.atoms[n].element == "H" && topology.neighbors(n).size() == 1) {
                        carried[n] = true;
                        ids << d.atoms[n].id;
                    }
        return ids;
    };
    connect(view, &MoleculeView::dragStarted, this,
            [this, moving](const QString &id) { safe([&] { view->setDragAtoms(moving(id)); }); });
    connect(view, &MoleculeView::atomDragged, this, [this, moving](auto id, Vec delta) {
        safe([this, id, delta, moving] {
            const auto ids = moving(id);
            edit(tr("原子を移動"), [=](Document &d) { transform(d, ids, Mat::Identity(), delta); });
        });
    });
    connect(view, &MoleculeView::rectangleSelected, this, [this](Ids ids, bool append) {
        safe([this, ids, append] {
            auto next = append ? selected : Ids();
            QSet<QString> listed(next.begin(), next.end());
            for (const auto &id : expandedSelection(ids))
                if (!listed.contains(id)) {
                    listed.insert(id);
                    next << id;
                }
            setSelection(next);
        });
    });
    connect(view, &MoleculeView::contextRequested, this, &MoleculePanel::contextMenu);
    connect(view, &MoleculeView::shortcut, this, [this](int key) {
        safe([this, key] {
            if (key == Qt::Key_Escape) {
                if (session.candidate())
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
                if (hasSelection(2))
                    bondAtoms(selected[0], selected[1],
                              QStringList{"single", "double", "triple"}[key - Qt::Key_1]);
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
                        if (const auto entry = paletteEntry(key); entry.kind == PaletteKind::Atom)
                            paletteElement = element(entry.value).symbol;
                        search->clear();
                        setPaletteCategory("all");
                        refreshPalette();
                        if (!select())
                            fail(tr("部品が見つかりません: %1").arg(key));
                    }
                    paletteChosen();
                    if (!rid.isEmpty())
                        addAmino(rid, n);
                    else if (!c.isEmpty()) {
                        auto t = currentTemplate();
                        setSelection({c});
                        siteInput->setText(QString::number(site + 1));
                        proposeLigand(c, {site}, t);
                    } else
                        place(atom, p);
                });
            });
    refreshPalette();
    refreshToolHint();
}
void MoleculePanel::addEditActions(QMenu *editMenu) {
    const auto action = [&](QMenu *menu, const QString &text, const QKeySequence &key, const QString &icon,
                            auto fn) {
        auto *a = menu->addAction(text);
        if (!icon.isEmpty())
            a->setIcon(lineIcon(icon));
        if (!key.isEmpty())
            a->setShortcut(key);
        connect(a, &QAction::triggered, this, [this, fn] { safe(fn); });
        return a;
    };
    const auto local = [&](QMenu *m, const QString &text, const QKeySequence &key, const QString &icon,
                           auto fn, const QString &name) {
        auto *a = action(m, text, key, icon, fn);
        a->setObjectName(name);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(a);
        return a;
    };
    local(
        editMenu, tr("選択原子を削除"), QKeySequence::Delete, "delete",
        [this] {
            if (hasSelection())
                deleteAtoms(selected);
        },
        "deleteAction");
    local(
        editMenu, tr("すべて選択"), QKeySequence::SelectAll, "selectAll", [this] { selectAll(); },
        "selectAllAction");
    local(
        editMenu, tr("選択をXYZでコピー"), QKeySequence::Copy, "copy", [this] { copySelection(); },
        "copyAction");
    local(
        editMenu, tr("XYZを断片として貼り付け"), QKeySequence::Paste, "paste", [this] { pasteFragment(); },
        "pasteAction");
    auto *orders = editMenu->addMenu(lineIcon("bond"), tr("選択した2原子の結合次数"));
    for (int n = 1; n <= 3; ++n)
        local(
            orders, QStringList{tr("単結合"), tr("二重結合"), tr("三重結合")}[n - 1],
            QKeySequence(Qt::Key_0 + n), QString("bond%1").arg(n),
            [this, n] {
                if (hasSelection(2))
                    bondAtoms(selected[0], selected[1], QStringList{"single", "double", "triple"}[n - 1]);
            },
            QString("bondOrder%1Action").arg(n));
    // One key per editing tool while "1 分子" has the focus (text fields keep their letters).
    auto *editingTools = editMenu->addMenu(lineIcon("select"), tr("ツールを選ぶ"));
    for (auto *b : toolGroup->buttons()) {
        const auto tool = EditorTool(toolGroup->id(b));
        const auto key = b->property("toolKey").toString();
        local(
            editingTools, b->text(), QKeySequence(key), editorToolKey(tool), [this, tool] { setTool(tool); },
            "toolAction_" + editorToolKey(tool));
        b->setToolTip(tr("%1（%2）").arg(b->toolTip(), key));
    }
    editMenu->addSeparator();
    action(editMenu, tr("番号で原子を選択…"), {}, "hash", [this] { selectNumbers(); });
    auto *attributes = action(editMenu, tr("座標の表（属性の列つき）"), {}, "table",
                              [this] { coordinates->showAttributes(); });
    attributes->setObjectName("coordinateAttributesAction");
    attributes->setToolTip(tr("「1 分子」の座標の表に形式電荷・不対電子・原子名の列を表示します。"));
    editMenu->addSeparator();
}
void MoleculePanel::addViewActions(QMenu *viewMenu) {
    const auto action = [&](QMenu *menu, const QString &text, const QKeySequence &key, const QString &icon,
                            auto fn) {
        auto *a = menu->addAction(text);
        if (!icon.isEmpty())
            a->setIcon(lineIcon(icon));
        if (!key.isEmpty())
            a->setShortcut(key);
        connect(a, &QAction::triggered, this, [this, fn] { safe(fn); });
        return a;
    };
    const auto local = [&](QMenu *m, const QString &text, const QKeySequence &key, const QString &icon,
                           auto fn, const QString &name) {
        auto *a = action(m, text, key, icon, fn);
        a->setObjectName(name);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(a);
        return a;
    };
    // H toggles the hydrogens while "1 分子" has the focus, like the other editing keys.
    showHydrogens->setShortcut(QKeySequence(Qt::Key_H));
    showHydrogens->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(showHydrogens);
    viewMenu->addAction(showHydrogens);
    viewMenu->addAction(showIndices);
    auto *labelMenu = viewMenu->addMenu(lineIcon("label"), tr("ラベル"));
    labelMenu->setToolTipsVisible(true);
    labelMenu->menuAction()->setToolTip(tr(
        "元素記号・番号を表示する原子です。ラベルはほかのラベル・原子に重ならない向きに置き、置けないものは"
        "選択した原子・配位中心・水素以外・手前の原子を優先して省きます。"));
    labelMenu->addActions(labelModes->actions());
    auto *styles = viewMenu->addMenu(lineIcon("sphere"), tr("表示スタイル"));
    auto *styleGroup = new QActionGroup(this);
    const auto chosenStyle = settings.value("view/style", "ball_and_stick").toString();
    for (const auto &[label, style, name] :
         {std::tuple{tr("ボール＆スティック"), "ball_and_stick", "viewStyleBallAndStick"},
          {tr("スティック"), "stick", "viewStyleStick"},
          {tr("空間充填（CPK）"), "spacefill", "viewStyleSpacefill"},
          {tr("ワイヤーフレーム"), "wireframe", "viewStyleWireframe"}}) {
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
    auto *hideAction =
        action(viewMenu, tr("選択した原子を隠す"), QKeySequence("Ctrl+Shift+H"), "eyeOff", [this] {
            auto hidden = view->hiddenAtoms();
            for (const auto &id : requireSelection())
                hidden.insert(id);
            setSelection({});
            view->setHidden(hidden);
            statusBar()->showMessage(
                tr("%n原子を隠しています（表示のみ。計算・保存には含みます）。", nullptr, hidden.size()),
                8000);
        });
    hideAction->setObjectName("viewHideSelection");
    action(viewMenu, tr("すべての原子を表示"), QKeySequence("Ctrl+Shift+U"), "eye", [this] {
        view->setHidden({});
    })->setObjectName("viewShowAll");
    viewMenu->addSeparator();
    auto *hbondAction = viewMenu->addAction(tr("水素結合を表示"));
    hbondAction->setObjectName("viewHydrogenBonds");
    hbondAction->setCheckable(true);
    hbondAction->setChecked(settings.value("view/hydrogen_bonds", false).toBool());
    hbondAction->setToolTip(
        tr("N・O・Fに結合したHと、N・O・Fの間（H···Y 1.5〜2.6 Å、X–H···Y 120°以上）を破線で表示します。"));
    view->setHydrogenBonds(hbondAction->isChecked());
    connect(hbondAction, &QAction::toggled, this, [this](bool on) {
        view->setHydrogenBonds(on);
        settings.setValue("view/hydrogen_bonds", on);
    });
    action(viewMenu, tr("選択中の計測を画面に表示"), QKeySequence("Ctrl+M"), "ruler", [this] {
        if (selected.size() < 2 || selected.size() > 4)
            fail(tr("2〜4原子を順に選択してください（距離・角度・二面角）。"));
        measure(displayDocument(), selected); // Rejects coincident atoms before adding.
        monitors.append(selected);
        view->setMonitors(monitors);
    })->setObjectName("viewAddMonitor");
    action(viewMenu, tr("画面の計測表示を消去"), {}, "eraser", [this] {
        monitors.clear();
        view->setMonitors({});
    })->setObjectName("viewClearMonitors");
    viewMenu->addSeparator();
    local(viewMenu, tr("全体を表示"), QKeySequence(Qt::Key_F), "fit", [this] { view->fit(); }, "fitAction");
    // The segments under the 3D view of "1 分子", with the same names.
    auto *layoutMenu = viewMenu->addMenu(lineIcon("split"), tr("1 分子の並べ方"));
    auto *layoutGroup = new QActionGroup(this);
    for (int i = 0; i < moleculeLayout->count(); ++i) {
        auto *a = layoutMenu->addAction(moleculeLayoutNames().value(i));
        a->setObjectName("moleculeLayoutAction_" + moleculeLayout->itemData(i).toString());
        a->setCheckable(true);
        a->setChecked(moleculeLayout->currentIndex() == i);
        a->setToolTip(moleculeLayout->itemText(i));
        layoutGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] {
            window.showPanel(this);
            moleculeLayout->setCurrentIndex(i);
        });
    }
    layoutMenu->setToolTipsVisible(true);
    connect(moleculeLayout, &QComboBox::currentIndexChanged, layoutMenu,
            [layoutGroup](int i) { layoutGroup->actions().value(i)->setChecked(true); });
    viewControls->menuButton()->setMenu(viewMenu);
}
void MoleculePanel::refresh() {
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
    const auto changes = session.candidate() ? atomChanges(document(), *session.candidate()) : AtomChanges{};
    const auto &addedAtoms = changes.added, &movedAtoms = changes.moved;
    QVector<std::pair<Vec, QString>> removedAtoms;
    for (const auto &a : changes.removed)
        removedAtoms.append({a.position, a.element});
    view->setChanges(removedAtoms, addedAtoms, movedAtoms);
    banner->refresh(session.candidateName(), changes);
    const auto marker = [](const char *color, const QString &mark) {
        return QString("<span style=\"color:%1\"><b>%2</b></span>").arg(color, mark);
    };
    // The same changes listed atom by atom in the "編集" tab, beside the selection.
    candidateCard->setVisible(session.candidate().has_value());
    if (session.candidate()) {
        // The first 8 changes are described; the rest are only counted.
        QStringList lines;
        const int total = changes.removed.size() + addedAtoms.size() + (movedAtoms.isEmpty() ? 0 : 1);
        const auto full = [&] { return lines.size() >= 8; };
        const auto line = [&](const char *color, const QString &mark, const QString &name,
                              const QString &text) {
            lines << QString("%1&nbsp; <b>%2</b>&nbsp; %3")
                         .arg(marker(color, mark), name.toHtmlEscaped(), text.toHtmlEscaped());
        };
        for (const auto &a : changes.removed) {
            if (full())
                break;
            const auto place = atomPlace(document(), a.id);
            line("#d23c3c", "×", atomName(document(), a.id),
                 place.isEmpty() ? tr("削除", "change") : tr("%1　削除").arg(place));
        }
        for (const auto &a : session.candidate()->atoms) {
            if (full())
                break;
            if (addedAtoms.contains(a.id)) {
                QString partner;
                for (const auto &n : session.candidate()->neighbors(a.id))
                    if (document().contains(n)) {
                        partner = atomName(document(), n);
                        break;
                    }
                line("#25a35a", "○", a.element,
                     partner.isEmpty() ? tr("追加", "change") : tr("%1 に結合　追加").arg(partner));
            }
        }
        if (!movedAtoms.isEmpty() && !full())
            line("#e08a1e", "○", tr("%n 原子", nullptr, movedAtoms.size()), tr("移動", "change"));
        if (total > lines.size())
            lines << tr("ほか %n 件", nullptr, total - int(lines.size()));
        candidateChanges->setText(lines.join("<br>"));
    }
    topRow->setCurrentIndex(session.candidate() ? 1 : 0);
    QStringList seq, plain;
    residueList->clear();
    // Residue links colored in the markup, in the accent rather than the theme's link color.
    const auto link = ChromeColors(QWidget::palette()).accentText.name();
    // N → C by chain and number: caps added after the chain was built (ACE 0, NME n+1) take their place.
    auto residues = d.residues;
    QStringList chainOrder;
    for (const auto &r : std::as_const(residues))
        if (!chainOrder.contains(r.chain))
            chainOrder << r.chain;
    std::stable_sort(residues.begin(), residues.end(), [&](const Residue &a, const Residue &b) {
        const auto ca = chainOrder.indexOf(a.chain), cb = chainOrder.indexOf(b.chain);
        return ca != cb ? ca < cb : a.number < b.number;
    });
    for (const auto &r : std::as_const(residues)) {
        QString label = r.code + " " + QString::number(r.number);
        seq << QString("<a href=\"%1\"><span style=\"color:%3\">%2</span></a>")
                   .arg(r.id.toHtmlEscaped(), label.toHtmlEscaped(), link);
        plain << label;
        // The free ends of the chain, in the words of the "N末端" and "C末端" choices of this page.
        QStringList ends;
        const auto n = r.nTerminal, c = r.cTerminal;
        if (n == "charged" || n == "neutral" || n == "ACE")
            ends << tr("N末端 %1").arg(n == "charged" ? tr("プロトン化") : n == "neutral" ? tr("非荷電") : n);
        if (c == "charged" || c == "neutral" || c == "NME")
            ends << tr("C末端 %1")
                        .arg(c == "charged"   ? QString("COO−")
                             : c == "neutral" ? QString("COOH")
                                              : c);
        auto *item =
            new QListWidgetItem(ends.isEmpty() ? label : label + " · " + ends.join(" · "), residueList);
        item->setData(Qt::UserRole, r.id);
    }
    // Long chains show their first residues; the "残基" tab lists them all.
    if (const int count = seq.size(); count > 24) {
        seq = seq.mid(0, 24) << tr("…（全 %1 残基）").arg(count);
        plain = plain.mid(0, 24) << seq.last();
    }
    sequenceLabel->setText(seq.isEmpty() ? QString() : tr("配列 N → C　%1").arg(seq.join(" · ")));
    // One line when the view is wide enough, wrapping below the view controls otherwise.
    const auto margins = sequenceChip->layout()->contentsMargins();
    sequenceChip->setMaximumWidth(
        sequenceLabel->fontMetrics().horizontalAdvance(tr("配列 N → C　%1").arg(plain.join(" · "))) +
        margins.left() + margins.right() + 8);
    sequenceLabel->setToolTip(seq.isEmpty() ? QString()
                                            : tr("残基をクリックすると、その残基の原子を選択します。"));
    sequenceChip->setVisible(!seq.isEmpty());
    QString current = pointList->currentItem() ? pointList->currentItem()->data(Qt::UserRole).toString() : "";
    pointList->clear();
    int index = 0;
    for (const auto &q : d.pointCharges) {
        auto *item = new QListWidgetItem(QString("%1: %2 e  (%3, %4, %5)")
                                             .arg(++index)
                                             .arg(q.charge)
                                             .arg(q.position.x(), 0, 'f', 2)
                                             .arg(q.position.y(), 0, 'f', 2)
                                             .arg(q.position.z(), 0, 'f', 2),
                                         pointList);
        item->setData(Qt::UserRole, q.id);
        if (q.id == current)
            pointList->setCurrentItem(item);
    }
    pointList->setVisible(index > 0);
    pointEmpty->setVisible(index == 0);
    refreshChargePanel();
    refreshing = false;
    refreshSelection();
}
void MoleculePanel::showProperties(const QString &title) {
    for (int i = 0; i < properties->count(); ++i)
        if (properties->tabText(i) == title)
            properties->setCurrentIndex(i);
}

void MoleculePanel::refreshPalette() {
    QString cat = paletteCategory(), needle = search->text().trimmed();
    periodicTableButton->setVisible(cat == "all" || cat == "atom" || cat == "center");
    // The element the atom tool places (under "配位中心", the center element) is marked among the
    // common ones.
    for (auto *b : findChild<QWidget *>("quickElements")->findChildren<QToolButton *>())
        b->setChecked(b->text() == (cat == "center" ? centerElement : paletteElement));
    search->setPlaceholderText(cat == "atom"     ? tr("周期表で元素記号・名前・番号を検索")
                               : cat == "center" ? tr("元素記号・名前・原子番号を検索")
                                                 : tr("名前・元素・SMILESを検索"));
    growth->setVisible(cat == "amino");
    myPartsBar->setVisible(cat == "my");
    myPartsEmpty->setVisible(cat == "my" && customParts.isEmpty());
    if (cat != "all" && cat != "amino")
        growth->setChecked(false);
    QString old = palette->currentItem() ? palette->currentItem()->data(Qt::UserRole).toString() : "";
    palette->clear();
    // The tooltip names the item in the interface language (tip, else the search text); searches
    // find the names in either language.
    auto add = [&](const QString &label, const QString &id, const QString &searchText,
                   const QJsonObject &t = QJsonObject(), const QString &tip = QString()) {
        // Atoms (and centers under "配位中心") are chosen below by symbol, name or number; the other
        // parts by their text.
        const auto entry = paletteEntry(id, t);
        if (entry.kind != PaletteKind::Atom && entry.kind != PaletteKind::Smiles &&
            !(cat == "center" && entry.kind == PaletteKind::Center) && !needle.isEmpty() &&
            !searchText.contains(needle, Qt::CaseInsensitive))
            return;
        auto *item = new QListWidgetItem(label, palette);
        item->setData(Qt::UserRole, id);
        if (!t.isEmpty())
            item->setData(TemplateDelegate::TemplateRole, t);
        if (entry.kind == PaletteKind::Atom)
            item->setData(TemplateDelegate::ElementRole, entry.value);
        else if (entry.kind == PaletteKind::Center) {
            const auto &e = element(entry.value);
            item->setData(TemplateDelegate::ElementRole, e.symbol);
            item->setData(TemplateDelegate::ElementDetailRole, tr("配位中心 · 原子番号 %1").arg(e.number));
        }
        item->setToolTip(tip.isEmpty() ? searchText : tip);
        item->setData(Qt::AccessibleTextRole, tip.isEmpty() ? searchText : tip);
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
    // The element with the searched symbol or number first, then those whose name contains it.
    const auto matching = [&](int limit) {
        QStringList symbols;
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
        return symbols.mid(0, limit);
    };
    if (cat == "all" || cat == "atom") {
        // Without a search: the chosen element ("原子" also lists the common ones).
        QStringList symbols;
        if (needle.isEmpty()) {
            symbols << paletteElement;
            if (cat == "atom")
                for (const auto *common : {"H", "C", "N", "O", "F", "P", "S", "Cl", "Br", "I"})
                    if (!symbols.contains(common))
                        symbols << common;
        } else
            symbols = matching(cat == "atom" ? 40 : 6);
        heading(tr("原子（元素はH〜Iのボタン・周期表で変更）"));
        for (const auto &symbol : std::as_const(symbols)) {
            const auto &e = element(symbol);
            // Japanese names the element in English too.
            const auto names = english() ? e.nameEn : QString("%1  %2").arg(e.nameJa, e.nameEn);
            add(e.name(), paletteKey(PaletteKind::Atom, e.symbol),
                tr("%1  %2\n原子番号 %3").arg(e.symbol, names).arg(e.number));
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
            const auto tip = english() ? QString("%1  %2 %3")
                                             .arg(t["name_en"].toString(), t["smiles"].toString(),
                                                  idsFromJson(t["aliases"]).join(' '))
                                       : QString();
            QString label = templateName(t);
            if (label.isEmpty())
                label = t["name_en"].toString(); // A part named in English only.
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
                        states << tr("中性");
                    label = tr("%1（%2）").arg(label, states.join(", "));
                }
            }
            if (first)
                heading(title);
            first = false;
            add(label, t["id"].toString(), text, t, tip);
        }
    };
    const auto builtIn = [](const QJsonObject &t, const QStringList &categories) {
        return !t["id"].toString().startsWith("my:") && categories.contains(t["category"].toString());
    };
    for (const auto &[group, title] : {std::pair{"skeleton", tr("骨格")},
                                       {"substituent", tr("置換基")},
                                       {"ligand", tr("配位子")},
                                       {"amino", tr("アミノ酸・末端キャップ")}})
        if (cat == "all" || cat == group)
            addTemplates(
                [&, group = QString(group)](const QJsonObject &t) {
                    return builtIn(t, group == "amino" && cat == "all" ? QStringList{"amino", "cap"}
                                                                       : QStringList{group});
                },
                title);
    if (cat == "all" || cat == "center") {
        heading(tr("配位中心（金属）"));
        const QStringList metals{"Li", "Na", "Mg", "Al", "K",  "Ca", "Ti", "V",  "Cr", "Mn", "Fe", "Co", "Ni",
                                 "Cu", "Zn", "Ru", "Rh", "Pd", "Ag", "Cd", "Pt", "Au", "La", "Ce", "Gd"};
        // "配位中心" also takes any element as the center: the one chosen with the element buttons or
        // the periodic table first, and with a search every element it names.
        QStringList symbols = metals;
        if (cat == "center" && !needle.isEmpty())
            symbols = matching(40);
        else if (cat == "center" && !centerElement.isEmpty() && !metals.contains(centerElement))
            symbols.prepend(centerElement);
        for (const auto &s : std::as_const(symbols)) {
            const auto &e = element(s);
            const auto names = english() ? e.nameEn : QString("%1  %2").arg(e.nameJa, e.nameEn);
            const auto text =
                metals.contains(s)
                    ? tr("%1  %2\n金属 配位中心 · 原子番号 %3").arg(e.symbol, names).arg(e.number)
                    : tr("%1  %2\n配位中心 · 原子番号 %3").arg(e.symbol, names).arg(e.number);
            add(e.name(), paletteKey(PaletteKind::Center, s), english() ? text + "\n" + e.nameJa : text, {},
                text);
        }
    }
    if (cat == "all" || cat == "my")
        addTemplates([](const QJsonObject &t) { return t["id"].toString().startsWith("my:"); },
                     tr("マイ部品"));
    // A search that names no part may be a SMILES: offer to build it (a fragment beside the molecule).
    static const QRegularExpression smilesText(R"(^[A-Za-z0-9@+\-\[\]()=#$/\\%.:*]+$)");
    if (cat == "all" && !palette->count() && smilesText.match(needle).hasMatch())
        add(tr("SMILES「%1」から3Dで作成").arg(needle), paletteKey(PaletteKind::Smiles, needle),
            tr("SMILESとして読み込み、今の分子の横に置く候補を作ります。"));
    if (!palette->currentItem())
        selectFirstPart();
    // The highlighted atom or center is the element a click places, also after a category change.
    if (const auto entry = paletteChoice(); palette->currentItem() && elementBox && entry.element())
        elementBox->setCurrentIndex(elementBox->findData(entry.value));
}

void MoleculePanel::selectFirstPart() {
    for (int i = 0; i < palette->count(); ++i)
        if (palette->item(i)->flags() & Qt::ItemIsSelectable) {
            palette->setCurrentRow(i);
            return;
        }
}

QString MoleculePanel::paletteCategory() const {
    return categoryGroup->checkedButton() ? categoryGroup->checkedButton()->property("category").toString()
                                          : QString("all");
}

void MoleculePanel::setPaletteCategory(const QString &code) {
    for (auto *b : categoryGroup->buttons())
        if (b->property("category") == code)
            b->setChecked(true);
}

void MoleculePanel::showPalette(bool show) {
    paletteColumn->setVisible(show);
    paletteToggle->setChecked(show);
    settings.setValue("molecule/palette", show);
}

QString MoleculePanel::selectionUnit() const {
    return unitGroup->checkedButton() ? unitGroup->checkedButton()->property("unit").toString()
                                      : QString("atom");
}

void MoleculePanel::chooseElement(const QString &symbol) {
    // Under "配位中心" the element buttons and the periodic table choose the center element; from the
    // other part categories they go to "原子", whose list holds the element.
    if (!QStringList{"all", "atom", "center"}.contains(paletteCategory()))
        setPaletteCategory("atom");
    const bool center = paletteCategory() == "center";
    (center ? centerElement : paletteElement) = element(symbol).symbol;
    search->clear();
    refreshPalette();
    for (int i = 0; i < palette->count(); ++i)
        if (palette->item(i)->data(Qt::UserRole) ==
            paletteKey(center ? PaletteKind::Center : PaletteKind::Atom, element(symbol).symbol))
            palette->setCurrentRow(i);
    paletteChosen();
}

QJsonObject MoleculePanel::currentTemplate() const {
    if (!palette->currentItem())
        fail(tr("左のパレットから部品を選択してください。"));
    QString id = palette->currentItem()->data(Qt::UserRole).toString();
    for (auto t : customParts)
        if (t.toObject()["id"] == id)
            return t.toObject();
    return findTemplate(id);
}

void MoleculePanel::paletteChosen() {
    if (!palette->currentItem())
        return;
    const auto entry = paletteChoice();
    if (entry.kind == PaletteKind::Smiles) {
        search->clear();
        runWorker(document().atoms.isEmpty() ? "smiles" : "fragment", entry.value);
        return;
    }
    if (entry.element())
        elementBox->setCurrentIndex(elementBox->findData(entry.value));
    if (entry.kind == PaletteKind::Atom)
        paletteElement = entry.value;
    else if (entry.kind == PaletteKind::Center) {
        centerElement = entry.value;
        showProperties(tr("配位"));
    } else if (entry.kind == PaletteKind::Amino) {
        showProperties(tr("残基"));
        if (growth->isChecked() && !growthResidue.isEmpty() && !session.candidate())
            addAmino(growthResidue, growthN);
    }
    // The replace tool keeps working with whatever is chosen: it replaces atoms with that part.
    if (tool() != EditorTool::Replace)
        setTool(EditorTool::Place);
    refreshToolHint();
    view->setAminoHandles(entry.kind == PaletteKind::Amino);
}

bool MoleculePanel::hasSelection(int count) {
    if (!selected.isEmpty() && (!count || selected.size() == count))
        return true;
    statusBar()->showMessage(
        count ? tr("%n原子を選択してください。", nullptr, count) : tr("原子を選択してください。"), 5000);
    return false;
}

void MoleculePanel::enableWithSelection(QWidget *widget, int minimum, int maximum) {
    selectionControls.append({widget, minimum, maximum, widget->toolTip()});
}

void MoleculePanel::refreshSelectionControls() {
    const int count = selected.size();
    for (const auto &c : std::as_const(selectionControls)) {
        const bool ready = count >= c.minimum && (!c.maximum || count <= c.maximum);
        c.widget->setEnabled(ready);
        const QString need =
            c.minimum == c.maximum ? (c.minimum == 1 ? tr("原子を1つ選択すると使えます。")
                                                     : tr("%1原子を順に選択すると使えます。").arg(c.minimum))
            : c.maximum            ? tr("%1〜%2原子を順に選択すると使えます。").arg(c.minimum).arg(c.maximum)
                                   : tr("原子を選択すると使えます。");
        c.widget->setToolTip(ready ? c.tooltip : c.tooltip.isEmpty() ? need : c.tooltip + "\n" + need);
    }
}

Ids MoleculePanel::requireSelection(int count) const {
    if (selected.isEmpty() || (count && selected.size() != count))
        fail(count ? tr("%n原子を選択してください。", nullptr, count) : tr("原子を選択してください。"));
    return selected;
}

Ids MoleculePanel::expandedSelection(const Ids &ids) const {
    const auto &d = displayDocument();
    QString mode = selectionUnit();
    if (mode == "atom" || mode == "bond" || ids.isEmpty())
        return ids;
    if (mode == "all")
        return d.ids();
    // The unit of each atom is looked up once per call, and a unit already taken adds nothing.
    std::optional<Topology> topology;
    if (mode == "component")
        topology.emplace(d);
    QHash<QString, int> unitOf; // The residue or part holding each atom: the first one.
    if (mode == "residue")
        for (int r = d.residues.size() - 1; r >= 0; --r)
            for (const auto &atom : d.residues[r].atoms)
                unitOf.insert(atom, r);
    if (mode == "part")
        for (int p = d.parts.size() - 1; p >= 0; --p)
            for (const auto &atom : d.parts[p].atoms)
                unitOf.insert(atom, p);
    Ids out;
    QSet<QString> listed;
    QSet<int> units;
    for (const auto &id : ids) {
        Ids group;
        if (mode == "component") {
            const int i = topology->index(id);
            if (i < 0)
                fail(tr("原子を選択してください。"));
            if (listed.contains(id))
                continue;
            for (int j : topology->component(i))
                group << d.atoms[j].id;
        } else if (const int unit = unitOf.value(id, -1); unit >= 0) {
            if (units.contains(unit))
                continue;
            units.insert(unit);
            group = mode == "residue" ? d.residues[unit].atoms.values() : d.parts[unit].atoms;
        }
        if (group.isEmpty())
            group << id;
        for (const auto &a : group)
            if (!listed.contains(a)) {
                listed.insert(a);
                out << a;
            }
    }
    return out;
}

void MoleculePanel::setSelection(const Ids &ids) {
    const auto present = displayDocument().ids();
    const QSet<QString> atoms(present.begin(), present.end());
    selected = ids;
    selected.removeIf([&](const auto &id) { return !atoms.contains(id); });
    view->setSelection(selected);
    refreshSelection();
}

void MoleculePanel::atomClicked(const QString &id, Qt::KeyboardModifiers mods) {
    const auto mode = tool();
    if (mode == EditorTool::Select || mode == EditorTool::Move) {
        auto group = expandedSelection({id});
        // A click selects; Shift/Ctrl+click adds or removes, keeping the order for measurements.
        if (!(mods & (Qt::ShiftModifier | Qt::ControlModifier))) {
            setSelection(group);
            return;
        }
        auto next = selected;
        if (next.contains(id)) {
            const QSet<QString> removed(group.begin(), group.end());
            next.removeIf([&](const QString &a) { return removed.contains(a); });
        } else {
            QSet<QString> listed(next.begin(), next.end());
            for (const auto &a : group)
                if (!listed.contains(a)) {
                    listed.insert(a);
                    next << a;
                }
        }
        setSelection(next);
    } else if (mode == EditorTool::Bond) {
        if (selected.size() != 1) {
            setSelection({id});
            return;
        }
        bondAtoms(selected.first(), id, value(bondKind));
    } else {
        place(id, document().atom(id).position);
        if (mods.testFlag(Qt::ShiftModifier))
            return;
    }
}

void MoleculePanel::place(const QString &id, const Vec &position) {
    const auto mode = tool();
    const auto entry = paletteChoice();
    auto symbol = value(elementBox), kind = value(bondKind);
    bool hydrogen = autoH->isChecked();
    if (mode == EditorTool::Charge) {
        double q = chargeValue->value();
        edit(tr("点電荷を配置"), [=](Document &d) { addPointCharge(d, position, q); });
    } else if (mode == EditorTool::Delete) {
        if (id.isEmpty())
            return;
        deleteAtoms(expandedSelection({id}));
    } else if (mode != EditorTool::Place && mode != EditorTool::Replace)
        return;
    else if (entry.kind == PaletteKind::Smiles)
        fail(tr("SMILESの項目は、パレットでクリックすると3D構造を作ります。"));
    else if (mode == EditorTool::Replace) {
        if (id.isEmpty())
            return;
        if (entry.part()) {
            auto ids = expandedSelection({id});
            auto t = currentTemplate();
            double angle = twist->value();
            edit(tr("部品を置換"), [=](Document &d) { replacePart(d, ids, t, angle); }, true);
        } else
            replaceElements(expandedSelection({id}), symbol);
    } else if (entry.kind == PaletteKind::Atom)
        edit(tr("原子を追加"), [=](Document &d) { addAtom(d, symbol, position, id, kind, hydrogen); });
    else if (entry.kind == PaletteKind::Center) {
        QString g = value(geometryBox);
        double dist = distance->value();
        int ox = oxidation->value();
        edit(tr("配位中心を配置"), [=](Document &d) { addCenter(d, symbol, g, position, dist, ox); });
    } else if (entry.kind == PaletteKind::Ligand && !id.isEmpty())
        fail(tr(
            "配位中心の紫の番号（空きサイト）をクリックしてください。空白をクリックすると単独で置きます。"));
    // A ligand clicked on a blank is put down on its own, like a skeleton.
    else {
        auto t = currentTemplate();
        double angle = twist->value();
        if (entry.kind == PaletteKind::Amino) {
            if (!id.isEmpty()) {
                auto rid = residueForAtom(document(), id);
                if (rid.isEmpty())
                    fail(tr("ペプチドの末端の＋を選択してください。"));
                const auto names = residue(document(), rid).atoms;
                QString parent = id;
                if (document().atom(id).element == "H" && document().neighbors(id).size() == 1)
                    parent = document().neighbors(id).first();
                if (parent == names.value("N"))
                    addAmino(rid, true);
                else if (parent == names.value("C") || parent == names.value("OXT"))
                    addAmino(rid, false);
                else
                    fail(tr("N末端またはC末端を選択してください。"));
                return;
            }
            QString code = t["code"].toString(), state = t["state"].toString("default"),
                    ch = value(chiralityBox), nt = value(nTerm), ct = value(cTerm);
            edit(tr("アミノ酸を配置"), [=](Document &d) {
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
            edit(tr("置換基を接続"), [=](Document &d) { attachPart(d, id, t, angle); });
        else if (entry.kind == PaletteKind::Substituent)
            fail(tr("置換する水素へクリックまたはドロップしてください。"));
        else
            edit(tr("部品を配置"),
                 [=](Document &d) { placeTemplate(d, t, d.atoms.isEmpty() ? Vec::Zero() : position); });
        if (!keepTool->isChecked())
            setTool(EditorTool::Select);
    }
    view->keepInView();
}

void MoleculePanel::applyMoleculeLayout() {
    const auto layout = moleculeLayout->currentData().toString();
    moleculeSplit->widget(0)->setVisible(layout != "table");
    coordinates->setVisible(layout != "view");
    // Both shown: the border where it was dragged, else 3D:表 = 6:4 (the table keeps four rows).
    // Queued after the splitter has taken in the table just shown, which would size it otherwise;
    // the selected rows are brought into view once the table has that size.
    if (layout != "view")
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (moleculeLayout->currentData() == "both" &&
                    !moleculeSplit->restoreState(settings.value("molecule/split").toByteArray()))
                    moleculeSplit->setSizes({600, 400});
                coordinates->refreshSelection();
            },
            Qt::QueuedConnection);
    // The hints are about the view.
    for (auto *hint : {static_cast<QWidget *>(mouseHint), findChild<QWidget *>("controlsHint")})
        hint->setVisible(layout != "table");
    // The table edits the coordinates of "座標（1原子）", and measures once the view is hidden.
    if (positionCard) // Built after the layout is first applied.
        positionCard->setVisible(layout == "view");
    coordinates->refreshSelection();
}

void MoleculePanel::showCoordinateTable() {
    window.showPanel(this);
    if (moleculeLayout->currentData() == "view")
        moleculeLayout->setCurrentIndex(moleculeLayout->findData("both"));
}

void MoleculePanel::refreshSelection() {
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
        measureTitle->setText(QStringList{tr("距離"), tr("角度"), tr("二面角")}[count - 2]);
        dimensionLabel->setText(QStringList{tr("距離 / Å"), tr("角度 / °"), tr("二面角 / °")}[count - 2]);
        measurement->setText(labels.join(" – ") + " = " + value);
    }
    measureCard->setVisible(measuring);
    // The selection in click order at the top of the "編集" tab.
    selectionTitle->setText(shown.isEmpty() ? tr("選択なし") : tr("選択 %n 原子", nullptr, count));
    if (shown.isEmpty())
        selectionList->setText(tr("3D画面で原子をクリックして選択します。Shift／Ctrl＋クリックで順に追加し、"
                                  "2〜4原子で距離・角度・二面角を測定します。"));
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
            rows << tr("ほか %n 原子", nullptr, count - 8);
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
        mass->setPlaceholderText(tr("整数: 質量数 / 小数: amu"));
    } else {
        for (auto *field : positionFields)
            field->setValue(field->minimum());
        formal->setValue(MinimumAtomCharge - 1);
        radical->setValue(-1);
        mass->clear();
        mass->setPlaceholderText(selected.isEmpty() ? tr("原子を選択してください")
                                                    : tr("混在（空欄は変更なし）"));
    }
    refreshSelectionControls();
    if (selected.size() == 2)
        if (auto b = d.bond(selected[0], selected[1]))
            bondKind->setCurrentIndex(bondKind->findData(b->kind));
    if (coordinates)
        coordinates->refreshSelection();
}

QString MoleculePanel::askSmiles(const QString &title, const QString &label, const QString &initial) {
    QInputDialog dialog(this);
    dialog.setObjectName("smilesDialog");
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextValue(initial);
    dialog.setOkButtonText(tr("3Dで作成"));
    dialog.resize(560, dialog.sizeHint().height());
    return dialog.exec() == QDialog::Accepted ? dialog.textValue().trimmed() : QString();
}

void MoleculePanel::addFragmentFromSmiles(const QString &initial) {
    const auto smiles = askSmiles(tr("SMILESから断片を追加"),
                                  tr("追加する分子のSMILES（例: CCO＝エタノール、c1ccccc1＝ベンゼン）\n"
                                     "今の分子の+X側に置いた候補を作ります。確認してから確定してください。"),
                                  initial);
    if (!smiles.isEmpty())
        runWorker("fragment", smiles);
}

void MoleculePanel::addFragmentFromFile() {
    const auto p = QFileDialog::getOpenFileName(
        this, tr("断片を追加"), settings.value("directory").toString(),
        tr("構造 (*.xyz *.mol *.sdf *.mol2 *.cml *.pdb *.ent *.nw *.gjf *.com)"));
    if (p.isEmpty())
        return;
    checkMcpPath(p);
    const auto base = session.revision();
    const auto options = chooseStructureImport(this, p);
    if (!options)
        return;
    if (base != session.revision())
        fail(tr("読込中に分子が変更されました。読込をやり直してください。"));
    const auto fragment = importStructure(p, options->model, options->altloc, options->frame);
    edit(tr("ファイルから断片を追加"), [&](Document &d) { insertFragment(d, fragment); }, true);
    view->keepInView();
}

void MoleculePanel::importSmiles(const QString &initial) {
    const auto smiles =
        askSmiles(tr("SMILESから作成"),
                  tr("SMILES（例: CCO＝エタノール、c1ccccc1＝ベンゼン、[NH4+]＝アンモニウム）\n"
                     "今の分子を置き換える候補を作ります。確認してから確定してください。"),
                  initial);
    if (!smiles.isEmpty())
        runWorker("smiles", smiles);
}

void MoleculePanel::deleteAtoms(const Ids &chosen) {
    const bool terminate = deleteTerminate && deleteTerminate->isChecked();
    edit(
        tr("選択原子を削除"),
        [=](Document &d) {
            mf::deleteAtoms(d, chosen, {.includeAttachedHydrogens = true, .capCutBonds = terminate});
        },
        true);
}

void MoleculePanel::proposeLigand(const QString &center, const QList<int> &sites, const QJsonObject &t,
                                  double twist) {
    edit(tr("配位子を配置"), [=](Document &d) { placeLigand(d, center, sites, t, twist); }, true);
    ligandCandidate = session.candidateSerial();
}

void MoleculePanel::bondAtoms(const QString &a, const QString &b, const QString &kind) {
    const bool hydrogen = autoH->isChecked();
    edit(tr("結合を変更"), [=](Document &d) { changeBond(d, a, b, kind, hydrogen); });
}

void MoleculePanel::replaceElements(const Ids &ids, const QString &symbol) {
    const bool hydrogen = autoH->isChecked();
    edit(tr("元素を変更"), [=](Document &d) { replaceElement(d, ids, symbol, hydrogen); });
}

void MoleculePanel::fixAtoms(const Ids &ids, bool fixed) {
    AtomProperties properties;
    properties.fixed = fixed;
    edit(fixed ? tr("選択原子を固定") : tr("選択原子の固定を解除"),
         [=](Document &d) { setAtomProperties(d, ids, properties); });
}

void MoleculePanel::selectAll() {
    setSelection(displayDocument().ids());
}

void MoleculePanel::copySelection() {
    if (!resolveCandidate())
        return;
    Document part = document();
    if (!selected.isEmpty()) {
        Ids others;
        for (const auto &a : document().atoms)
            if (!selected.contains(a.id))
                others << a.id;
        for (auto &a : part.atoms)
            a.fixed = false; // Only the copy loses atoms; fixing applies to the document.
        part.remove(others);
    }
    QApplication::clipboard()->setText(part.xyz());
    statusBar()->showMessage(tr("%n原子をXYZでコピーしました。", nullptr, part.atoms.size()), 5000);
}

void MoleculePanel::pasteFragment() {
    const auto text = QApplication::clipboard()->text();
    if (text.trimmed().isEmpty())
        fail(tr("クリップボードにXYZがありません。"));
    const auto fragment = xyzStructure(text);
    edit(tr("XYZを断片として貼り付け"), [&](Document &d) { insertFragment(d, fragment); }, true);
    view->keepInView();
}

EditorTool MoleculePanel::tool() const {
    return toolGroup->checkedId() < 0 ? EditorTool::Select : EditorTool(toolGroup->checkedId());
}

void MoleculePanel::setTool(EditorTool tool) {
    toolGroup->button(int(tool))->setChecked(true);
}

PaletteEntry MoleculePanel::paletteChoice() const {
    const auto *item = palette->currentItem();
    return item ? paletteEntry(item->data(Qt::UserRole).toString(),
                               item->data(TemplateDelegate::TemplateRole).toJsonObject())
                : PaletteEntry();
}

void MoleculePanel::refreshToolHint() {
    if (!mouseHint || !palette)
        return;
    // What a click does with the current tool, naming the element or part it places.
    const auto mode = tool();
    const auto entry = paletteChoice();
    const auto part = palette->currentItem() ? palette->currentItem()->text() : tr("パレットで選択");
    const auto symbol = value(elementBox);
    QString hint;
    switch (mode) {
    case EditorTool::Select:
        hint =
            tr("クリック: 選択 · Shift／Ctrl＋クリック: 追加・解除（順番を保持） · Shiftドラッグ: 範囲選択");
        break;
    case EditorTool::Move:
        hint = tr("移動: 原子をドラッグ（付いている水素と、選択中なら選択全体も動く） · Altドラッグ: 回転");
        break;
    case EditorTool::Bond:
        hint = tr("結合（%1）: 2原子を順にクリック · 種類は3D画面の上の「結合の種類」で選択")
                   .arg(bondKind->currentText());
        break;
    case EditorTool::Delete:
        hint = tr("削除: 原子をクリックすると削除の候補を作ります");
        break;
    case EditorTool::Replace:
        hint = tr("置き換え（%1）: 置き換える原子をクリック").arg(palette->currentItem() ? part : symbol);
        break;
    case EditorTool::Charge:
        hint = tr("点電荷: 空白をクリックで配置 · 電荷の値は「電荷」ページ");
        break;
    case EditorTool::Place:
        switch (entry.kind) {
        case PaletteKind::Atom:
            hint = tr("配置（%1）: 空白をクリックで配置 · 原子をクリックで結合した%1を追加 · "
                      "結合をクリックで間に挿入")
                       .arg(symbol);
            break;
        case PaletteKind::Center:
            hint = tr("配置（配位中心 %1）: 空白をクリックで配置 · 配位形状は「配位」ページ").arg(symbol);
            break;
        case PaletteKind::Smiles:
            hint = tr("配置: パレットの「%1」をクリックすると3D構造を作ります").arg(part);
            break;
        case PaletteKind::Substituent:
            hint = tr("配置（%1）: 置き換える水素をクリック（ドロップも可）").arg(part);
            break;
        case PaletteKind::Ligand:
            hint =
                tr("配置（%1）: 配位中心の紫の番号（空きサイト）をクリック · 空白なら単独で配置").arg(part);
            break;
        case PaletteKind::Part:
        case PaletteKind::Amino:
            hint = tr("配置（%1）: 空白をクリックで配置 · 原子・水素をクリックで接続").arg(part);
            break;
        }
        break;
    }
    // Delete and replace act on the selection unit too, which is set with the select tool.
    if ((mode == EditorTool::Delete || mode == EditorTool::Replace) && selectionUnit() != "atom")
        hint = tr("%1（選択単位: %2）").arg(hint, unitGroup->checkedButton()->text());
    mouseHint->setText(hint);
    mouseHint->setToolTip(hint);
    // The option bar shows what the tool uses: the selection unit for the tools that pick atoms,
    // the bond kind for bonds and atoms put on atoms, and what the palette has chosen.
    const bool placing = mode == EditorTool::Place;
    const QHash<QString, bool> shown{
        {"selectionUnitOption", mode == EditorTool::Select || mode == EditorTool::Move},
        {"partOption", placing || mode == EditorTool::Replace},
        {"bondOption", mode == EditorTool::Bond || (placing && entry.kind == PaletteKind::Atom)},
        {"keepToolOption", placing && entry.part()},
        {"deleteOption", mode == EditorTool::Delete},
        {"chargeOption", mode == EditorTool::Charge}};
    for (auto it = shown.begin(); it != shown.end(); ++it)
        if (auto *group = toolOptionGroups.value(it.key()))
            group->setVisible(it.value());
    const auto &e = element(symbol);
    const auto chosen = mode == EditorTool::Replace && entry.element() ? e.name()
                        : entry.kind == PaletteKind::Atom   ? tr("%1（%2）").arg(e.name(), e.symbol)
                        : entry.kind == PaletteKind::Center ? tr("配位中心 %1（%2）").arg(e.name(), e.symbol)
                                                            : part;
    toolPart->setText(QString("%1 <b>%2</b>")
                          .arg(mode == EditorTool::Replace ? tr("置き換え先") : tr("配置する部品"),
                               chosen.toHtmlEscaped()));
}

void MoleculePanel::contextMenu(const QPoint &p, const QString &id) {
    if (!id.isEmpty() && !selected.contains(id))
        setSelection(expandedSelection({id}));
    QMenu menu(this);
    const auto add = [&](QMenu *to, const QString &text, const QString &icon, auto fn) {
        auto *a = to->addAction(lineIcon(icon), text);
        connect(a, &QAction::triggered, this, [this, fn] { safe(fn); });
    };
    if (!selected.isEmpty()) {
        add(&menu, tr("選択原子を削除（候補）"), "delete", [this] { deleteAtoms(requireSelection()); });
        add(&menu, tr("元素を %1 に変更").arg(value(elementBox)), "replace",
            [this] { replaceElements(requireSelection(), value(elementBox)); });
        // As the coordinate table: fixed only when every selected atom is, else all of them get fixed.
        const bool allFixed = std::all_of(selected.begin(), selected.end(), [this](const QString &a) {
            return displayDocument().atom(a).fixed;
        });
        add(&menu, allFixed ? tr("固定を解除") : tr("選択原子を固定"), allFixed ? "unlock" : "lock",
            [this, allFixed] { fixAtoms(requireSelection(), !allFixed); });
        if (selected.size() <= 3)
            add(&menu, tr("選択順に向きをそろえる（原点・+X・XY面）"), "axes", [this] {
                auto ids = requireSelection();
                edit(tr("向きをそろえる"), [=](Document &d) { orient(d, ids); });
                view->keepInView();
            });
        if (selected.size() == 2) {
            auto *bonds = menu.addMenu(lineIcon("bond"), tr("2原子の結合"));
            for (const auto &[label, kind, icon] : {std::tuple{tr("単結合"), "single", "bond1"},
                                                    {tr("二重結合"), "double", "bond2"},
                                                    {tr("三重結合"), "triple", "bond3"},
                                                    {tr("芳香族"), "aromatic", "bondAromatic"},
                                                    {tr("配位（供与→中心）"), "dative", "bondDative"},
                                                    {tr("結合を削除"), "none", "delete"}})
                add(bonds, label, icon, [this, kind = QString(kind)] {
                    auto ids = requireSelection(2);
                    bondAtoms(ids[0], ids[1], kind);
                });
        }
        menu.addSeparator();
    }
    add(&menu, tr("全体を表示"), "fit", [this] { view->fit(); });
    menu.exec(p);
}

void MoleculePanel::selectNumbers() {
    bool ok;
    QString s = QInputDialog::getText(this, tr("原子番号で選択"), tr("例: 1,3,5-8（この順で選択）"),
                                      QLineEdit::Normal, "", &ok);
    if (!ok)
        return;
    Ids ids;
    for (auto part : s.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts)) {
        auto range = part.split('-');
        bool good;
        int a = range[0].toInt(&good);
        if (!good)
            fail(tr("原子番号が不正です。"));
        int b = a;
        if (range.size() == 2) {
            b = range[1].toInt(&good);
            if (!good || b < a)
                fail(tr("範囲が不正です。"));
        }
        if (range.size() > 2 || a < 1 || b > displayDocument().atoms.size())
            fail(tr("原子番号が範囲外です。"));
        for (int i = a; i <= b; ++i)
            if (!ids.contains(displayDocument().atoms[i - 1].id))
                ids << displayDocument().atoms[i - 1].id;
    }
    setSelection(ids);
}

void MoleculePanel::showReport() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("構造の確認"));
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

void MoleculePanel::compareDialog() {
    if (!resolveCandidate())
        return;
    const auto p = QFileDialog::getOpenFileName(
        this, tr("比較する構造"), settings.value("directory").toString(),
        tr("構造 (*.molforma.json *.json *.xyz *.mol *.sdf *.mol2 *.cml *.pdb *.ent "
           "*.nw *.gjf *.com);;すべて (*)"));
    if (p.isEmpty())
        return;
    checkMcpPath(p);
    const auto base = session.revision();
    const auto options = chooseStructureImport(this, p);
    if (!options)
        return;
    if (base != session.revision())
        fail(tr("読込中に分子が変更されました。読込をやり直してください。"));
    const auto reference = importStructure(p, options->model, options->altloc, options->frame);
    const auto fit = superpose(document(), reference);
    QMessageBox box(QMessageBox::Information, tr("構造の比較"),
                    tr("%1\n\nRMSD（座標そのまま）: %2 Å\n重ね合わせ後のRMSD: %3 Å（水素以外 %4 Å）\n"
                       "重ね合わせ後の最大のずれ: %5 Å")
                        .arg(QFileInfo(p).fileName())
                        .arg(fit.raw, 0, 'f', 4)
                        .arg(fit.rmsd, 0, 'f', 4)
                        .arg(fit.heavyRmsd, 0, 'f', 4)
                        .arg(fit.maximum, 0, 'f', 4),
                    QMessageBox::Close, this);
    box.setTextFormat(Qt::PlainText); // The file name is shown as written.
    box.setObjectName("compareResult");
    auto *align = box.addButton(tr("比較対象に重ねる（候補）"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() != align)
        return;
    edit(
        tr("比較対象に重ね合わせ"), [&](Document &d) { transform(d, d.ids(), fit.rotation, fit.shift); },
        true);
}

void MoleculePanel::zMatrixDialog() {
    if (!resolveCandidate())
        return;
    QDialog dialog(this);
    dialog.setObjectName("zMatrixDialog");
    dialog.setWindowTitle(tr("Z-matrix（内部座標）"));
    dialog.resize(760, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro =
        new QLabel(tr("原子番号の順に、前の原子を参照して距離・角度・二面角で表します（結合した原子を優先）。"
                      "値を編集すると、その原子の側（結合の先）を動かして「1 分子」に反映します。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *table = new QTableWidget(0, 8);
    table->setObjectName("zMatrixTable");
    table->setHorizontalHeaderLabels({tr("元素"), tr("結合相手"), tr("距離 / Å"), tr("角度相手"),
                                      tr("角度 / °"), tr("二面角相手"), tr("二面角 / °"), ""});
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *copy = buttons->addButton(tr("テキストをコピー"), QDialogButtonBox::ActionRole);
    copy->setObjectName("zMatrixCopy");
    layout->addWidget(buttons);
    QVector<ZMatrixRow> rows;
    bool filling = false;
    const auto fill = [&] {
        QScopedValueRollback guard(filling, true);
        rows = zMatrix(document());
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
                return k >= 0 ? document().atoms[k].element + QString::number(k + 1) : QString();
            };
            cell(0, document().atoms[i].element + QString::number(i + 1), false);
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
        const auto id = [&](int k) { return document().atoms[k].id; };
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
        safe([&] { edit(tr("Z-matrixの値を変更"), [&](Document &d) { setDimension(d, ids, value); }); });
        fill(); // Other references move with the edited side.
    });
    connect(copy, &QPushButton::clicked, &dialog, [&] {
        QApplication::clipboard()->setText(zMatrixText(document()));
        statusBar()->showMessage(tr("Z-matrixをコピーしました。"), 5000);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MoleculePanel::symmetryDialog() {
    QDialog dialog(this);
    dialog.setObjectName("symmetryDialog");
    dialog.setWindowTitle(tr("点群・対称化"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new FormLayout;
    layout->addLayout(form);
    auto *tolerance = new QDoubleSpinBox;
    tolerance->setObjectName("symmetryTolerance");
    tolerance->setRange(.001, .5);
    tolerance->setDecimals(3);
    tolerance->setSingleStep(.01);
    tolerance->setValue(settings.value("symmetry/tolerance", .05).toDouble());
    tolerance->setSuffix(" Å");
    form->addRow(tr("許容差（原子の位置）"), tolerance);
    auto *result = new QLabel;
    result->setObjectName("symmetryResult");
    result->setWordWrap(true);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(result);
    auto *note =
        new QLabel(tr("全原子（ゴースト・同位体は別の種類）で判定します。対称化は対称操作で座標を平均し、"
                      "候補として表示します。NWChemの入力はnoautosymのため、計算では対称性を使いません。"));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *apply = buttons->addButton(tr("対称化（候補にする）"), QDialogButtonBox::AcceptRole);
    apply->setObjectName("symmetryApply");
    layout->addWidget(buttons);
    PointGroup group;
    const auto evaluate = [&] {
        group = pointGroup(displayDocument(), tolerance->value());
        settings.setValue("symmetry/tolerance", tolerance->value());
        if (group.name.isEmpty())
            result->setText(displayDocument().atoms.size() > 200 ? tr("200原子を超える構造は判定しません。")
                                                                 : tr("点群を決められませんでした。"));
        else
            result->setText(
                tr("点群: %1（対称操作 %2、対称な位置からの最大のずれ %3 Å）")
                    .arg(group.name)
                    .arg(group.name.contains("∞") ? QString("∞") : QString::number(group.operations.size()))
                    .arg(group.deviation, 0, 'f', 4));
        apply->setEnabled(!group.name.isEmpty() && group.name != "C1" && group.name != "Kh" &&
                          !session.candidate() && !chemistryRunning());
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
        tr("点群に合わせて対称化"),
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
    statusBar()->showMessage(tr("%1に対称化した候補です（最大の移動 %2 Å）。「変更を確定」で適用します。")
                                 .arg(group.name)
                                 .arg(moved, 0, 'f', 4),
                             15000);
}

void MoleculePanel::savePart() {
    auto ids = requireSelection();
    bool ok;
    QString name = QInputDialog::getText(this, tr("マイ部品"), tr("部品名"), QLineEdit::Normal, "", &ok);
    if (!ok)
        return;
    const QStringList kinds{tr("置換基（接続点1つ）"), tr("配位子"), tr("骨格（接続点なし・空白に配置）")};
    QString kind = QInputDialog::getItem(
        this, tr("部品の種類"), tr("種類（置換基・配位子は接続点の原子を最初に選択）"), kinds, 0, false, &ok);
    if (!ok)
        return;
    auto t = makePart(document(), ids, name,
                      kind == kinds[1]   ? "ligand"
                      : kind == kinds[2] ? "skeleton"
                                         : "substituent");
    QString p = QFileDialog::getSaveFileName(this, tr("部品を保存"), name + ".molforma-part.json",
                                             tr("部品 (*.molforma-part.json)"));
    if (p.isEmpty())
        return;
    writeFile(p, QJsonDocument(t).toJson());
    customParts.append(t);
    settings.setValue("parts", customParts);
    refreshPalette();
}

void MoleculePanel::loadPart() {
    QString p = QFileDialog::getOpenFileName(this, tr("マイ部品を読み込む"), {},
                                             tr("部品 (*.molforma-part.json *.json)"));
    if (!p.isEmpty())
        loadPartFile(p);
}

void MoleculePanel::loadPartFile(const QString &p) {
    auto t = parseJson(readFile(p));
    validateTemplate(t);
    if (!t["id"].toString().startsWith("my:"))
        fail(tr("マイ部品のIDはmy:で始まる必要があります。"));
    for (int i = customParts.size() - 1; i >= 0; --i)
        if (customParts[i].toObject()["id"] == t["id"])
            customParts.removeAt(i);
    customParts.append(t);
    settings.setValue("parts", customParts);
    setPaletteCategory("my");
    showPalette(true);
    refreshPalette();
}

void MoleculePanel::applyChromeStyle() {
    const auto style = [&](QWidget *w, const QString &rules) {
        if (w)
            w->setStyleSheet(chromeRules(rules, QWidget::palette()));
    };
    style(findChild<QWidget *>("toolRail"), R"(
        #toolRail { background: @base; border-right: 1px solid @line; }
        QToolButton { border: none; border-radius: 8px; color: @text; padding-top: 3px; }
        QToolButton:hover { background: @hover; }
        QToolButton:checked { background: @tint; color: @accentText; font-weight: bold; }
    )");
    style(findChild<QWidget *>("paletteColumn"), R"(
        #paletteColumn { background: @base; }
        QLabel#myPartsEmpty { color: @muted; padding: 4px 2px; }
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
    banner->applyChromeStyle();
    const QString overlay = R"(
        #sequenceChip, #measureCard { background: @base; border: 1px solid @line; border-radius: 8px; }
        QToolButton { border: none; border-radius: 5px; padding: 3px 7px; color: @text; }
        QToolButton:hover { background: @hover; }
        QLabel#measurementLabel { color: @text; }
    )";
    for (const auto *name : {"sequenceChip", "measureCard"})
        style(findChild<QWidget *>(name), overlay);
    viewControls->applyStyle(QWidget::palette());
    style(findChild<QWidget *>("hintBar"), R"(
        #hintBar { background: @base; border-top: 1px solid @line; }
        QLabel { color: @muted; }
        QLabel#toolHint { color: @text; }
        QFrame[choice="segments"] { background: @segment; border: none; border-radius: 7px; }
        QFrame[choice="segments"] QToolButton { border: none; border-radius: 5px; padding: 3px 10px; color: @muted;
                                                background: transparent; }
        QFrame[choice="segments"] QToolButton:hover { color: @text; }
        QFrame[choice="segments"] QToolButton:checked { background: @base; color: @accentText; font-weight: bold; }
    )");
    // The pages of properties: the cards and buttons of the settings tabs.
    style(propertyColumn, settingsRules() + R"(
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
}
void MoleculePanel::pruneSelection(const Document &document) {
    const auto ids = document.ids();
    const QSet<QString> present(ids.begin(), ids.end());
    selected.removeIf([&](const auto &id) { return !present.contains(id); });
}
bool MoleculePanel::keepsSelection() const {
    return keepSelection->isChecked();
}
void MoleculePanel::cancelGrowth() {
    growth->setChecked(false);
    growthResidue.clear();
}
void MoleculePanel::stopEditing() {
    // Focus-out signals during the owner's destruction must not access its session or settings.
    for (auto *child : findChildren<QObject *>())
        child->disconnect(this);
    propertyColumn->removeEventFilter(this);
    disconnect(this, nullptr, &window, nullptr);
}
void MoleculePanel::savePreferences() {
    settings.setValue("keepSelection", keepSelection->isChecked());
    settings.setValue("keepTool", keepTool->isChecked());
    settings.setValue("parts", customParts);
}
void MoleculePanel::setWorkerRunning(bool running) {
    cancelWorkerButton->setVisible(running);
}
QString MoleculePanel::layoutMode() const {
    return moleculeLayout->currentData().toString();
}
QRect MoleculePanel::propertiesArea(const QWidget *target) const {
    return propertyColumn->isVisible()
               ? QRect(propertyColumn->mapTo(target, QPoint()), propertyColumn->size())
               : QRect();
}
bool MoleculePanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == propertyColumn && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        emit geometryChanged();
    return QWidget::eventFilter(watched, event);
}
void MoleculePanel::edit(const QString &name, const std::function<void(Document &)> &operation,
                         bool preview) {
    window.edit(name, operation, preview);
}
void MoleculePanel::safe(const std::function<void()> &operation) {
    window.safe(operation);
}
QStatusBar *MoleculePanel::statusBar() const {
    return window.statusBar();
}
bool MoleculePanel::chemistryRunning() const {
    return window.chemistryRunning();
}
void MoleculePanel::runWorker(const QString &mode, const QString &smiles, const Ids &moving, int count) {
    window.runWorker(mode, smiles, moving, count);
}
void MoleculePanel::cancelWorker() {
    window.cancelWorker();
}
void MoleculePanel::confirmCandidate() {
    window.confirmCandidate();
}
void MoleculePanel::cancelCandidate() {
    window.cancelCandidate();
}
bool MoleculePanel::resolveCandidate() {
    return window.resolveCandidate();
}
void MoleculePanel::checkMcpPath(const QString &path) const {
    window.checkMcpPath(path);
}
void MoleculePanel::showSetting(int tab, const QString &setting) {
    window.showSetting(tab, setting);
}
void MoleculePanel::showInputLine() {
    window.showInputLine();
}
} // namespace mf
