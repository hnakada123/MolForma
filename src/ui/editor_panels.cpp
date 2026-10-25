#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/coordination.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "editing/stereochemistry.h"
#include "nwchem/nwchem.h"
#include "ui/calculation_panel.h"
#include "ui/editor_chrome.h"
#include "ui/input_preview_panel.h"
#include "ui/main_window.h"
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
            fail("整数をカンマまたは空白で区切ってください。");
        result << n;
    }
    return result;
}
static Vec fields(QDoubleSpinBox *const *spins) {
    return {spins[0]->value(), spins[1]->value(), spins[2]->value()};
}
// A form in a card or above the cards of a page: its rows, then buttons side by side.
static QFormLayout *formLayout(QWidget *parent) {
    auto *form = new QFormLayout(parent);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    return form;
}
QFormLayout *MainWindow::panel(const QString &title) {
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
QPushButton *MainWindow::button(QFormLayout *f, const QString &text, const std::function<void()> &slot,
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
QDoubleSpinBox *MainWindow::number(QFormLayout *f, const QString &label, double v, double min, double max) {
    auto *n = new QDoubleSpinBox;
    n->setDecimals(5);
    n->setRange(min, max);
    n->setValue(v);
    n->setKeyboardTracking(false);
    f->addRow(label, n);
    return n;
}
QComboBox *MainWindow::choices(QFormLayout *f, const QString &label, const QStringList &labels,
                               const QStringList &values) {
    auto *box = new QComboBox;
    for (int i = 0; i < labels.size(); ++i)
        box->addItem(labels[i], values.isEmpty() ? labels[i] : values[i]);
    f->addRow(label, box);
    return box;
}
SettingsCard *MainWindow::section(QFormLayout *&f, const QString &title, int open) {
    // A card per section on the page of the form; the rows that follow go into it.
    auto *cards = pageCards.value(f);
    auto *card = new SettingsCard(title);
    card->setObjectName("moleculeCard_" + title);
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
void MainWindow::buildEditPanel() {
    auto *outer = panel("編集");
    // A pending candidate atom by atom, then the selection in click order, above the edit pages.
    candidateCard = new QFrame;
    candidateCard->setObjectName("candidateCard");
    auto *changes = new QVBoxLayout(candidateCard);
    changes->setContentsMargins(10, 8, 10, 10);
    changes->setSpacing(6);
    auto *changesTitle = new QLabel("変更候補の内容");
    auto bold = changesTitle->font();
    bold.setBold(true);
    changesTitle->setFont(bold);
    changes->addWidget(changesTitle);
    candidateChanges = new QLabel;
    candidateChanges->setObjectName("candidateChanges");
    candidateChanges->setTextFormat(Qt::RichText);
    candidateChanges->setWordWrap(true);
    changes->addWidget(candidateChanges);
    auto *changesNote = new QLabel("確定するまで分子は変わりません。確定後も Ctrl+Z で戻せます。");
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
    auto *cardButtons = new QHBoxLayout;
    auto *clearSelection = new QPushButton("選択解除");
    clearSelection->setObjectName("clearSelection");
    connect(clearSelection, &QPushButton::clicked, this, [this] { setSelection({}); });
    enableWithSelection(clearSelection, 1);
    cardButtons->addWidget(clearSelection);
    auto *deleteSelection = new QPushButton("選択原子を削除");
    deleteSelection->setObjectName("deleteSelection");
    deleteSelection->setToolTip("選択した原子を削除する候補を作ります（Delete）。確定するまで元に戻せます。");
    connect(deleteSelection, &QPushButton::clicked, this,
            [this] { safe([this] { deleteAtoms(requireSelection()); }); });
    enableWithSelection(deleteSelection, 1);
    cardButtons->addWidget(deleteSelection);
    cardButtons->addStretch();
    cardLayout->addLayout(cardButtons);
    keepSelection = new QCheckBox("編集後に選択維持");
    keepSelection->setToolTip("オフでは、編集を確定するたびに選択を解除します。");
    keepSelection->setChecked(settings.value("keepSelection", true).toBool());
    cardLayout->addWidget(keepSelection);
    deleteTerminate = new QCheckBox("削除後に水素で終端化");
    deleteTerminate->setObjectName("deleteTerminate");
    deleteTerminate->setChecked(true);
    deleteTerminate->setToolTip(
        "すべての削除（Delete・ツールバー・削除ツール・右クリック）に適用します。削除した原子"
        "だけに付いていた水素も一緒に削除します。");
    cardLayout->addWidget(deleteTerminate);
    outer->addRow(card);
    auto *color = choices(outer, "色分け", {"元素", "残基"}, {"element", "residue"});
    color->setObjectName("moleculeColorMode");
    connect(color, &QComboBox::currentIndexChanged, this,
            [this, color] { view->setColorMode(value(color)); });
    // The atoms and bonds under the selection; transformations and attributes on pages of their own.
    auto *f = outer;
    QStringList symbols;
    for (const auto &e : elements())
        symbols << e.symbol;
    section(f, "元素");
    elementBox = choices(f, "追加・変更する元素", symbols);
    elementBox->setCurrentIndex(elementBox->findData("C"));
    elementBox->setObjectName("elementSelector");
    elementBox->setToolTip(
        "「配置」ツールで置く原子・配位中心の元素、「元素を変更」で選択原子に設定する元素です。"
        "パレットで原子・配位中心を選ぶと連動します。");
    enableWithSelection(button(f, "選択原子をこの元素に変更",
                               [this] {
                                   auto ids = requireSelection();
                                   QString s = value(elementBox);
                                   bool h = autoH->isChecked();
                                   edit("元素を変更", [=](Document &d) { replaceElement(d, ids, s, h); });
                               }),
                        1);
    // The coordinate table, when shown, edits the same values.
    positionCard = section(f, "座標（1原子）");
    positionCard->setVisible(moleculeLayout->currentData() == "view");
    for (int i = 0; i < 3; ++i) {
        positionFields[i] = number(f, QString("XYZ")[i] + QString(" / Å"));
        positionFields[i]->setSpecialValueText("—");
        enableWithSelection(positionFields[i], 1, 1);
    }
    enableWithSelection(button(f, "座標を適用",
                               [this] {
                                   auto id = requireSelection(1).first();
                                   Vec p = fields(positionFields);
                                   edit("座標を変更", [=](Document &d) {
                                       transform(d, {id}, Mat::Identity(), p - d.atom(id).position);
                                   });
                               }),
                        1, 1);
    enableWithSelection(button(f, "選択原子を原点へ",
                               [this] {
                                   auto ids = requireSelection(1);
                                   edit("原点へ移動", [=](Document &d) { orient(d, ids); });
                                   view->keepInView();
                               }),
                        1, 1);
    section(f, "配置・結合");
    auto *depth = number(f, "配置平面の奥行き / Å");
    depth->setToolTip("空白をクリックして原子・部品を置く平面の、画面奥行き方向の位置です（0: 画面中心）。");
    connect(depth, &QDoubleSpinBox::valueChanged, this, [this](double v) { view->setDepth(v); });
    bondKind = choices(f, "結合種類",
                       {"単結合", "二重結合", "三重結合", "芳香族", "配位（供与→中心）", "結合を削除"},
                       {"single", "double", "triple", "aromatic", "dative", "none"});
    enableWithSelection(button(f, "選択した2原子を結合",
                               [this] {
                                   auto ids = requireSelection(2);
                                   auto kind = value(bondKind);
                                   bool h = autoH->isChecked();
                                   edit("結合を変更", [=](Document &d) {
                                       d.setBond(ids[0], ids[1], kind);
                                       if (h)
                                           completeHydrogens(d, ids);
                                   });
                               }),
                        2, 2);
    auto *side = button(f, "結合の片側を選択", [this] {
        auto ids = requireSelection(2);
        setSelection(doc.bondSide(ids[0], ids[1]));
    });
    side->setToolTip("2原子の結合で分子を切り分け、後に選んだ原子の側をすべて選択します。");
    enableWithSelection(side, 2, 2);
    f = panel("変形");
    section(f, "平行移動（選択原子）");
    for (int i = 0; i < 3; ++i)
        translateFields[i] = number(f, QString("Δ") + QString("XYZ")[i] + " / Å");
    enableWithSelection(button(
                            f, "選択を平行移動",
                            [this] {
                                auto ids = requireSelection();
                                Vec v = fields(translateFields);
                                edit("平行移動", [=](Document &d) { transform(d, ids, Mat::Identity(), v); });
                            },
                            "translateSelection"),
                        1);
    section(f, "回転");
    // Rotation axis and mirror plane pass through the selected atoms' center, or the origin.
    auto *pivot = choices(f, "回転・鏡映の中心", {"選択の中心", "原点"}, {"selection", "origin"});
    pivot->setObjectName("transformPivot");
    const auto around = [pivot](const Document &d, const Ids &ids) {
        return value(pivot) == "origin" ? Vec(Vec::Zero()) : centroid(d, ids);
    };
    auto *axis = choices(f, "回転軸", {"Z", "X", "Y"}, {"2", "0", "1"});
    axis->setObjectName("rotationAxis");
    twist = number(f, "回転角 / °", 0, -360, 360);
    twist->setObjectName("rotationAngle");
    enableWithSelection(button(
                            f, "選択を回転",
                            [this, axis, around] {
                                auto ids = requireSelection();
                                int ax = value(axis).toInt();
                                double a = twist->value();
                                edit("選択を回転", [=](Document &d) {
                                    transform(d, ids, rotation(Vec::Unit(ax), a), Vec::Zero(),
                                              around(d, ids));
                                });
                            },
                            "rotateSelection"),
                        1);
    section(f, "立体");
    enableWithSelection(button(f, "不斉中心を反転",
                               [this] {
                                   auto id = requireSelection(1).first();
                                   edit("不斉中心を反転", [=](Document &d) { invertCenter(d, id); });
                               }),
                        1, 1);
    enableWithSelection(button(f, "E/Zを切替",
                               [this] {
                                   auto ids = requireSelection(2);
                                   edit("E/Zを切替", [=](Document &d) { switchEZ(d, ids); });
                               }),
                        2, 2);
    auto *stereo = button(f, "立体を記録（R/S・E/Z）",
                          [this] { edit("立体配置を記録", [](Document &d) { recordStereo(d); }); });
    stereo->setEnabled(hasStereochemistryBackend());
    if (!hasStereochemistryBackend())
        stereo->setToolTip("RDKitを含むビルドで使えます。");
    section(f, "向き・位置");
    auto *orientation = button(
        f, "向きをそろえる（原点・+X・XY）",
        [this] {
            auto ids = requireSelection();
            edit("向きをそろえる", [=](Document &d) { orient(d, ids); });
            view->keepInView();
        },
        "orientSelection");
    orientation->setToolTip("選択順に、1番目を原点、2番目を+X軸上、3番目をXY平面上へ動かします（分子全体）。"
                            "1原子ならその原子が原点に来るよう平行移動します。");
    enableWithSelection(orientation, 1, 3);
    button(f, "重心を原点へ", [this] {
        auto ids = selected;
        edit("重心を原点へ", [=](Document &d) { centerMass(d, ids, false); });
        view->keepInView();
    });
    button(f, "主慣性軸へ整列", [this] {
        auto ids = selected;
        edit("主慣性軸へ整列", [=](Document &d) { centerMass(d, ids, true); });
        view->keepInView();
    });
    section(f, "鏡映・複製");
    auto *plane = choices(f, "鏡映面", {"YZ", "XZ", "XY"}, {"0", "1", "2"});
    plane->setObjectName("mirrorPlane");
    auto *copy = new QCheckBox("鏡像の写しを作る");
    copy->setObjectName("mirrorCopy");
    f->addRow(copy);
    auto *mirrorButton = button(
        f, "指定平面で鏡映",
        [this, plane, copy, around] {
            auto ids = requireSelection();
            int ax = value(plane).toInt();
            bool makeCopy = copy->isChecked();
            edit("鏡映", [=](Document &d) {
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
    for (int i = 0; i < 3; ++i)
        duplicateFields[i] = number(f, QString("複製 ") + QString("XYZ")[i] + " / Å", i == 0 ? 3 : 0);
    auto *duplicateButton = button(
        f, "指定間隔で複製",
        [this] {
            auto ids = requireSelection();
            Vec v = fields(duplicateFields);
            edit("複製", [=](Document &d) { duplicate(d, ids, v); });
            view->keepInView();
        },
        "duplicateSelection");
    enableWithSelection(duplicateButton, 1);
    f = panel("属性");
    section(f, "原子の属性");
    formal = new QSpinBox;
    formal->setRange(-99, 20);
    formal->setSpecialValueText("変更なし");
    f->addRow("形式電荷", formal);
    radical = new QSpinBox;
    radical->setRange(-1, 8);
    radical->setSpecialValueText("変更なし");
    f->addRow("不対電子", radical);
    formal->setToolTip("水素補完などの構造準備に使用します。計算の全電荷は「2 計算条件」で設定します。");
    radical->setToolTip("原子のラジカル属性です。計算のスピン多重度は「2 計算条件」で設定します。");
    mass = new QLineEdit;
    mass->setPlaceholderText("整数: 質量数 / 小数: amu");
    f->addRow("同位体 / 質量", mass);
    for (QWidget *field : std::initializer_list<QWidget *>{formal, radical, mass})
        enableWithSelection(field, 1);
    auto *attributes = button(f, "属性を適用", [this] {
        auto ids = requireSelection();
        int q = formal->value(), r = radical->value();
        QString m = mass->text().trimmed();
        edit("原子属性を変更", [=](Document &d) {
            for (auto id : ids) {
                auto &a = d.atom(id);
                if (q != -99)
                    a.charge = q;
                if (r != -1)
                    a.radical = r;
                if (!m.isEmpty() || ids.size() == 1) {
                    a.isotope = 0;
                    a.mass = 0;
                    if (!m.isEmpty()) {
                        bool ok;
                        a.isotope = m.toInt(&ok);
                        if (!ok) {
                            a.isotope = 0;
                            a.mass = m.toDouble(&ok);
                            if (!ok || !std::isfinite(a.mass) || a.mass <= 0)
                                fail("質量は正の値にしてください。");
                        } else if (a.isotope <= 0)
                            fail("質量数は正の整数にしてください。");
                    }
                }
            }
            d.invalidateAssignments();
        });
    });
    enableWithSelection(attributes, 1);
    section(f, "ゴースト・固定");
    // The names of the coordinate table and the 3D view's menu, for the same edits.
    for (bool ghost : {true, false}) {
        auto *b = button(
            f, ghost ? "ゴーストにする" : "ゴーストを解除",
            [this, ghost] {
                auto ids = requireSelection();
                edit(ghost ? "選択原子をゴーストにする" : "選択原子のゴーストを解除", [=](Document &d) {
                    for (auto id : ids)
                        d.atom(id).ghost = ghost;
                    d.invalidateAssignments();
                });
            },
            ghost ? "ghostSelectedAtoms" : "unghostSelectedAtoms");
        b->setToolTip("基底関数だけを置く原子（NWChemのbq）です。BSSE補正などで使います。");
        enableWithSelection(b, 1);
    }
    for (bool fixed : {true, false}) {
        auto *b = button(
            f, fixed ? "選択原子を固定" : "固定を解除",
            [this, fixed] {
                auto ids = requireSelection();
                edit(fixed ? "選択原子を固定" : "選択原子の固定を解除", [=](Document &d) {
                    for (auto id : ids)
                        d.atom(id).fixed = fixed;
                });
            },
            fixed ? "fixSelectedAtoms" : "unfixSelectedAtoms");
        b->setToolTip("編集時の移動・削除を防ぎ、NWChemの構造最適化でも座標を固定します。");
        enableWithSelection(b, 1);
    }
    section(f, "並べ替え");
    auto *order =
        choices(f, "原子の並べ替え", {"元素順", "残基順", "選択を先頭"}, {"element", "residue", "selected"});
    button(f, "原子を並べ替え", [this, order] {
        auto mode = value(order);
        auto ids = selected;
        edit("原子を並べ替え", [=](Document &d) {
            QMap<QString, int> ranks;
            int n = 0;
            for (auto v : d.data["residues"].toArray())
                for (auto id : v.toObject()["atoms"].toObject())
                    ranks[id.toString()] = n++;
            std::stable_sort(d.atoms.begin(), d.atoms.end(), [&](const Atom &a, const Atom &b) {
                if (mode == "element")
                    return element(a.element).number < element(b.element).number;
                if (mode == "residue")
                    return ranks.value(a.id, 1000000) < ranks.value(b.id, 1000000);
                return ids.contains(a.id) > ids.contains(b.id);
            });
        });
    });
    section(f, "マイ部品");
    enableWithSelection(button(f, "選択原子をマイ部品に保存…", [this] { savePart(); }), 1);
    button(f, "マイ部品を読込…", [this] { loadPart(); });
    button(f, "選択中のマイ部品を一覧から外す", [this] {
        auto t = currentTemplate();
        if (!t["id"].toString().startsWith("my:"))
            fail("マイ部品をパレットで選択してください。");
        for (int i = customParts.size() - 1; i >= 0; --i)
            if (customParts[i].toObject()["id"] == t["id"])
                customParts.removeAt(i);
        settings.setValue("parts", customParts);
        refreshPalette();
    });
    section(f, "整形");
    auto *local =
        button(f, "局所整形（選択原子だけ移動）", [this] { runWorker("refine", {}, requireSelection()); });
    local->setToolTip("力場で選択原子だけを動かして整えます。結果は候補として表示します。");
    enableWithSelection(local, 1);
    button(f, "3Dを再生成", [this] {
        runWorker("regenerate");
    })->setToolTip("結合から3D座標を作り直し、力場で整えます。結果は候補として表示します。");
    section(f, "読込時の保留", 0);
    button(f, "読込時に保留した設定を確認…", [this] {
        QDialog dialog(this);
        dialog.setWindowTitle("原子参照と保留設定");
        dialog.resize(680, 500);
        auto *l = new QVBoxLayout(&dialog);
        auto *text = new QPlainTextEdit(
            QString::fromUtf8(QJsonDocument(QJsonObject{{"references", doc.data["references"]},
                                                        {"invalid_settings", doc.data["invalid_settings"]}})
                                  .toJson()));
        text->setReadOnly(true);
        l->addWidget(text);
        auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
        l->addWidget(close);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
    });
}
QString MainWindow::selectedResidue() const {
    for (auto id : selected) {
        auto rid = residueForAtom(displayDocument(), id);
        if (!rid.isEmpty())
            return rid;
    }
    if (residueList->currentItem())
        return residueList->currentItem()->data(Qt::UserRole).toString();
    fail("対象の残基を選択してください。");
}
QString MainWindow::selectedPart() const {
    for (auto id : selected)
        for (auto v : displayDocument().data["parts"].toArray())
            if (idsFromJson(v.toObject()["atoms"]).contains(id))
                return v.toObject()["id"].toString();
    fail("部品を選択してください。");
}
QString MainWindow::selectedCenter() const {
    const auto &d = displayDocument();
    for (auto id : selected) {
        for (auto v : d.data["centers"].toArray())
            if (v.toObject()["atom"] == id)
                return id;
        for (auto v : d.data["parts"].toArray()) {
            auto p = v.toObject();
            if (idsFromJson(p["atoms"]).contains(id)) {
                auto center = p["metadata"].toObject()["coordination_center"].toString();
                if (!center.isEmpty())
                    return center;
            }
        }
    }
    fail("配位中心または配位子を選択してください。");
}
QList<int> MainWindow::sites() const {
    auto indices = integers(siteInput->text());
    for (auto &i : indices)
        --i;
    return indices;
}
void MainWindow::buildCoordinationPanel() {
    auto *f = panel("配位");
    auto *intro = new QLabel("配位中心（金属）またはその配位子を3D画面で選んでから操作します。"
                             "空いているサイトは紫の番号で表示します。");
    intro->setWordWrap(true);
    intro->setProperty("caption", true);
    f->addRow(intro);
    section(f, "配位形状");
    geometryBox = choices(f, "配位形状", geometryLabels(), geometryNames());
    geometryBox->setCurrentIndex(6);
    distance = number(f, "中心–供与原子 / Å", 2.1, .1, 20);
    oxidation = new QSpinBox;
    oxidation->setRange(-8, 12);
    oxidation->setValue(2);
    oxidation->setToolTip("構造準備の補助情報です。計算の全電荷・多重度は「2 計算条件」で設定します。");
    f->addRow("酸化数（補助）", oxidation);
    button(f, "配位形状・距離を適用", [this] {
        auto id = selectedCenter(), g = value(geometryBox);
        double dist = distance->value();
        int ox = oxidation->value();
        edit("配位形状を変更", [=](Document &d) { adjustCenter(d, id, g, dist, ox); }, true);
    });
    button(f, "酸化数だけを適用", [this] {
        auto id = selectedCenter();
        int ox = oxidation->value();
        edit("酸化数を変更", [=](Document &d) {
            auto c = center(d, id);
            c["oxidation"] = ox;
            updateCenter(d, c);
        });
    });
    section(f, "配位子の配置");
    siteInput = new QLineEdit("1");
    siteInput->setPlaceholderText("例: 1 または 1,2");
    siteInput->setToolTip("3D画面の紫の番号です。サイトをクリックしても入力されます。"
                          "二座配位子は2サイトをカンマで区切ります。");
    f->addRow("サイト番号", siteInput);
    button(f, "配位子パレットを開く", [this] { setPaletteCategory("ligand"); });
    button(f, "選択サイトへ配置（候補）", [this] {
        QString c = selectedCenter();
        auto idx = sites();
        auto t = currentTemplate();
        double angle = twist->value();
        edit("配位子を配置", [=](Document &d) { placeLigand(d, c, idx, t, angle); }, true);
    })->setProperty("primary", true);
    button(f, "配位子を交換", [this] {
        QString p = selectedPart();
        auto idx = sites();
        auto t = currentTemplate();
        edit("配位子を交換", [=](Document &d) { moveLigand(d, p, idx, t); }, true);
    })->setToolTip("選択した配位子を、パレットで選んだ配位子に置き換えます。");
    button(f, "配位子を指定サイトへ移動", [this] {
        QString p = selectedPart();
        auto idx = sites();
        edit("配位子を移動", [=](Document &d) { moveLigand(d, p, idx); }, true);
    });
    button(f, "2サイトの配位子を交換", [this] {
        auto id = selectedCenter();
        auto indices = sites();
        if (indices.size() != 2)
            fail("2サイトを指定してください。");
        edit("配位子のサイトを交換", [=](Document &d) { swapLigands(d, id, indices[0], indices[1]); }, true);
    })->setToolTip("「サイト番号」に2つの番号（例: 1,3）を入れて、そのサイトの配位子を入れ替えます。");
    section(f, "サイトの方向", 0);
    auto *dx = number(f, "サイト方向 X"), *dy = number(f, "サイト方向 Y"), *dz = number(f, "サイト方向 Z", 1);
    button(f, "個別サイトXYZ方向を適用", [this, dx, dy, dz] {
        auto id = selectedCenter();
        auto indices = sites();
        Vec dir(dx->value(), dy->value(), dz->value());
        double dist = distance->value();
        edit(
            "配位サイトの方向を変更", [=](Document &d) { setSiteDirection(d, id, indices, dir, dist); },
            true);
    });
    auto *info =
        new QLabel("酸化数は構造準備の補助情報です。計算の全電荷・多重度は「2 計算条件」で設定します。");
    info->setWordWrap(true);
    info->setProperty("caption", true);
    pageCards.value(f)->insertWidget(pageCards.value(f)->count() - 1, info);
}
void MainWindow::addAmino(const QString &rid, bool n) {
    auto t = currentTemplate();
    if (t["category"] != "amino")
        fail("アミノ酸をパレットで選択してください。");
    QString code = t["code"].toString(), state = t["state"].toString("default"), ch = value(chiralityBox),
            nt = value(nTerm), ct = value(cTerm), result;
    edit("末端に残基を追加", [&](Document &d) {
        int before = d.formalCharge();
        result =
            n ? prependResidue(d, code, rid, state, ch, nt) : appendResidue(d, code, rid, state, ch, nt, ct);
        if (d.job()["charge"].toInt() == before)
            d.setJobValue("charge", d.formalCharge());
    });
    growthResidue = result;
    growthN = n;
    view->keepInView();
}
void MainWindow::buildResiduePanel() {
    auto *f = panel("残基");
    section(f, "PDB・残基の認識");
    button(f, "PDBを読み込む…", [this] { open(); });
    button(f, "PDBの診断・修復…", [this] { pdbPreparation(); }, "preparePdb");
    button(f, "構造を検査…", [this] { showReport(); });
    button(f, "結合グラフから残基を認識", [this] {
        edit("残基を認識", [](Document &d) { recognizeResidues(d); }, true);
    })->setEnabled(hasStereochemistryBackend());
    button(f, "選択残基の欠損原子を補完", [this] {
        auto rid = selectedResidue();
        edit("欠損側鎖・水素を補完", [=](Document &d) { repairResidue(d, rid); }, true);
    });
    section(f, "配列から構築");
    sequenceInput = new QLineEdit;
    sequenceInput->setObjectName("sequenceInput");
    sequenceInput->setPlaceholderText("AGS / ALA-GLY-SER");
    f->addRow("N → C 配列", sequenceInput);
    chiralityBox = choices(f, "立体", {"L", "D"});
    aminoState = choices(f, "側鎖状態", {"default", "neutral", "anion", "HID", "HIE", "HIP"});
    nTerm = choices(f, "N末端", {"プロトン化", "非荷電", "ACE"}, {"charged", "neutral", "ACE"});
    cTerm = choices(f, "C末端", {"COO−", "COOH", "NME"}, {"charged", "neutral", "NME"});
    auto *replace = new QCheckBox("既存の分子を置き換える");
    f->addRow(replace);
    button(
        f, "配列から構築",
        [this, replace] {
            auto codes = parseSequence(sequenceInput->text());
            auto n = value(nTerm), c = value(cTerm), ch = value(chiralityBox), state = value(aminoState);
            bool clear = replace->isChecked();
            edit(
                "配列から構築",
                [=](Document &d) {
                    if (clear) // Keep calculation/execution settings and the project envelope.
                        replaceCoordinateStructure(d, Document());
                    auto old = d.ids();
                    double shift = 0;
                    for (const auto &a : d.atoms)
                        shift = std::max(shift, a.position.x() + 6);
                    buildPeptide(d, codes, n, c, ch, state);
                    if (!old.isEmpty()) {
                        Ids added;
                        for (auto id : d.ids())
                            if (!old.contains(id))
                                added << id;
                        transform(d, added, Mat::Identity(), Vec(shift, 0, 0));
                    }
                    if (clear)
                        d.setJobValue("charge", d.formalCharge());
                },
                true);
            view->keepInView();
        },
        "buildSequence")
        ->setProperty("primary", true);
    section(f, "残基の編集（パレットのアミノ酸を使用）");
    button(f, "選択C末端へ残基を追加", [this] { addAmino(selectedResidue(), false); });
    button(f, "選択N末端へ残基を追加", [this] { addAmino(selectedResidue(), true); });
    button(f, "選択残基を置換", [this] {
        auto rid = selectedResidue();
        auto t = currentTemplate();
        if (t["category"] != "amino")
            fail("アミノ酸を選択してください。");
        QString code = t["code"].toString(), state = value(aminoState), ch = value(chiralityBox);
        edit("残基を置換", [=](Document &d) { mutateResidue(d, rid, code, state, ch); }, true);
    });
    button(f, "選択した鎖の両末端へ適用", [this] {
        auto rid = selectedResidue();
        QString n = value(nTerm), c = value(cTerm);
        edit(
            "末端状態を変更",
            [=](Document &d) {
                auto r = residue(d, rid);
                auto seq = idsFromJson(d.data["chains"].toObject()[r["chain"].toString()]);
                if (seq.isEmpty())
                    fail("鎖情報がありません。");
                int old = d.formalCharge();
                setTerminals(d, seq.first(), n, {});
                setTerminals(d, seq.last(), {}, c);
                if (d.job()["charge"].toInt() == old)
                    d.setJobValue("charge", d.formalCharge());
            },
            true);
    });
    button(f, "残基を削除（鎖を分割）", [this] {
        auto rid = selectedResidue();
        edit("残基を削除", [=](Document &d) { deleteResidue(d, rid); }, true);
    });
    button(f, "選択した2末端を再連結", [this] {
        Ids residues;
        for (auto id : requireSelection()) {
            auto rid = residueForAtom(doc, id);
            if (!rid.isEmpty() && !residues.contains(rid))
                residues << rid;
        }
        if (residues.size() != 2)
            fail("C末端側、N末端側の順に2残基を選択してください。");
        edit("鎖を再連結", [=](Document &d) { reconnectResidues(d, residues[0], residues[1]); }, true);
    });
    section(f, "主鎖・側鎖の二面角");
    auto *torsion = choices(f, "二面角", {"phi", "psi", "omega", "chi1", "chi2", "chi3", "chi4", "chi5"});
    auto *angle = number(f, "角度 / °", 180, -360, 360);
    button(f, "二面角を適用", [this, torsion, angle] {
        auto rid = selectedResidue(), kind = value(torsion);
        double a = angle->value();
        edit("残基二面角を変更", [=](Document &d) { setResidueTorsion(d, rid, kind, a); });
    });
    section(f, "残基の一覧");
    residueList = new QListWidget;
    residueList->setMaximumHeight(200);
    residueList->setObjectName("residueList");
    f->addRow(residueList);
    connect(residueList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        safe([this, item] {
            Ids ids;
            for (auto v : residue(displayDocument(), item->data(Qt::UserRole).toString())["atoms"].toObject())
                ids << v.toString();
            setSelection(ids);
        });
    });
    button(f, "未登録の原子を選択", [this] {
        Ids ids;
        for (const auto &a : doc.atoms)
            if (residueForAtom(doc, a.id).isEmpty())
                ids << a.id;
        setSelection(ids);
    });
    button(f, "選択原子を残基に登録…", [this] {
        auto ids = requireSelection();
        bool ok;
        QString code =
            QInputDialog::getText(this, "残基を登録", "3文字コード", QLineEdit::Normal, "UNK", &ok);
        if (ok)
            edit("残基を登録", [=](Document &d) { registerResidue(d, ids, code.toUpper()); });
    });
    button(f, "原子対応表をCSVで書き出す…", [this] {
        if (!resolveCandidate())
            return;
        QString p = QFileDialog::getSaveFileName(this, "対応表を保存", "atoms.csv", "CSV (*.csv)");
        if (p.isEmpty())
            return;
        QString csv = "index,element,atom_name,residue\n";
        auto quote = [](QString s) {
            s.replace('"', "\"\"");
            return '"' + s + '"';
        };
        for (int i = 0; i < doc.atoms.size(); ++i) {
            const auto &a = doc.atoms[i];
            auto rid = residueForAtom(doc, a.id);
            QString name = rid.isEmpty() ? "" : residue(doc, rid)["code"].toString();
            csv += QString("%1,%2,%3,%4\n").arg(i + 1).arg(quote(a.element), quote(a.name), quote(name));
        }
        writeFile(p, csv.toUtf8());
    });
}
void MainWindow::buildChargePanel() {
    auto *f = panel("電荷");
    chargeEditor = f->parentWidget()->parentWidget();
    // The total charge and multiplicity are settings of "2 計算条件": here they are shown with the
    // formal charges of the atoms and the electrons they give, with the way there.
    section(f, "分子の電荷");
    chargeState = new QLabel;
    chargeState->setObjectName("moleculeChargeState");
    auto *chargeLink = linkButton("② 計算条件で変更", "moleculeChargeLink");
    connect(chargeLink, &QPushButton::clicked, this, [this] {
        workspaceTabs->setCurrentWidget(calculation);
        calculation->showField("charge");
    });
    auto *chargeRow = new QWidget;
    auto *chargeRowLayout = new QHBoxLayout(chargeRow);
    chargeRowLayout->setContentsMargins(0, 0, 0, 0);
    chargeRowLayout->setSpacing(10);
    chargeRowLayout->addWidget(chargeState);
    chargeRowLayout->addWidget(chargeLink);
    chargeRowLayout->addStretch();
    f->addRow("全電荷・多重度", chargeRow);
    chargeSummary = new QLabel;
    chargeSummary->setObjectName("moleculeChargeSummary");
    chargeSummary->setTextFormat(Qt::PlainText);
    chargeSummary->setWordWrap(true);
    f->addRow(chargeSummary);
    inputState = new QLabel;
    inputState->setObjectName("moleculeInputState");
    inputState->setWordWrap(true);
    f->addRow(inputState);
    button(
        f, "形式電荷の合計を全電荷に適用",
        [this] { edit("全電荷を合わせる", [](Document &d) { d.setJobValue("charge", d.formalCharge()); }); },
        "applyMoleculeFormalCharge")
        ->setToolTip("原子の形式電荷の合計を、計算の全電荷（「2 計算条件」）にします。");
    auto *preview = linkButton("④ 入力プレビューで確認", "moleculeInputPreview");
    connect(preview, &QPushButton::clicked, this, [this] { workspaceTabs->setCurrentWidget(inputPreview); });
    f->addRow(preview);
    section(f, "外部点電荷");
    pointChargeState = new QLabel;
    pointChargeState->setObjectName("moleculePointChargeState");
    auto *pointLink = linkButton("② 計算条件で設定", "moleculePointChargeLink");
    pointLink->setToolTip("点電荷をNWChem入力に含めるかは「2 計算条件」の「周囲の環境」で設定します。");
    connect(pointLink, &QPushButton::clicked, this, [this] {
        workspaceTabs->setCurrentWidget(calculation);
        calculation->showField("include_point_charges");
    });
    auto *pointRow = new QWidget;
    auto *pointRowLayout = new QHBoxLayout(pointRow);
    pointRowLayout->setContentsMargins(0, 0, 0, 0);
    pointRowLayout->setSpacing(10);
    pointRowLayout->addWidget(pointChargeState);
    pointRowLayout->addWidget(pointLink);
    pointRowLayout->addStretch();
    f->addRow(pointRow);
    pointList = new QListWidget;
    pointList->setObjectName("pointChargeList");
    pointList->setMaximumHeight(160);
    f->addRow(pointList);
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
        auto *label = new QLabel(i < 3 ? QString("XYZ")[i] + QString(" / Å") : QString("電荷 / e"));
        label->setProperty("caption", true);
        label->setBuddy(spin);
        fieldGrid->addWidget(label, (i / 2) * 2, i % 2);
        fieldGrid->addWidget(spin, (i / 2) * 2 + 1, i % 2);
    }
    chargeValue->setValue(.5);
    f->addRow(fieldsBox);
    connect(pointList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        for (auto v : displayDocument().data["point_charges"].toArray()) {
            auto q = v.toObject();
            if (q["id"] == item->data(Qt::UserRole).toString()) {
                Vec p = vectorFromJson(q["position"]);
                for (int i = 0; i < 3; ++i)
                    chargeFields[i]->setValue(p[i]);
                chargeValue->setValue(q["charge"].toDouble());
            }
        }
    });
    button(
        f, "点電荷を追加",
        [this] {
            Vec p = fields(chargeFields);
            double q = chargeValue->value();
            edit("点電荷を追加", [=](Document &d) {
                auto charges = d.data["point_charges"].toArray();
                charges.append(QJsonObject{{"id", newId()},
                                           {"position", vectorToJson(p)},
                                           {"charge", q},
                                           {"source", "manual"},
                                           {"source_unit", "angstrom"}});
                d.data["point_charges"] = charges;
            });
        },
        "addPointCharge")
        ->setProperty("primary", true);
    button(
        f, "点電荷を更新",
        [this] {
            if (!pointList->currentItem())
                fail("点電荷を選択してください。");
            QString id = pointList->currentItem()->data(Qt::UserRole).toString();
            Vec p = fields(chargeFields);
            double q = chargeValue->value();
            edit("点電荷を更新", [=](Document &d) {
                auto charges = d.data["point_charges"].toArray();
                for (int i = 0; i < charges.size(); ++i) {
                    auto c = charges[i].toObject();
                    if (c["id"] == id) {
                        c["position"] = vectorToJson(p);
                        c["charge"] = q;
                        charges[i] = c;
                    }
                }
                d.data["point_charges"] = charges;
            });
        },
        "updatePointCharge");
    button(
        f, "点電荷を削除",
        [this] {
            if (!pointList->currentItem())
                fail("点電荷を選択してください。");
            QString id = pointList->currentItem()->data(Qt::UserRole).toString();
            edit("点電荷を削除", [=](Document &d) {
                auto charges = d.data["point_charges"].toArray();
                for (int i = charges.size() - 1; i >= 0; --i)
                    if (charges[i].toObject()["id"] == id)
                        charges.removeAt(i);
                d.data["point_charges"] = charges;
            });
        },
        "deletePointCharge");
    enableWithSelection(
        button(f, "選択した水をTIP3P点電荷へ",
               [this] {
                   auto ids = requireSelection();
                   edit("水を点電荷へ変換", [=](Document &d) { waterToCharges(d, ids); }, true);
               }),
        1);
    button(f, "点電荷をファイルに書き出す…", [this] {
        if (!resolveCandidate())
            return;
        QString p =
            QFileDialog::getSaveFileName(this, "点電荷を書き出す", "charges.dat", "点電荷 (*.dat *.txt)");
        if (p.isEmpty())
            return;
        QString text = "# MolForma point charges; units=angstrom; x y z q\n";
        for (auto v : doc.data["point_charges"].toArray()) {
            auto q = v.toObject();
            Vec xyz = vectorFromJson(q["position"]);
            text += QString("%1 %2 %3 %4\n")
                        .arg(xyz.x(), 0, 'f', 10)
                        .arg(xyz.y(), 0, 'f', 10)
                        .arg(xyz.z(), 0, 'f', 10)
                        .arg(q["charge"].toDouble(), 0, 'g', 15);
        }
        writeFile(p, text.toUtf8());
    });
}
void MainWindow::refreshChargePanel() {
    const auto &d = displayDocument();
    chargeState->setText(
        QString("%1 · 多重度 %2").arg(d.job()["charge"].toInt()).arg(d.job()["multiplicity"].toInt(1)));
    pointChargeState->setText(nwchemSettings(d)["include_point_charges"].toBool() ? "NWChem入力に含める"
                                                                                  : "NWChem入力に含めない");
    chargeEditor->setEnabled(!candidate && !worker);
    const auto check = validateNwchem(d);
    QStringList text{QString("形式電荷の合計 %1 · 電子数 %2%3")
                         .arg(d.formalCharge())
                         .arg(check.electrons)
                         .arg(check.ecpElectrons ? "（ECP適用後）" : "")};
    if (check.errors.isEmpty())
        text << QString("α %1 / β %2").arg(check.alpha).arg(check.beta);
    for (const auto &error : check.errors)
        text << "エラー: " + error;
    for (const auto &warning : check.warnings)
        text << "確認: " + warning;
    if (candidate)
        text.prepend("未確定候補です。確定・取消後に設定を変更できます。");
    else if (worker)
        text.prepend("分子の整形中です。終了後に設定を変更できます。");
    chargeSummary->setText(text.join('\n'));
    // The outcome of the checks as the cards of the settings tabs say it.
    const bool ok = check.errors.isEmpty();
    inputState->setText(ok ? QString("✓ 入力を生成できます")
                           : QString("入力を生成できません（エラー %1 件）").arg(check.errors.size()));
    if (inputState->property("good").toBool() != ok) {
        inputState->setProperty("good", ok);
        inputState->setProperty("bad", !ok);
        repolish(inputState);
    }
}
void MainWindow::pdbPreparation() {
    if (candidate)
        fail("先に候補を確定または取消してください。");
    if (doc.atoms.isEmpty())
        fail("PDBファイルを開いてから診断してください。");
    QDialog dialog(this);
    dialog.setWindowTitle("PDBの診断・修復");
    dialog.resize(820, 620);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel("残基を選ぶと診断を表示します。修復は3D画面で候補を確認して確定します。");
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *list = new QListWidget;
    layout->addWidget(list);
    for (auto v : doc.data["residues"].toArray()) {
        auto r = v.toObject();
        auto *item = new QListWidgetItem(QString("%1 %2  ·  %3 / %4")
                                             .arg(r["code"].toString())
                                             .arg(r["number"].toInt())
                                             .arg(r["n_terminal"].toString(), r["c_terminal"].toString()),
                                         list);
        item->setData(Qt::UserRole, r["id"].toString());
    }
    auto *report = new QPlainTextEdit;
    report->setReadOnly(true);
    report->setPlainText(structureReport(doc));
    layout->addWidget(report, 1);
    connect(list, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem *item) {
        if (!item)
            return;
        safe([&] {
            report->setPlainText(residueDiagnostics(doc, item->data(Qt::UserRole).toString()).join('\n'));
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
    add("3Dで選択箇所を見る", [&] {
        if (!list->currentItem())
            fail("残基を選んでください。");
        Ids ids;
        for (auto id : residue(doc, list->currentItem()->data(Qt::UserRole).toString())["atoms"].toObject())
            ids << id.toString();
        setSelection(ids);
        dialog.accept();
    });
    add("修復候補を作成", [&] {
        if (!list->currentItem())
            fail("残基を選んでください。");
        auto rid = list->currentItem()->data(Qt::UserRole).toString();
        edit("PDBの欠損原子を補完", [=](Document &d) { repairResidue(d, rid); }, true);
        dialog.accept();
    });
    add("残基を再認識", [&] {
        edit("PDBの残基を認識", [](Document &d) { recognizeResidues(d); }, true);
        dialog.accept();
    })->setEnabled(hasStereochemistryBackend());
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
} // namespace mf
