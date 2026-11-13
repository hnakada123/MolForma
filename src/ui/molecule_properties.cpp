// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/coordination.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "editing/site.h"
#include "editing/stereochemistry.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "model/settings.h"
#include "nwchem/nwchem.h"
#include "ui/chrome_layouts.h"
#include "ui/molecule_dialogs.h"
#include "ui/molecule_panel.h"
#include "ui/part_palette.h"
#include "ui/property_page.h"
#include "ui/settings_widgets.h"
#include "ui/transform_page.h"
#include <QJsonDocument>
#include <QtWidgets>
namespace mf {
static QString value(QComboBox *b) {
    return b->currentData().toString();
}
static QList<int> integers(const QString &s) {
    QList<int> result;
    for (auto part : s.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts)) {
        bool ok;
        int n = part.toInt(&ok);
        if (!ok)
            fail(tr("整数をカンマまたは空白で区切ってください。"));
        result << n;
    }
    return result;
}
static Vec fields(QDoubleSpinBox *const *spins) {
    return {spins[0]->value(), spins[1]->value(), spins[2]->value()};
}
// The pages of the properties that stay with the workspace, each filled on a PropertyPage: "編集" holds
// the options of the tools (the element, the bond kind, what a deletion does) and what refresh() shows
// of the selection and the candidate; "配位" and "残基" place the part chosen in the palette with the
// ligand and amino-acid tools, which read their fields.
PropertyPage *MoleculePanel::addPage(const QString &title) {
    auto *page = new PropertyPage(editorContext, uiContext);
    properties->addTab(page, title);
    propertyPages << page;
    return page;
}
void MoleculePanel::buildEditPanel() {
    auto *page = addPage(tr("編集"));
    auto *outer = page->form();
    // A pending candidate atom by atom, then the selection in click order, above the edit pages.
    candidateCard = new QFrame;
    candidateCard->setObjectName("candidateCard");
    auto *changes = new QVBoxLayout(candidateCard);
    changes->setContentsMargins(10, 8, 10, 10);
    changes->setSpacing(6);
    auto *changesTitle = new QLabel(tr("変更候補の内容"));
    auto bold = changesTitle->font();
    bold.setBold(true);
    changesTitle->setFont(bold);
    changes->addWidget(changesTitle);
    candidateChanges = new QLabel;
    candidateChanges->setObjectName("candidateChanges");
    candidateChanges->setTextFormat(Qt::RichText);
    candidateChanges->setWordWrap(true);
    changes->addWidget(candidateChanges);
    auto *changesNote = new QLabel(tr("確定するまで分子は変わりません。確定後も Ctrl+Z で戻せます。"));
    changesNote->setWordWrap(true);
    changes->addWidget(changesNote);
    candidateCard->hide();
    outer->addRow(candidateCard);
    auto *card = new QFrame;
    card->setObjectName("selectionCard");
    card->setProperty("card", true);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(10, 8, 10, 10);
    cardLayout->setSpacing(6);
    selectionTitle = new QLabel;
    selectionTitle->setObjectName("selectionTitle");
    selectionTitle->setFont(bold);
    cardLayout->addWidget(selectionTitle);
    selectionList = new QLabel;
    selectionList->setObjectName("selectionList");
    selectionList->setTextFormat(Qt::RichText);
    selectionList->setWordWrap(true);
    cardLayout->addWidget(selectionList);
    auto *cardButtons = new FlowLayout(nullptr, 6, 6); // Wraps in a narrow column.
    auto *clearSelection = new QPushButton(tr("選択解除"));
    clearSelection->setObjectName("clearSelection");
    connect(clearSelection, &QPushButton::clicked, this, [this] { setSelection({}); });
    page->enableWithSelection(clearSelection, 1);
    cardButtons->addWidget(clearSelection);
    auto *deleteSelection = new QPushButton(tr("選択原子を削除"));
    deleteSelection->setObjectName("deleteSelection");
    deleteSelection->setToolTip(
        tr("選択した原子を削除する候補を作ります（Delete）。確定するまで元に戻せます。"));
    connect(deleteSelection, &QPushButton::clicked, this,
            [this] { safe([this] { deleteAtoms(requireSelection()); }); });
    page->enableWithSelection(deleteSelection, 1);
    cardButtons->addWidget(deleteSelection);
    // Refining only the selection, as "全体整形" above the view refines the whole molecule.
    auto *refineSelection = new QPushButton(tr("局所整形"));
    refineSelection->setObjectName("refineSelectionButton");
    refineSelection->setToolTip(tr("力場で選択原子だけを動かして整え、候補として表示します。"));
    connect(refineSelection, &QPushButton::clicked, this,
            [this] { safe([this] { runWorker("refine", {}, requireSelection()); }); });
    page->enableWithSelection(refineSelection, 1);
    cardButtons->addWidget(refineSelection);
    cardLayout->addLayout(cardButtons);
    keepSelection = new QCheckBox(tr("編集後に選択維持"));
    keepSelection->setObjectName("keepSelection");
    keepSelection->setToolTip(tr("オフでは、編集を確定するたびに選択を解除します。"));
    keepSelection->setChecked(uiContext.preference(Preference::keepSelection, true).toBool());
    cardLayout->addWidget(keepSelection);
    deleteTerminate = new QCheckBox(tr("削除後に水素で終端化"));
    deleteTerminate->setObjectName("deleteTerminate");
    deleteTerminate->setChecked(true);
    deleteTerminate->setToolTip(
        tr("すべての削除（Delete・ツールバー・削除ツール・右クリック）に適用します。削除した原子"
           "だけに付いていた水素も一緒に削除します。"));
    cardLayout->addWidget(deleteTerminate);
    outer->addRow(card);
    // The atoms and bonds under the selection; transformations and attributes on pages of their own.
    auto *f = outer;
    QStringList symbols;
    for (const auto &e : elements())
        symbols << e.symbol;
    page->section(f, QT_TR_NOOP("元素"));
    elementBox = page->choices(f, tr("追加・変更する元素"), symbols);
    elementBox->setCurrentIndex(elementBox->findData("C"));
    elementBox->setObjectName("elementSelector");
    elementBox->setToolTip(
        tr("「配置」ツールで置く原子・配位中心の元素、「元素を変更」で選択原子に設定する元素です。"
           "パレットで原子・配位中心を選ぶと連動します。"));
    page->enableWithSelection(
        page->button(f, tr("選択原子をこの元素に変更"),
                     [this] { replaceElements(requireSelection(), value(elementBox)); }),
        1);
    // In a card like the rest of the page: how the 3D view colors the atoms.
    auto *color = page->choices(f, tr("3D画面の色分け"), {tr("元素"), tr("残基"), tr("鎖"), tr("単色")},
                                {"element", "residue", "chain", "uniform"});
    color->setObjectName("moleculeColorMode");
    color->setToolTip(
        tr("残基: 残基ごとに色を変えます。鎖: たんぱく質・核酸の残基を鎖ごとの色にし、配位子・イオン・水は"
           "元素の色のままにします。単色: すべての原子を下の「色」で選んだ色にします。"));
    // The color of "単色": one of a few, or any from the color dialog.
    static constexpr std::pair<const char *, const char *> palette[] = {
        {QT_TR_NOOP("赤"), "#c8323c"}, {QT_TR_NOOP("橙"), "#e8822a"},   {QT_TR_NOOP("黄"), "#e3c13b"},
        {QT_TR_NOOP("緑"), "#4a9e52"}, {QT_TR_NOOP("水色"), "#5ab4d6"}, {QT_TR_NOOP("青"), "#3f6fb5"},
        {QT_TR_NOOP("紫"), "#8a5aa8"}, {QT_TR_NOOP("灰色"), "#9a9a9a"}};
    const auto swatch = [](const QColor &c) {
        QPixmap pixmap(14, 14);
        pixmap.fill(c);
        return QIcon(pixmap);
    };
    auto *uniform = new QComboBox;
    uniform->setObjectName("moleculeUniformColor");
    for (const auto &[name, hex] : palette)
        uniform->addItem(swatch(QColor(hex)), translated(name), QString(hex));
    uniform->addItem(tr("その他の色…"));
    uniform->setEnabled(false);
    uniform->setProperty("chosen", 0);
    view->setUniformColor(QColor(palette[0].second));
    f->addRow(tr("色"), uniform);
    connect(color, &QComboBox::currentIndexChanged, this, [this, color, uniform] {
        uniform->setEnabled(value(color) == "uniform");
        view->setColorMode(value(color));
    });
    connect(uniform, &QComboBox::currentIndexChanged, this, [this, uniform](int index) {
        if (const QColor c(uniform->itemData(index).toString()); c.isValid()) {
            uniform->setProperty("chosen", index);
            view->setUniformColor(c);
        }
    });
    // "その他の色…" asks for a color, which an item before it then holds; cancelling keeps the last one.
    connect(uniform, &QComboBox::activated, this, [this, uniform, swatch](int index) {
        if (index != uniform->count() - 1)
            return;
        const int chosen = uniform->property("chosen").toInt();
        const auto c =
            QColorDialog::getColor(QColor(uniform->itemData(chosen).toString()), this, tr("単色の色"));
        if (!c.isValid()) {
            uniform->setCurrentIndex(chosen);
            return;
        }
        int custom = uniform->count() - 2;
        if (!uniform->itemData(custom, Qt::UserRole + 1).toBool()) {
            uniform->insertItem(++custom, QString());
            uniform->setItemData(custom, true, Qt::UserRole + 1);
        }
        uniform->setItemIcon(custom, swatch(c));
        uniform->setItemText(custom, c.name());
        uniform->setItemData(custom, c.name());
        uniform->setCurrentIndex(custom);
    });
    // The coordinate table, when shown, edits the same values.
    positionCard = page->section(f, QT_TR_NOOP("座標（1原子）"));
    positionCard->setVisible(moleculeLayout->currentData() == "view");
    for (int i = 0; i < 3; ++i) {
        positionFields[i] = page->number(f, QString("XYZ")[i] + QString(" / Å"));
        positionFields[i]->setObjectName(QString("atomPosition") + QString("XYZ")[i]);
        positionFields[i]->setSpecialValueText("—");
        page->enableWithSelection(positionFields[i], 1, 1);
    }
    page->enableWithSelection(page->button(f, tr("座標を適用"),
                                           [this] {
                                               auto id = requireSelection(1).first();
                                               Vec p = fields(positionFields);
                                               edit(tr("座標を変更"), [=](Document &d) {
                                                   transform(d, {id}, Mat::Identity(),
                                                             p - d.atom(id).position);
                                               });
                                           }),
                              1, 1);
    page->enableWithSelection(page->button(f, tr("選択原子を原点へ"),
                                           [this] {
                                               auto ids = requireSelection(1);
                                               edit(tr("原点へ移動"), [=](Document &d) { orient(d, ids); });
                                               view->keepInView();
                                           }),
                              1, 1);
    page->section(f, QT_TR_NOOP("配置・結合"));
    auto *depth = page->number(f, tr("配置平面の奥行き / Å"));
    depth->setToolTip(
        tr("空白をクリックして原子・部品を置く平面の、画面奥行き方向の位置です（0: 画面中心）。"));
    connect(depth, &QDoubleSpinBox::valueChanged, this, [this](double v) { view->setDepth(v); });
    bondKind = page->choices(f, tr("結合種類"),
                             {tr("単結合"), tr("二重結合"), tr("三重結合"), tr("芳香族"),
                              tr("配位（供与→中心）"), tr("結合を削除")},
                             {"single", "double", "triple", "aromatic", "dative", "none"});
    page->enableWithSelection(page->button(f, tr("選択した2原子を結合"),
                                           [this] {
                                               auto ids = requireSelection(2);
                                               bondAtoms(ids[0], ids[1], value(bondKind));
                                           }),
                              2, 2);
    auto *side = page->button(f, tr("結合の片側を選択"), [this] {
        auto ids = requireSelection(2);
        setSelection(document().bondSide(ids[0], ids[1]));
    });
    side->setToolTip(tr("2原子の結合で分子を切り分け、後に選んだ原子の側をすべて選択します。"));
    page->enableWithSelection(side, 2, 2);
}
void MoleculePanel::sortAtoms(const QString &mode) {
    const auto ids = selected;
    edit(tr("原子を並べ替え"), [=](Document &d) { mf::sortAtoms(d, mode, ids); });
}
QString MoleculePanel::selectedResidue() const {
    for (auto id : selected) {
        auto rid = residueForAtom(displayDocument(), id);
        if (!rid.isEmpty())
            return rid;
    }
    if (residueList->currentItem())
        return residueList->currentItem()->data(Qt::UserRole).toString();
    fail(tr("対象の残基を選択してください。"));
}
QString MoleculePanel::selectedPart() const {
    for (auto id : selected)
        for (const auto &p : displayDocument().parts)
            if (p.atoms.contains(id))
                return p.id;
    fail(tr("部品を選択してください。"));
}
QString MoleculePanel::selectedCenter() const {
    const auto &d = displayDocument();
    for (auto id : selected) {
        if (d.findCenter(id))
            return id;
        for (const auto &p : d.parts)
            if (p.atoms.contains(id) && !p.center().isEmpty())
                return p.center();
    }
    fail(tr("配位中心または配位子を選択してください。"));
}
QList<int> MoleculePanel::sites() const {
    auto indices = integers(siteInput->text());
    for (auto &i : indices)
        --i;
    return indices;
}
void MoleculePanel::buildCoordinationPanel() {
    auto *page = addPage(tr("配位"));
    auto *f = page->form();
    // The notes in the cards, as on the other pages.
    page->section(f, QT_TR_NOOP("配位形状"));
    auto *intro = new QLabel(tr("配位中心またはその配位子を3D画面で選んでから操作します。"
                                "空いているサイトは紫の番号で表示します。"));
    intro->setWordWrap(true);
    intro->setProperty("caption", true);
    f->addRow(intro);
    geometryBox = page->choices(f, tr("配位形状"), geometryLabels(), geometryNames());
    geometryBox->setCurrentIndex(6);
    geometryBox->setObjectName("coordinationGeometry");
    distance = page->number(f, tr("中心–供与原子 / Å"), 2.1, .1, 20);
    distance->setObjectName("coordinationDistance");
    oxidation = new QSpinBox;
    oxidation->setObjectName("coordinationOxidation");
    oxidation->setRange(-8, 12);
    oxidation->setValue(2);
    oxidation->setToolTip(tr("構造準備の補助情報です。計算の全電荷・多重度は「2 計算条件」で設定します。"));
    f->addRow(tr("酸化数（補助）"), oxidation);
    auto *info =
        new QLabel(tr("酸化数は構造準備の補助情報です。計算の全電荷・多重度は「2 計算条件」で設定します。"));
    info->setWordWrap(true);
    info->setProperty("caption", true);
    f->addRow(info);
    page->button(f, tr("配位形状・距離を適用"), [this] {
        auto id = selectedCenter(), g = value(geometryBox);
        double dist = distance->value();
        int ox = oxidation->value();
        edit(tr("配位形状を変更"), [=](Document &d) { adjustCenter(d, id, g, dist, ox); }, true);
    });
    page->button(f, tr("酸化数だけを適用"), [this] {
        auto id = selectedCenter();
        int ox = oxidation->value();
        edit(tr("酸化数を変更"), [=](Document &d) { setOxidation(d, id, ox); });
    });
    page->section(f, QT_TR_NOOP("配位子の配置"));
    siteInput = new QLineEdit("1");
    siteInput->setObjectName("ligandSites");
    siteInput->setPlaceholderText(tr("例: 1 または 1,2"));
    siteInput->setToolTip(tr("3D画面の紫の番号です。サイトをクリックしても入力されます。"
                             "多座配位子は供与原子の数（η配位は1つ）だけサイトをカンマで区切ります。"));
    f->addRow(tr("サイト番号"), siteInput);
    page->button(f, tr("配位子パレットを開く"), [this] {
        partPalette->setCategory("ligand");
        showPalette(true);
    });
    page->button(f, tr("選択サイトへ配置（候補）"),
                 [this] {
                     QString c = selectedCenter();
                     auto idx = sites();
                     auto t = currentTemplate();
                     double angle = transformPage->twist();
                     proposeLigand(c, idx, t, angle);
                 })
        ->setProperty("primary", true);
    page->button(f, tr("配位子を交換"),
                 [this] {
                     QString p = selectedPart();
                     auto idx = sites();
                     auto t = currentTemplate();
                     edit(tr("配位子を交換"), [=](Document &d) { moveLigand(d, p, idx, t); }, true);
                 })
        ->setToolTip(tr("選択した配位子を、パレットで選んだ配位子に置き換えます。"));
    page->button(f, tr("配位子を指定サイトへ移動"), [this] {
        QString p = selectedPart();
        auto idx = sites();
        edit(tr("配位子を移動"), [=](Document &d) { moveLigand(d, p, idx); }, true);
    });
    page->button(f, tr("2サイトの配位子を交換"),
                 [this] {
                     auto id = selectedCenter();
                     auto indices = sites();
                     if (indices.size() != 2)
                         fail(tr("2サイトを指定してください。"));
                     edit(
                         tr("配位子のサイトを交換"),
                         [=](Document &d) { swapLigands(d, id, indices[0], indices[1]); }, true);
                 })
        ->setToolTip(tr("「サイト番号」に2つの番号（例: 1,3）を入れて、そのサイトの配位子を入れ替えます。"));
    page->section(f, QT_TR_NOOP("サイトの方向"), 0);
    auto *dx = page->number(f, tr("サイト方向 X")), *dy = page->number(f, tr("サイト方向 Y")),
         *dz = page->number(f, tr("サイト方向 Z"), 1);
    page->button(f, tr("個別サイトXYZ方向を適用"), [this, dx, dy, dz] {
        auto id = selectedCenter();
        auto indices = sites();
        Vec dir(dx->value(), dy->value(), dz->value());
        double dist = distance->value();
        edit(
            tr("配位サイトの方向を変更"), [=](Document &d) { setSiteDirection(d, id, indices, dir, dist); },
            true);
    });
}
void MoleculePanel::addAmino(const QString &rid, bool n) {
    auto t = currentTemplate();
    if (t["category"] != "amino")
        fail(tr("アミノ酸をパレットで選択してください。"));
    QString code = t["code"].toString(), state = t["state"].toString("default"), ch = value(chiralityBox),
            nt = value(nTerm), ct = value(cTerm), result;
    edit(tr("末端に残基を追加"),
         [&](Document &d) { result = addTerminalResidue(d, rid, n, code, state, ch, nt, ct); });
    growthResidue = result;
    growthN = n;
    view->keepInView();
}
void MoleculePanel::buildResiduePanel() {
    auto *page = addPage(tr("残基"));
    auto *f = page->form();
    page->section(f, QT_TR_NOOP("PDB・残基の認識"));
    page->button(f, tr("PDBを読み込む…"), [this] { emit openRequested(); });
    page->button(f, tr("PDBからダウンロード…"), [this] { emit downloadRequested(); }, "downloadPdb");
    page->button(
        f, tr("PDBの診断・修復…"), [this] { preparePdb(this, editorContext, uiContext); }, "preparePdb");
    page->button(f, tr("構造を検査…"), [this] { showReport(); });
    page->button(f, tr("結合グラフから残基を認識"),
                 [this] { edit(tr("残基を認識"), [](Document &d) { recognizeResidues(d); }, true); })
        ->setEnabled(hasStereochemistryBackend());
    page->button(f, tr("選択残基の欠損原子を補完"), [this] {
        auto rid = selectedResidue();
        edit(tr("欠損側鎖・水素を補完"), [=](Document &d) { repairResidue(d, rid); }, true);
    });
    // A cluster model for quantum chemistry: the atoms around a ligand or residues, selected, then cut out.
    page->section(f, QT_TR_NOOP("活性部位の切り出し"));
    auto *radius = page->number(f, tr("半径 / Å"), 5, 1, 30);
    radius->setObjectName("siteRadius");
    radius->setDecimals(1);
    radius->setSingleStep(0.5);
    radius->setToolTip(tr("選択した原子からこの距離以内にある原子を選びます。"));
    auto *wholeResidues = new QCheckBox(tr("残基ごとに含める"));
    wholeResidues->setObjectName("siteWholeResidues");
    wholeResidues->setChecked(true);
    wholeResidues->setToolTip(
        tr("半径内に原子がある残基を丸ごと含めます。残基に登録されていない原子は、その分子ごと含めます。"));
    f->addRow(wholeResidues);
    auto *waters = new QCheckBox(tr("水分子を含める"));
    waters->setObjectName("siteWaters");
    waters->setChecked(true);
    f->addRow(waters);
    page->button(
            f, tr("周囲を選択"),
            [this, radius, wholeResidues, waters] {
                const SiteOptions options{radius->value(), wholeResidues->isChecked(), waters->isChecked()};
                const auto ids = siteAtoms(displayDocument(), requireSelection(), options);
                setSelection(ids);
                showMessage(
                    tr("周囲の%n原子を選択しました（半径 %1 Å）。", nullptr, ids.size()).arg(options.radius),
                    5000);
            },
            "selectSite")
        ->setToolTip(tr("選択した配位子や残基を中心に、その周りの原子を選択します。"));
    auto *fixCuts = new QCheckBox(tr("切断した原子を固定"));
    fixCuts->setObjectName("siteFixCuts");
    fixCuts->setChecked(true);
    fixCuts->setToolTip(tr("水素で終端した原子を固定し、構造最適化でも全体の中での位置を保ちます。"));
    f->addRow(fixCuts);
    auto *hydrogens = new QCheckBox(tr("水素を付ける（pH 7）"));
    hydrogens->setObjectName("siteHydrogens");
    hydrogens->setChecked(true);
    hydrogens->setToolTip(tr(
        "PDBの構造にない水素を、pH 7の状態で付けます。Asp・Glu −1、Lys・Arg +1、鎖の末端は荷電、Hisは金属に"
        "配位していない窒素に水素（どちらも配位していなければHIE）、金属に配位したCys −1、イオンと硫酸は"
        "その電荷にし、金属との結合は配位結合にします。そのほかの配位子は原子価に合わせて水素を付けます。"));
    f->addRow(hydrogens);
    auto *charges = new QCheckBox(tr("周りの電荷を点電荷として残す"));
    charges->setObjectName("siteCharges");
    charges->setToolTip(
        tr("削除する原子のうち、電荷を持つ基（Asp・Glu・Lys・Arg、鎖の末端、イオン、硫酸など）を点電荷として"
           "残し、計算条件で外部点電荷を含めます。水と中性の基は残しません。"));
    f->addRow(charges);
    auto *chargeRadius = page->number(f, tr("点電荷の範囲 / Å"), 15, 1, 100);
    chargeRadius->setObjectName("siteChargeRadius");
    chargeRadius->setDecimals(1);
    chargeRadius->setToolTip(tr("切り出す原子からこの距離以内の電荷を点電荷として残します。"));
    chargeRadius->setEnabled(false);
    connect(charges, &QCheckBox::toggled, chargeRadius, &QWidget::setEnabled);
    auto *cut = page->button(
        f, tr("選択範囲を切り出す"),
        [this, fixCuts, hydrogens, charges, chargeRadius] {
            const auto ids = requireSelection();
            const SiteCutOptions options{fixCuts->isChecked(), hydrogens->isChecked(), charges->isChecked(),
                                         chargeRadius->value()};
            edit(tr("活性部位を切り出す"), [=](Document &d) { cutOutNwchemSite(d, ids, options); }, true);
            view->fit();
        },
        "cutOutSite");
    cut->setProperty("primary", true);
    cut->setToolTip(tr("選択していない原子を削除し、切った結合を水素で終端します（候補として表示）。"));
    page->section(f, QT_TR_NOOP("配列から構築"));
    sequenceInput = new QLineEdit;
    sequenceInput->setObjectName("sequenceInput");
    sequenceInput->setPlaceholderText("AGS / ALA-GLY-SER");
    f->addRow(tr("N → C 配列"), sequenceInput);
    chiralityBox = page->choices(f, tr("立体"), {"L", "D"});
    chiralityBox->setObjectName("peptideChirality");
    // The states of the charged side chains; the others have only their default.
    aminoState = page->choices(f, tr("側鎖状態"),
                               {tr("既定（中性pH）"), tr("中性"), tr("陰イオン"), "HID", "HIE", "HIP"},
                               {"default", "neutral", "anion", "HID", "HIE", "HIP"});
    aminoState->setObjectName("peptideSideChain");
    aminoState->setToolTip(
        tr("既定: Asp・Glu −1、Lys・Arg +1、His は HIE。中性: Asp・Glu・Lys・Arg の電荷を外します。"
           "陰イオン: Cys・Tyr を −1 にします。HID・HIE・HIP（+1）は His のプロトン化状態です。"));
    nTerm =
        page->choices(f, tr("N末端"), {tr("プロトン化"), tr("非荷電"), "ACE"}, {"charged", "neutral", "ACE"});
    nTerm->setObjectName("peptideNTerminus");
    cTerm = page->choices(f, tr("C末端"), {"COO−", "COOH", "NME"}, {"charged", "neutral", "NME"});
    cTerm->setObjectName("peptideCTerminus");
    auto *replace = new QCheckBox(tr("既存の分子を置き換える"));
    f->addRow(replace);
    page->button(
            f, tr("配列から構築"),
            [this, replace] {
                auto codes = parseSequence(sequenceInput->text());
                auto n = value(nTerm), c = value(cTerm), ch = value(chiralityBox), state = value(aminoState);
                bool clear = replace->isChecked();
                edit(
                    tr("配列から構築"), [=](Document &d) { buildSequence(d, codes, n, c, ch, state, clear); },
                    true);
                view->keepInView();
            },
            "buildSequence")
        ->setProperty("primary", true);
    page->section(f, QT_TR_NOOP("残基の編集（パレットのアミノ酸を使用）"));
    page->button(f, tr("選択C末端へ残基を追加"), [this] { addAmino(selectedResidue(), false); });
    page->button(f, tr("選択N末端へ残基を追加"), [this] { addAmino(selectedResidue(), true); });
    page->button(f, tr("選択残基を置換"), [this] {
        auto rid = selectedResidue();
        auto t = currentTemplate();
        if (t["category"] != "amino")
            fail(tr("アミノ酸を選択してください。"));
        // The state of the amino acid chosen in the palette (HIP, neutral Glu, ...), as when adding it at an
        // end; the total charge follows as there.
        QString code = t["code"].toString(), state = t["state"].toString("default"), ch = value(chiralityBox);
        edit(tr("残基を置換"), [=](Document &d) { mutateResidue(d, rid, code, state, ch); }, true);
    });
    page->button(f, tr("選択した鎖の両末端へ適用"), [this] {
        auto rid = selectedResidue();
        QString n = value(nTerm), c = value(cTerm);
        edit(tr("末端状態を変更"), [=](Document &d) { setChainTerminals(d, rid, n, c); }, true);
    });
    page->button(f, tr("残基を削除（鎖を分割）"), [this] {
        auto rid = selectedResidue();
        edit(tr("残基を削除"), [=](Document &d) { deleteResidue(d, rid); }, true);
    });
    page->button(f, tr("選択した2末端を再連結"), [this] {
        Ids residues;
        for (auto id : requireSelection()) {
            auto rid = residueForAtom(document(), id);
            if (!rid.isEmpty() && !residues.contains(rid))
                residues << rid;
        }
        if (residues.size() != 2)
            fail(tr("C末端側、N末端側の順に2残基を選択してください。"));
        edit(tr("鎖を再連結"), [=](Document &d) { reconnectResidues(d, residues[0], residues[1]); }, true);
    });
    page->section(f, QT_TR_NOOP("主鎖・側鎖の二面角"));
    auto *torsion =
        page->choices(f, tr("二面角"), {"phi", "psi", "omega", "chi1", "chi2", "chi3", "chi4", "chi5"});
    torsion->setObjectName("residueTorsion");
    auto *angle = page->number(f, tr("角度 / °"), 180, -360, 360);
    angle->setObjectName("residueTorsionAngle");
    page->button(
        f, tr("二面角を適用"),
        [this, torsion, angle] {
            auto rid = selectedResidue(), kind = value(torsion);
            double a = angle->value();
            edit(tr("残基二面角を変更"), [=](Document &d) { setResidueTorsion(d, rid, kind, a); });
        },
        "applyResidueTorsion");
    page->section(f, QT_TR_NOOP("残基の一覧"));
    residueList = new QListWidget;
    residueList->setMaximumHeight(200);
    residueList->setObjectName("residueList");
    f->addRow(residueList);
    connect(residueList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        safe([this, item] {
            setSelection(residue(displayDocument(), item->data(Qt::UserRole).toString()).atoms.values());
        });
    });
    page->button(f, tr("未登録の原子を選択"), [this] {
        const auto residues = atomResidues(document());
        Ids ids;
        for (const auto &a : document().atoms)
            if (!residues.contains(a.id))
                ids << a.id;
        setSelection(ids);
    });
    page->button(f, tr("選択原子を残基に登録…"), [this] {
        auto ids = requireSelection();
        bool ok;
        QString code =
            QInputDialog::getText(this, tr("残基を登録"), tr("3文字コード"), QLineEdit::Normal, "UNK", &ok);
        if (ok)
            edit(tr("残基を登録"), [=](Document &d) { registerResidue(d, ids, code.toUpper()); });
    });
    page->button(f, tr("原子対応表をCSVで書き出す…"), [this] {
        if (!resolveCandidate())
            return;
        QString p = QFileDialog::getSaveFileName(this, tr("対応表を保存"), "atoms.csv", "CSV (*.csv)");
        if (p.isEmpty())
            return;
        QString csv = "index,element,atom_name,residue\n";
        auto quote = [](QString s) {
            s.replace('"', "\"\"");
            return '"' + s + '"';
        };
        const auto residues = atomResidues(document());
        QHash<QString, QString> codes;
        for (const auto &r : document().residues)
            if (!codes.contains(r.id))
                codes.insert(r.id, r.code);
        for (int i = 0; i < document().atoms.size(); ++i) {
            const auto &a = document().atoms[i];
            QString name = codes.value(residues.value(a.id));
            csv += QString("%1,%2,%3,%4\n").arg(i + 1).arg(quote(a.element), quote(a.name), quote(name));
        }
        writeFile(p, csv.toUtf8());
    });
}
} // namespace mf
