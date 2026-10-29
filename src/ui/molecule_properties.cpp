// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/atom_properties.h"
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/coordination.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "editing/stereochemistry.h"
#include "model/i18n.h"
#include "nwchem/nwchem.h"
#include "ui/editor_chrome.h"
#include "ui/molecule_panel.h"
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
// A form in a card or above the cards of a page: its rows, then buttons side by side.
static QFormLayout *formLayout(QWidget *parent) {
    auto *form = new FormLayout(parent);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    return form;
}
QFormLayout *MoleculePanel::panel(const QString &title) {
    // The cards of section() under what comes before them, on the background of the settings tabs.
    auto *page = new QWidget;
    page->setProperty("canvas", true);
    auto *cards = new QVBoxLayout(page);
    cards->setContentsMargins(10, 10, 10, 12);
    cards->setSpacing(10);
    auto *top = new QWidget;
    auto *form = formLayout(top);
    cards->addWidget(top);
    cards->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setProperty("canvas", true);
    scroll->setWidgetResizable(true);
    scroll->setWidget(page);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    properties->addTab(scroll, title);
    pageCards[form] = cards;
    return form;
}
QPushButton *MoleculePanel::button(QFormLayout *f, const QString &text, const std::function<void()> &slot,
                                   const QString &name) {
    auto *b = new QPushButton(text);
    if (!name.isEmpty())
        b->setObjectName(name);
    // Buttons follow each other in rows that wrap, not each across the whole card.
    auto &row = buttonRows[f];
    if (!row.second || row.first != f->rowCount() - 1) {
        auto *buttons = new QWidget;
        new FlowLayout(buttons, 6, 6);
        buttons->layout()->setContentsMargins(0, 2, 0, 0);
        f->addRow(buttons);
        row = {f->rowCount() - 1, buttons};
    }
    row.second->layout()->addWidget(b);
    connect(b, &QPushButton::clicked, this, [this, slot] { safe(slot); });
    return b;
}
QDoubleSpinBox *MoleculePanel::number(QFormLayout *f, const QString &label, double v, double min,
                                      double max) {
    auto *n = new QDoubleSpinBox;
    n->setDecimals(5);
    n->setRange(min, max);
    n->setValue(v);
    n->setKeyboardTracking(false);
    f->addRow(label, n);
    return n;
}
QComboBox *MoleculePanel::choices(QFormLayout *f, const QString &label, const QStringList &labels,
                                  const QStringList &values) {
    auto *box = new QComboBox;
    for (int i = 0; i < labels.size(); ++i)
        box->addItem(labels[i], values.isEmpty() ? labels[i] : values[i]);
    f->addRow(label, box);
    return box;
}
SettingsCard *MoleculePanel::section(QFormLayout *&f, const QString &title, int open) {
    // A card per section on the page of the form; the rows that follow go into it. The title is the
    // source text (QT_TR_NOOP): it names the card in every language and is shown translated.
    auto *cards = pageCards.value(f);
    auto *card = new SettingsCard(translated(title));
    card->setObjectName("moleculeCard_" + title);
    // A long title (English, a wide font) wraps rather than widen the page, which scrolls only down.
    card->titleLabel()->setWordWrap(true);
    auto *body = new QWidget;
    auto *form = formLayout(body);
    card->body()->addWidget(body);
    if (open >= 0)
        card->setCollapsible(open);
    cards->insertWidget(cards->count() - 1, card);
    pageCards[form] = cards;
    f = form;
    return card;
}
void MoleculePanel::buildEditPanel() {
    auto *outer = panel(tr("編集"));
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
    enableWithSelection(clearSelection, 1);
    cardButtons->addWidget(clearSelection);
    auto *deleteSelection = new QPushButton(tr("選択原子を削除"));
    deleteSelection->setObjectName("deleteSelection");
    deleteSelection->setToolTip(
        tr("選択した原子を削除する候補を作ります（Delete）。確定するまで元に戻せます。"));
    connect(deleteSelection, &QPushButton::clicked, this,
            [this] { safe([this] { deleteAtoms(requireSelection()); }); });
    enableWithSelection(deleteSelection, 1);
    cardButtons->addWidget(deleteSelection);
    // Refining only the selection, as "全体整形" above the view refines the whole molecule.
    auto *refineSelection = new QPushButton(tr("局所整形"));
    refineSelection->setObjectName("refineSelectionButton");
    refineSelection->setToolTip(tr("力場で選択原子だけを動かして整え、候補として表示します。"));
    connect(refineSelection, &QPushButton::clicked, this,
            [this] { safe([this] { runWorker("refine", {}, requireSelection()); }); });
    enableWithSelection(refineSelection, 1);
    cardButtons->addWidget(refineSelection);
    cardLayout->addLayout(cardButtons);
    keepSelection = new QCheckBox(tr("編集後に選択維持"));
    keepSelection->setObjectName("keepSelection");
    keepSelection->setToolTip(tr("オフでは、編集を確定するたびに選択を解除します。"));
    keepSelection->setChecked(settings.value("keepSelection", true).toBool());
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
    section(f, QT_TR_NOOP("元素"));
    elementBox = choices(f, tr("追加・変更する元素"), symbols);
    elementBox->setCurrentIndex(elementBox->findData("C"));
    elementBox->setObjectName("elementSelector");
    elementBox->setToolTip(
        tr("「配置」ツールで置く原子・配位中心の元素、「元素を変更」で選択原子に設定する元素です。"
           "パレットで原子・配位中心を選ぶと連動します。"));
    enableWithSelection(button(f, tr("選択原子をこの元素に変更"),
                               [this] { replaceElements(requireSelection(), value(elementBox)); }),
                        1);
    // In a card like the rest of the page: how the 3D view colors the atoms.
    auto *color = choices(f, tr("3D画面の色分け"), {tr("元素"), tr("残基")}, {"element", "residue"});
    color->setObjectName("moleculeColorMode");
    connect(color, &QComboBox::currentIndexChanged, this,
            [this, color] { view->setColorMode(value(color)); });
    // The coordinate table, when shown, edits the same values.
    positionCard = section(f, QT_TR_NOOP("座標（1原子）"));
    positionCard->setVisible(moleculeLayout->currentData() == "view");
    for (int i = 0; i < 3; ++i) {
        positionFields[i] = number(f, QString("XYZ")[i] + QString(" / Å"));
        positionFields[i]->setObjectName(QString("atomPosition") + QString("XYZ")[i]);
        positionFields[i]->setSpecialValueText("—");
        enableWithSelection(positionFields[i], 1, 1);
    }
    enableWithSelection(button(f, tr("座標を適用"),
                               [this] {
                                   auto id = requireSelection(1).first();
                                   Vec p = fields(positionFields);
                                   edit(tr("座標を変更"), [=](Document &d) {
                                       transform(d, {id}, Mat::Identity(), p - d.atom(id).position);
                                   });
                               }),
                        1, 1);
    enableWithSelection(button(f, tr("選択原子を原点へ"),
                               [this] {
                                   auto ids = requireSelection(1);
                                   edit(tr("原点へ移動"), [=](Document &d) { orient(d, ids); });
                                   view->keepInView();
                               }),
                        1, 1);
    section(f, QT_TR_NOOP("配置・結合"));
    auto *depth = number(f, tr("配置平面の奥行き / Å"));
    depth->setToolTip(
        tr("空白をクリックして原子・部品を置く平面の、画面奥行き方向の位置です（0: 画面中心）。"));
    connect(depth, &QDoubleSpinBox::valueChanged, this, [this](double v) { view->setDepth(v); });
    bondKind = choices(f, tr("結合種類"),
                       {tr("単結合"), tr("二重結合"), tr("三重結合"), tr("芳香族"), tr("配位（供与→中心）"),
                        tr("結合を削除")},
                       {"single", "double", "triple", "aromatic", "dative", "none"});
    enableWithSelection(button(f, tr("選択した2原子を結合"),
                               [this] {
                                   auto ids = requireSelection(2);
                                   bondAtoms(ids[0], ids[1], value(bondKind));
                               }),
                        2, 2);
    auto *side = button(f, tr("結合の片側を選択"), [this] {
        auto ids = requireSelection(2);
        setSelection(document().bondSide(ids[0], ids[1]));
    });
    side->setToolTip(tr("2原子の結合で分子を切り分け、後に選んだ原子の側をすべて選択します。"));
    enableWithSelection(side, 2, 2);
    f = panel(tr("変形"));
    section(f, QT_TR_NOOP("平行移動（選択原子）"));
    for (int i = 0; i < 3; ++i) {
        translateFields[i] = number(f, QString("Δ") + QString("XYZ")[i] + " / Å");
        translateFields[i]->setObjectName(QString("translate") + QString("XYZ")[i]);
    }
    enableWithSelection(button(
                            f, tr("選択を平行移動"),
                            [this] {
                                auto ids = requireSelection();
                                Vec v = fields(translateFields);
                                edit(tr("平行移動"),
                                     [=](Document &d) { transform(d, ids, Mat::Identity(), v); });
                            },
                            "translateSelection"),
                        1);
    section(f, QT_TR_NOOP("回転"));
    // Rotation axis and mirror plane pass through the selected atoms' center, or the origin.
    auto *pivot = choices(f, tr("回転・鏡映の中心"), {tr("選択の中心"), tr("原点")}, {"selection", "origin"});
    pivot->setObjectName("transformPivot");
    const auto around = [pivot](const Document &d, const Ids &ids) {
        return value(pivot) == "origin" ? Vec(Vec::Zero()) : centroid(d, ids);
    };
    auto *axis = choices(f, tr("回転軸"), {"Z", "X", "Y"}, {"2", "0", "1"});
    axis->setObjectName("rotationAxis");
    twist = number(f, tr("回転角 / °"), 0, -360, 360);
    twist->setObjectName("rotationAngle");
    enableWithSelection(button(
                            f, tr("選択を回転"),
                            [this, axis, around] {
                                auto ids = requireSelection();
                                int ax = value(axis).toInt();
                                double a = twist->value();
                                edit(tr("選択を回転"), [=](Document &d) {
                                    transform(d, ids, rotation(Vec::Unit(ax), a), Vec::Zero(),
                                              around(d, ids));
                                });
                            },
                            "rotateSelection"),
                        1);
    section(f, QT_TR_NOOP("立体"));
    enableWithSelection(button(f, tr("不斉中心を反転"),
                               [this] {
                                   auto id = requireSelection(1).first();
                                   edit(tr("不斉中心を反転"), [=](Document &d) { invertCenter(d, id); });
                               }),
                        1, 1);
    enableWithSelection(button(f, tr("E/Zを切替"),
                               [this] {
                                   auto ids = requireSelection(2);
                                   edit(tr("E/Zを切替"), [=](Document &d) { switchEZ(d, ids); });
                               }),
                        2, 2);
    auto *stereo = button(f, tr("立体を記録（R/S・E/Z）"), [this] {
        edit(tr("立体配置を記録"), [](Document &d) { recordStereo(d); });
        // The 3D view labels them too: "C1 (S)", "(E)" on the double bond.
        QStringList found;
        for (int i = 0; i < document().atoms.size(); ++i)
            if (!document().atoms[i].stereo.isEmpty())
                found << QString("%1%2: %3")
                             .arg(document().atoms[i].element)
                             .arg(i + 1)
                             .arg(document().atoms[i].stereo);
        for (const auto &b : document().bonds)
            if (!b.stereo.isEmpty())
                found << QString("%1%2=%3%4: %5")
                             .arg(document().atom(b.a).element)
                             .arg(document().index(b.a) + 1)
                             .arg(document().atom(b.b).element)
                             .arg(document().index(b.b) + 1)
                             .arg(b.stereo);
        statusBar()->showMessage(found.isEmpty() ? tr("記録する不斉中心・E/Zの二重結合はありません。")
                                                 : tr("立体配置を記録しました: %1").arg(found.join(", ")),
                                 10000);
    });
    stereo->setEnabled(hasStereochemistryBackend());
    if (!hasStereochemistryBackend())
        stereo->setToolTip(tr("RDKitを含むビルドで使えます。"));
    section(f, QT_TR_NOOP("向き・位置"));
    auto *orientation = button(
        f, tr("向きをそろえる（原点・+X・XY）"),
        [this] {
            auto ids = requireSelection();
            edit(tr("向きをそろえる"), [=](Document &d) { orient(d, ids); });
            view->keepInView();
        },
        "orientSelection");
    orientation->setToolTip(
        tr("選択順に、1番目を原点、2番目を+X軸上、3番目をXY平面上へ動かします（分子全体）。"
           "1原子ならその原子が原点に来るよう平行移動します。"));
    enableWithSelection(orientation, 1, 3);
    button(f, tr("重心を原点へ"), [this] {
        auto ids = selected;
        edit(tr("重心を原点へ"), [=](Document &d) { centerMass(d, ids, false); });
        view->keepInView();
    });
    button(f, tr("主慣性軸へ整列"), [this] {
        auto ids = selected;
        edit(tr("主慣性軸へ整列"), [=](Document &d) { centerMass(d, ids, true); });
        view->keepInView();
    });
    section(f, QT_TR_NOOP("鏡映・複製"));
    auto *plane = choices(f, tr("鏡映面"), {"YZ", "XZ", "XY"}, {"0", "1", "2"});
    plane->setObjectName("mirrorPlane");
    auto *copy = new QCheckBox(tr("鏡像の写しを作る"));
    copy->setObjectName("mirrorCopy");
    f->addRow(copy);
    auto *mirrorButton = button(
        f, tr("指定平面で鏡映"),
        [this, plane, copy, around] {
            auto ids = requireSelection();
            int ax = value(plane).toInt();
            bool makeCopy = copy->isChecked();
            edit(tr("鏡映"), [=](Document &d) {
                if (makeCopy) {
                    auto added = duplicate(d, ids, Vec(6, 0, 0));
                    mirror(d, added, ax, around(d, added));
                } else
                    mirror(d, ids, ax, around(d, ids));
            });
            view->keepInView();
        },
        "mirrorSelection");
    enableWithSelection(mirrorButton, 1);
    for (int i = 0; i < 3; ++i) {
        duplicateFields[i] = number(f, tr("複製 %1 / Å").arg(QString("XYZ")[i]), i == 0 ? 3 : 0);
        duplicateFields[i]->setObjectName(QString("duplicate") + QString("XYZ")[i]);
    }
    auto *duplicateButton = button(
        f, tr("指定間隔で複製"),
        [this] {
            auto ids = requireSelection();
            Vec v = fields(duplicateFields);
            edit(tr("複製"), [=](Document &d) { duplicate(d, ids, v); });
            view->keepInView();
        },
        "duplicateSelection");
    enableWithSelection(duplicateButton, 1);
    f = panel(tr("属性"));
    section(f, QT_TR_NOOP("原子の属性"));
    formal = new QSpinBox;
    formal->setObjectName("atomFormalCharge");
    formal->setRange(MinimumAtomCharge - 1, MaximumAtomCharge);
    formal->setSpecialValueText(tr("変更なし"));
    f->addRow(tr("形式電荷"), formal);
    radical = new QSpinBox;
    radical->setObjectName("atomRadical");
    radical->setRange(-1, MaximumAtomRadical);
    radical->setSpecialValueText(tr("変更なし"));
    f->addRow(tr("不対電子"), radical);
    formal->setToolTip(tr("水素補完などの構造準備に使用します。計算の全電荷は「2 計算条件」で設定します。"));
    radical->setToolTip(tr("原子のラジカル属性です。計算のスピン多重度は「2 計算条件」で設定します。"));
    mass = new QLineEdit;
    mass->setObjectName("atomMass");
    mass->setPlaceholderText(tr("整数: 質量数 / 小数: amu"));
    f->addRow(tr("同位体 / 質量"), mass);
    for (QWidget *field : std::initializer_list<QWidget *>{formal, radical, mass})
        enableWithSelection(field, 1);
    auto *attributes = button(f, tr("属性を適用"), [this] {
        auto ids = requireSelection();
        int q = formal->value(), r = radical->value();
        QString m = mass->text().trimmed();
        AtomProperties properties;
        if (q != MinimumAtomCharge - 1)
            properties.charge = q;
        if (r != -1)
            properties.radical = r;
        if (!m.isEmpty() || ids.size() == 1) {
            const auto parsed = atomMassInput(m);
            properties.isotope = parsed.isotope;
            properties.mass = parsed.mass;
        }
        edit(tr("原子属性を変更"), [=](Document &d) { setAtomProperties(d, ids, properties); });
    });
    attributes->setObjectName("applyAtomAttributes");
    enableWithSelection(attributes, 1);
    section(f, QT_TR_NOOP("ゴースト・固定"));
    // The names of the coordinate table and the 3D view's menu, for the same edits.
    for (bool ghost : {true, false}) {
        auto *b = button(
            f, ghost ? tr("ゴーストにする") : tr("ゴーストを解除"),
            [this, ghost] {
                auto ids = requireSelection();
                edit(ghost ? tr("選択原子をゴーストにする") : tr("選択原子のゴーストを解除"),
                     [=](Document &d) {
                         AtomProperties properties;
                         properties.ghost = ghost;
                         setAtomProperties(d, ids, properties);
                     });
            },
            ghost ? "ghostSelectedAtoms" : "unghostSelectedAtoms");
        b->setToolTip(tr("基底関数だけを置く原子（NWChemのbq）です。BSSE補正などで使います。"));
        enableWithSelection(b, 1);
    }
    for (bool fixed : {true, false}) {
        auto *b = button(
            f, fixed ? tr("選択原子を固定") : tr("固定を解除"),
            [this, fixed] { fixAtoms(requireSelection(), fixed); },
            fixed ? "fixSelectedAtoms" : "unfixSelectedAtoms");
        b->setToolTip(tr("編集時の移動・削除を防ぎ、NWChemの構造最適化でも座標を固定します。"));
        enableWithSelection(b, 1);
    }
    // Sorting is in the menu "構造" of the coordinate table, the user's parts in the palette and refining
    // beside "全体整形": this page holds the attributes of atoms only.
}
void MoleculePanel::sortAtoms(const QString &mode) {
    const auto ids = selected;
    edit(tr("原子を並べ替え"), [=](Document &d) {
        QMap<QString, int> ranks;
        int n = 0;
        for (const auto &r : d.residues)
            for (const auto &id : r.atoms)
                ranks[id] = n++;
        std::stable_sort(d.atoms.begin(), d.atoms.end(), [&](const Atom &a, const Atom &b) {
            if (mode == "element")
                return element(a.element).number < element(b.element).number;
            if (mode == "residue")
                return ranks.value(a.id, 1000000) < ranks.value(b.id, 1000000);
            return ids.contains(a.id) > ids.contains(b.id);
        });
    });
}
void MoleculePanel::removePart() {
    auto t = currentTemplate();
    if (!t["id"].toString().startsWith("my:"))
        fail(tr("マイ部品をパレットで選択してください。"));
    for (int i = customParts.size() - 1; i >= 0; --i)
        if (customParts[i].toObject()["id"] == t["id"])
            customParts.removeAt(i);
    settings.setValue("parts", customParts);
    refreshPalette();
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
    auto *f = panel(tr("配位"));
    // The notes in the cards, as on the other pages.
    section(f, QT_TR_NOOP("配位形状"));
    auto *intro = new QLabel(tr("配位中心またはその配位子を3D画面で選んでから操作します。"
                                "空いているサイトは紫の番号で表示します。"));
    intro->setWordWrap(true);
    intro->setProperty("caption", true);
    f->addRow(intro);
    geometryBox = choices(f, tr("配位形状"), geometryLabels(), geometryNames());
    geometryBox->setCurrentIndex(6);
    geometryBox->setObjectName("coordinationGeometry");
    distance = number(f, tr("中心–供与原子 / Å"), 2.1, .1, 20);
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
    button(f, tr("配位形状・距離を適用"), [this] {
        auto id = selectedCenter(), g = value(geometryBox);
        double dist = distance->value();
        int ox = oxidation->value();
        edit(tr("配位形状を変更"), [=](Document &d) { adjustCenter(d, id, g, dist, ox); }, true);
    });
    button(f, tr("酸化数だけを適用"), [this] {
        auto id = selectedCenter();
        int ox = oxidation->value();
        edit(tr("酸化数を変更"), [=](Document &d) { setOxidation(d, id, ox); });
    });
    section(f, QT_TR_NOOP("配位子の配置"));
    siteInput = new QLineEdit("1");
    siteInput->setObjectName("ligandSites");
    siteInput->setPlaceholderText(tr("例: 1 または 1,2"));
    siteInput->setToolTip(tr("3D画面の紫の番号です。サイトをクリックしても入力されます。"
                             "二座配位子は2サイトをカンマで区切ります。"));
    f->addRow(tr("サイト番号"), siteInput);
    button(f, tr("配位子パレットを開く"), [this] {
        setPaletteCategory("ligand");
        showPalette(true);
    });
    button(f, tr("選択サイトへ配置（候補）"), [this] {
        QString c = selectedCenter();
        auto idx = sites();
        auto t = currentTemplate();
        double angle = twist->value();
        proposeLigand(c, idx, t, angle);
    })->setProperty("primary", true);
    button(f, tr("配位子を交換"), [this] {
        QString p = selectedPart();
        auto idx = sites();
        auto t = currentTemplate();
        edit(tr("配位子を交換"), [=](Document &d) { moveLigand(d, p, idx, t); }, true);
    })->setToolTip(tr("選択した配位子を、パレットで選んだ配位子に置き換えます。"));
    button(f, tr("配位子を指定サイトへ移動"), [this] {
        QString p = selectedPart();
        auto idx = sites();
        edit(tr("配位子を移動"), [=](Document &d) { moveLigand(d, p, idx); }, true);
    });
    button(f, tr("2サイトの配位子を交換"), [this] {
        auto id = selectedCenter();
        auto indices = sites();
        if (indices.size() != 2)
            fail(tr("2サイトを指定してください。"));
        edit(
            tr("配位子のサイトを交換"), [=](Document &d) { swapLigands(d, id, indices[0], indices[1]); },
            true);
    })->setToolTip(tr("「サイト番号」に2つの番号（例: 1,3）を入れて、そのサイトの配位子を入れ替えます。"));
    section(f, QT_TR_NOOP("サイトの方向"), 0);
    auto *dx = number(f, tr("サイト方向 X")), *dy = number(f, tr("サイト方向 Y")),
         *dz = number(f, tr("サイト方向 Z"), 1);
    button(f, tr("個別サイトXYZ方向を適用"), [this, dx, dy, dz] {
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
    auto *f = panel(tr("残基"));
    section(f, QT_TR_NOOP("PDB・残基の認識"));
    button(f, tr("PDBを読み込む…"), [this] { emit openRequested(); });
    button(f, tr("PDBの診断・修復…"), [this] { pdbPreparation(); }, "preparePdb");
    button(f, tr("構造を検査…"), [this] { showReport(); });
    button(f, tr("結合グラフから残基を認識"), [this] {
        edit(tr("残基を認識"), [](Document &d) { recognizeResidues(d); }, true);
    })->setEnabled(hasStereochemistryBackend());
    button(f, tr("選択残基の欠損原子を補完"), [this] {
        auto rid = selectedResidue();
        edit(tr("欠損側鎖・水素を補完"), [=](Document &d) { repairResidue(d, rid); }, true);
    });
    section(f, QT_TR_NOOP("配列から構築"));
    sequenceInput = new QLineEdit;
    sequenceInput->setObjectName("sequenceInput");
    sequenceInput->setPlaceholderText("AGS / ALA-GLY-SER");
    f->addRow(tr("N → C 配列"), sequenceInput);
    chiralityBox = choices(f, tr("立体"), {"L", "D"});
    chiralityBox->setObjectName("peptideChirality");
    // The states of the charged side chains; the others have only their default.
    aminoState =
        choices(f, tr("側鎖状態"), {tr("既定（中性pH）"), tr("中性"), tr("陰イオン"), "HID", "HIE", "HIP"},
                {"default", "neutral", "anion", "HID", "HIE", "HIP"});
    aminoState->setObjectName("peptideSideChain");
    aminoState->setToolTip(
        tr("既定: Asp・Glu −1、Lys・Arg +1、His は HIE。中性: Asp・Glu・Lys・Arg の電荷を外します。"
           "陰イオン: Cys・Tyr を −1 にします。HID・HIE・HIP（+1）は His のプロトン化状態です。"));
    nTerm = choices(f, tr("N末端"), {tr("プロトン化"), tr("非荷電"), "ACE"}, {"charged", "neutral", "ACE"});
    nTerm->setObjectName("peptideNTerminus");
    cTerm = choices(f, tr("C末端"), {"COO−", "COOH", "NME"}, {"charged", "neutral", "NME"});
    cTerm->setObjectName("peptideCTerminus");
    auto *replace = new QCheckBox(tr("既存の分子を置き換える"));
    f->addRow(replace);
    button(
        f, tr("配列から構築"),
        [this, replace] {
            auto codes = parseSequence(sequenceInput->text());
            auto n = value(nTerm), c = value(cTerm), ch = value(chiralityBox), state = value(aminoState);
            bool clear = replace->isChecked();
            edit(
                tr("配列から構築"),
                [=](Document &d) {
                    if (clear) // Keep calculation/execution settings and the project envelope.
                        replaceCoordinateStructure(d, Document());
                    auto old = d.ids();
                    double shift = 0;
                    for (const auto &a : d.atoms)
                        shift = std::max(shift, a.position.x() + 6);
                    buildPeptide(d, codes, n, c, ch, state);
                    if (!old.isEmpty()) {
                        const QSet<QString> before(old.begin(), old.end());
                        Ids added;
                        for (const auto &a : d.atoms)
                            if (!before.contains(a.id))
                                added << a.id;
                        transform(d, added, Mat::Identity(), Vec(shift, 0, 0));
                    }
                    if (clear)
                        d.charge = d.formalCharge();
                },
                true);
            view->keepInView();
        },
        "buildSequence")
        ->setProperty("primary", true);
    section(f, QT_TR_NOOP("残基の編集（パレットのアミノ酸を使用）"));
    button(f, tr("選択C末端へ残基を追加"), [this] { addAmino(selectedResidue(), false); });
    button(f, tr("選択N末端へ残基を追加"), [this] { addAmino(selectedResidue(), true); });
    button(f, tr("選択残基を置換"), [this] {
        auto rid = selectedResidue();
        auto t = currentTemplate();
        if (t["category"] != "amino")
            fail(tr("アミノ酸を選択してください。"));
        // The state of the amino acid chosen in the palette (HIP, neutral Glu, ...), as when adding it at an
        // end; the total charge follows as there.
        QString code = t["code"].toString(), state = t["state"].toString("default"), ch = value(chiralityBox);
        edit(tr("残基を置換"), [=](Document &d) { mutateResidue(d, rid, code, state, ch); }, true);
    });
    button(f, tr("選択した鎖の両末端へ適用"), [this] {
        auto rid = selectedResidue();
        QString n = value(nTerm), c = value(cTerm);
        edit(tr("末端状態を変更"), [=](Document &d) { setChainTerminals(d, rid, n, c); }, true);
    });
    button(f, tr("残基を削除（鎖を分割）"), [this] {
        auto rid = selectedResidue();
        edit(tr("残基を削除"), [=](Document &d) { deleteResidue(d, rid); }, true);
    });
    button(f, tr("選択した2末端を再連結"), [this] {
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
    section(f, QT_TR_NOOP("主鎖・側鎖の二面角"));
    auto *torsion = choices(f, tr("二面角"), {"phi", "psi", "omega", "chi1", "chi2", "chi3", "chi4", "chi5"});
    torsion->setObjectName("residueTorsion");
    auto *angle = number(f, tr("角度 / °"), 180, -360, 360);
    angle->setObjectName("residueTorsionAngle");
    button(
        f, tr("二面角を適用"),
        [this, torsion, angle] {
            auto rid = selectedResidue(), kind = value(torsion);
            double a = angle->value();
            edit(tr("残基二面角を変更"), [=](Document &d) { setResidueTorsion(d, rid, kind, a); });
        },
        "applyResidueTorsion");
    section(f, QT_TR_NOOP("残基の一覧"));
    residueList = new QListWidget;
    residueList->setMaximumHeight(200);
    residueList->setObjectName("residueList");
    f->addRow(residueList);
    connect(residueList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        safe([this, item] {
            setSelection(residue(displayDocument(), item->data(Qt::UserRole).toString()).atoms.values());
        });
    });
    button(f, tr("未登録の原子を選択"), [this] {
        const auto residues = atomResidues(document());
        Ids ids;
        for (const auto &a : document().atoms)
            if (!residues.contains(a.id))
                ids << a.id;
        setSelection(ids);
    });
    button(f, tr("選択原子を残基に登録…"), [this] {
        auto ids = requireSelection();
        bool ok;
        QString code =
            QInputDialog::getText(this, tr("残基を登録"), tr("3文字コード"), QLineEdit::Normal, "UNK", &ok);
        if (ok)
            edit(tr("残基を登録"), [=](Document &d) { registerResidue(d, ids, code.toUpper()); });
    });
    button(f, tr("原子対応表をCSVで書き出す…"), [this] {
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
void MoleculePanel::buildChargePanel() {
    auto *f = panel(tr("電荷"));
    chargeEditor = f->parentWidget()->parentWidget();
    // The total charge and multiplicity are settings of "2 計算条件": here they are shown with the
    // formal charges of the atoms and the electrons they give, with the way there.
    section(f, QT_TR_NOOP("分子の電荷"));
    chargeState = new QLabel;
    chargeState->setObjectName("moleculeChargeState");
    auto *chargeLink = linkButton(tr("2 計算条件で変更"), "moleculeChargeLink");
    connect(chargeLink, &QPushButton::clicked, this, [this] { showSetting(1, "charge"); });
    // The value and the link wrap: the link goes under the value where the column is narrow.
    auto *chargeRow = new QWidget;
    auto *chargeRowLayout = new FlowLayout(chargeRow, 10, 4);
    chargeRowLayout->setContentsMargins(0, 0, 0, 0);
    chargeRowLayout->addWidget(chargeState);
    chargeRowLayout->addWidget(chargeLink);
    f->addRow(tr("全電荷・多重度"), chargeRow);
    chargeSummary = new QLabel;
    chargeSummary->setObjectName("moleculeChargeSummary");
    chargeSummary->setTextFormat(Qt::PlainText);
    chargeSummary->setWordWrap(true);
    f->addRow(chargeSummary);
    inputState = new QLabel;
    inputState->setObjectName("moleculeInputState");
    inputState->setWordWrap(true);
    f->addRow(inputState);
    // With coordination centers, their oxidation numbers take the place of the centers' formal charges.
    button(
        f, tr("形式電荷の合計を全電荷に適用"),
        [this] { edit(tr("全電荷を合わせる"), [](Document &d) { d.charge = d.expectedCharge(); }); },
        "applyMoleculeFormalCharge")
        ->setToolTip(
            tr("原子の形式電荷の合計（配位中心は酸化数）を、計算の全電荷（「2 計算条件」）にします。"));
    auto *preview = linkButton(tr("4 入力プレビューで確認"), "moleculeInputPreview");
    connect(preview, &QPushButton::clicked, this, [this] { showInputLine(); });
    f->addRow(preview);
    section(f, QT_TR_NOOP("外部点電荷"));
    pointChargeState = new QLabel;
    pointChargeState->setObjectName("moleculePointChargeState");
    auto *pointLink = linkButton(tr("2 計算条件で設定"), "moleculePointChargeLink");
    pointLink->setToolTip(tr("点電荷をNWChem入力に含めるかは「2 計算条件」の「周囲の環境」で設定します。"));
    connect(pointLink, &QPushButton::clicked, this, [this] { showSetting(1, "include_point_charges"); });
    auto *pointRow = new QWidget;
    auto *pointRowLayout = new FlowLayout(pointRow, 10, 4);
    pointRowLayout->setContentsMargins(0, 0, 0, 0);
    pointRowLayout->addWidget(pointChargeState);
    pointRowLayout->addWidget(pointLink);
    f->addRow(pointRow);
    pointList = new QListWidget;
    pointList->setObjectName("pointChargeList");
    pointList->setMaximumHeight(160);
    f->addRow(pointList);
    pointEmpty =
        new QLabel(tr("点電荷はまだありません。「点電荷」ツールで3D画面の空白をクリックするか、下の欄に"
                      "位置と電荷を入れて「点電荷を追加」を押します。"));
    pointEmpty->setObjectName("pointChargeEmpty");
    pointEmpty->setProperty("muted", true);
    pointEmpty->setWordWrap(true);
    f->addRow(pointEmpty);
    // The position and charge two by two, under their names.
    auto *fieldsBox = new QWidget;
    auto *fieldGrid = new QGridLayout(fieldsBox);
    fieldGrid->setContentsMargins(0, 0, 0, 0);
    fieldGrid->setHorizontalSpacing(8);
    fieldGrid->setVerticalSpacing(3);
    const auto field = [](const QString &name) {
        auto *n = new QDoubleSpinBox;
        n->setObjectName(name);
        n->setDecimals(5);
        n->setRange(-10000, 10000);
        n->setKeyboardTracking(false);
        return n;
    };
    for (int i = 0; i < 4; ++i) {
        auto *spin = i < 3 ? (chargeFields[i] = field(QString("pointCharge") + QString("XYZ")[i]))
                           : (chargeValue = field("pointChargeValue"));
        auto *label = new QLabel(i < 3 ? QString("XYZ")[i] + QString(" / Å") : tr("電荷 / e"));
        label->setProperty("caption", true);
        label->setBuddy(spin);
        fieldGrid->addWidget(label, (i / 2) * 2, i % 2);
        fieldGrid->addWidget(spin, (i / 2) * 2 + 1, i % 2);
    }
    chargeValue->setValue(.5);
    f->addRow(fieldsBox);
    connect(pointList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        for (const auto &q : displayDocument().pointCharges)
            if (q.id == item->data(Qt::UserRole).toString()) {
                for (int i = 0; i < 3; ++i)
                    chargeFields[i]->setValue(q.position[i]);
                chargeValue->setValue(q.charge);
            }
    });
    button(
        f, tr("点電荷を追加"),
        [this] {
            Vec p = fields(chargeFields);
            double q = chargeValue->value();
            edit(tr("点電荷を追加"), [=](Document &d) { addPointCharge(d, p, q); });
        },
        "addPointCharge")
        ->setProperty("primary", true);
    button(
        f, tr("点電荷を更新"),
        [this] {
            if (!pointList->currentItem())
                fail(tr("点電荷を選択してください。"));
            QString id = pointList->currentItem()->data(Qt::UserRole).toString();
            Vec p = fields(chargeFields);
            double q = chargeValue->value();
            edit(tr("点電荷を更新"), [=](Document &d) { setPointCharge(d, id, p, q); });
        },
        "updatePointCharge");
    button(
        f, tr("点電荷を削除"),
        [this] {
            if (!pointList->currentItem())
                fail(tr("点電荷を選択してください。"));
            QString id = pointList->currentItem()->data(Qt::UserRole).toString();
            edit(tr("点電荷を削除"), [=](Document &d) { removePointCharge(d, id); });
        },
        "deletePointCharge");
    enableWithSelection(
        button(f, tr("選択した水をTIP3P点電荷へ"),
               [this] {
                   auto ids = requireSelection();
                   edit(tr("水を点電荷へ変換"), [=](Document &d) { waterToCharges(d, ids); }, true);
               }),
        1);
    button(f, tr("点電荷をファイルに書き出す…"), [this] {
        if (!resolveCandidate())
            return;
        QString p = QFileDialog::getSaveFileName(this, tr("点電荷を書き出す"), "charges.dat",
                                                 tr("点電荷 (*.dat *.txt)"));
        if (p.isEmpty())
            return;
        QString text = "# MolForma point charges; units=angstrom; x y z q\n";
        for (const auto &q : document().pointCharges)
            text += QString("%1 %2 %3 %4\n")
                        .arg(q.position.x(), 0, 'f', 10)
                        .arg(q.position.y(), 0, 'f', 10)
                        .arg(q.position.z(), 0, 'f', 10)
                        .arg(q.charge, 0, 'g', 15);
        writeFile(p, text.toUtf8());
    });
}
void MoleculePanel::refreshChargePanel() {
    const auto &d = displayDocument();
    chargeState->setText(tr("%1 · 多重度 %2").arg(d.charge).arg(d.multiplicity));
    pointChargeState->setText(nwchemSettings(d)["include_point_charges"].toBool()
                                  ? tr("NWChem入力に含める")
                                  : tr("NWChem入力に含めない"));
    chargeEditor->setEnabled(!session.candidate() && !chemistryRunning());
    if (auto *apply = findChild<QPushButton *>("applyMoleculeFormalCharge"))
        apply->setText(d.hasCenters()
                           ? tr("酸化数と形式電荷の合計（%1）を全電荷に適用").arg(d.expectedCharge())
                           : tr("形式電荷の合計を全電荷に適用"));
    const auto check = validateNwchem(d);
    QStringList text{(check.ecpElectrons ? tr("形式電荷の合計 %1 · 電子数 %2（ECP適用後）")
                                         : tr("形式電荷の合計 %1 · 電子数 %2"))
                         .arg(d.formalCharge())
                         .arg(check.electrons)};
    if (check.errors.isEmpty())
        text << QString("α %1 / β %2").arg(check.alpha).arg(check.beta);
    for (const auto &error : check.errors)
        text << tr("エラー: %1").arg(error);
    for (const auto &warning : check.warnings)
        text << tr("確認: %1").arg(warning);
    if (session.candidate())
        text.prepend(tr("未確定候補です。確定・取消後に設定を変更できます。"));
    else if (chemistryRunning())
        text.prepend(tr("分子の整形中です。終了後に設定を変更できます。"));
    chargeSummary->setText(text.join('\n'));
    // The outcome of the checks as the cards of the settings tabs say it.
    const bool ok = check.errors.isEmpty();
    inputState->setText(ok ? tr("✓ 入力を生成できます")
                           : tr("入力を生成できません（エラー %n 件）", nullptr, int(check.errors.size())));
    if (inputState->property("good").toBool() != ok) {
        inputState->setProperty("good", ok);
        inputState->setProperty("bad", !ok);
        repolish(inputState);
    }
}
void MoleculePanel::pdbPreparation() {
    if (session.candidate())
        fail(tr("先に候補を確定または取消してください。"));
    if (document().atoms.isEmpty())
        fail(tr("PDBファイルを開いてから診断してください。"));
    QDialog dialog(this);
    dialog.setWindowTitle(tr("PDBの診断・修復"));
    dialog.resize(820, 620);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(tr("残基を選ぶと診断を表示します。修復は3D画面で候補を確認して確定します。"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *list = new QListWidget;
    layout->addWidget(list);
    for (const auto &r : document().residues) {
        auto *item = new QListWidgetItem(
            QString("%1 %2  ·  %3 / %4").arg(r.code).arg(r.number).arg(r.nTerminal, r.cTerminal), list);
        item->setData(Qt::UserRole, r.id);
    }
    auto *report = new QPlainTextEdit;
    report->setReadOnly(true);
    report->setPlainText(structureReport(document()));
    layout->addWidget(report, 1);
    connect(list, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem *item) {
        if (!item)
            return;
        safe([&] {
            report->setPlainText(
                residueDiagnostics(document(), item->data(Qt::UserRole).toString()).join('\n'));
        });
    });
    auto *actions = new QHBoxLayout;
    layout->addLayout(actions);
    auto add = [&](QString text, auto fn) {
        auto *b = new QPushButton(text);
        actions->addWidget(b);
        connect(b, &QPushButton::clicked, &dialog, [&, fn] { safe(fn); });
        return b;
    };
    add(tr("3Dで選択箇所を見る"), [&] {
        if (!list->currentItem())
            fail(tr("残基を選んでください。"));
        setSelection(residue(document(), list->currentItem()->data(Qt::UserRole).toString()).atoms.values());
        dialog.accept();
    });
    add(tr("修復候補を作成"), [&] {
        if (!list->currentItem())
            fail(tr("残基を選んでください。"));
        auto rid = list->currentItem()->data(Qt::UserRole).toString();
        edit(tr("PDBの欠損原子を補完"), [=](Document &d) { repairResidue(d, rid); }, true);
        dialog.accept();
    });
    add(tr("残基を再認識"), [&] {
        edit(tr("PDBの残基を認識"), [](Document &d) { recognizeResidues(d); }, true);
        dialog.accept();
    })->setEnabled(hasStereochemistryBackend());
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
} // namespace mf
