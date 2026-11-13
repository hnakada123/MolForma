// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/part_palette.h"
#include "editing/library.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "model/elements.h"
#include "model/settings.h"
#include "ui/chrome_layouts.h"
#include "ui/line_icons.h"
#include "ui/periodic_table.h"
#include "ui/template_delegate.h"
#include <QDrag>
#include <QJsonDocument>
#include <QMimeData>
#include <QtWidgets>

namespace mf {
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

PartPalette::PartPalette(EditorContext &editorServices, UiServices &uiServices, QWidget *parent)
    : QWidget(parent), editorContext(editorServices), uiContext(uiServices) {
    customParts = uiContext.preference(Preference::customParts).toJsonArray();
    // The palette: search, categories, the common elements and the parts as tiles.
    setObjectName("paletteColumn");
    setAttribute(Qt::WA_StyledBackground);
    setMinimumWidth(230); // Three columns of tiles and the element buttons.
    setMaximumWidth(360);
    auto *ll = new QVBoxLayout(this);
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
        connect(b, &QToolButton::clicked, this,
                [this, symbol] { uiContext.safe([&] { chooseElement(symbol); }); });
    }
    periodicTableButton = new QPushButton(tr("周期表…"));
    periodicTableButton->setObjectName("openPeriodicTable");
    periodicTableButton->setToolTip(tr("周期表から原子を選択します（H〜Og）。"));
    periodicTableButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    quick->addWidget(periodicTableButton, 1, 4, 1, 2);
    for (int c = 0; c < 6; ++c)
        quick->setColumnStretch(c, 1);
    connect(periodicTableButton, &QPushButton::clicked, this, [this] {
        PeriodicTableDialog dialog(category() == "center" && !centerElement.isEmpty() ? centerElement
                                                                                      : paletteElement,
                                   search->text(), this);
        if (dialog.exec() == QDialog::Accepted)
            uiContext.safe([&] { chooseElement(dialog.selectedElement()); });
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
    selectionControls.add(savePartButton, 1);
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
    connect(savePartButton, &QPushButton::clicked, this, [this] { uiContext.safe([this] { savePart(); }); });
    connect(loadPartButton, &QPushButton::clicked, this, [this] { uiContext.safe([this] { loadPart(); }); });
    connect(removePartButton, &QPushButton::clicked, this,
            [this] { uiContext.safe([this] { removePart(); }); });
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
    connect(search, &QLineEdit::textChanged, this, &PartPalette::refresh);
    connect(categoryGroup, &QButtonGroup::buttonToggled, this, [this](QAbstractButton *, bool on) {
        if (on)
            refresh();
    });
    connect(palette, &QListWidget::itemClicked, this, [this] { uiContext.safe([this] { choose(); }); });
    // Enter in the search box takes the first match, like a click on it.
    connect(search, &QLineEdit::returnPressed, this, [this] {
        uiContext.safe([this] {
            selectFirstPart();
            if (palette->currentItem())
                choose();
        });
    });
    connect(palette, &QListWidget::currentItemChanged, this, &PartPalette::choiceChanged);
}

void PartPalette::refresh() {
    QString cat = category(), needle = search->text().trimmed();
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
    emit refreshed();
}

void PartPalette::selectFirstPart() {
    for (int i = 0; i < palette->count(); ++i)
        if (palette->item(i)->flags() & Qt::ItemIsSelectable) {
            palette->setCurrentRow(i);
            return;
        }
}

QString PartPalette::category() const {
    return categoryGroup->checkedButton() ? categoryGroup->checkedButton()->property("category").toString()
                                          : QString("all");
}

void PartPalette::setCategory(const QString &code) {
    for (auto *b : categoryGroup->buttons())
        if (b->property("category") == code)
            b->setChecked(true);
}

void PartPalette::chooseElement(const QString &symbol) {
    // Under "配位中心" the element buttons and the periodic table choose the center element; from the
    // other part categories they go to "原子", whose list holds the element.
    if (!QStringList{"all", "atom", "center"}.contains(category()))
        setCategory("atom");
    const bool center = category() == "center";
    (center ? centerElement : paletteElement) = element(symbol).symbol;
    search->clear();
    refresh();
    for (int i = 0; i < palette->count(); ++i)
        if (palette->item(i)->data(Qt::UserRole) ==
            paletteKey(center ? PaletteKind::Center : PaletteKind::Atom, element(symbol).symbol))
            palette->setCurrentRow(i);
    choose();
}

QJsonObject PartPalette::currentTemplate() const {
    if (!palette->currentItem())
        fail(tr("左のパレットから部品を選択してください。"));
    QString id = palette->currentItem()->data(Qt::UserRole).toString();
    for (auto t : customParts)
        if (t.toObject()["id"] == id)
            return t.toObject();
    return findTemplate(id);
}

PaletteEntry PartPalette::choice() const {
    const auto *item = palette->currentItem();
    return item ? paletteEntry(item->data(Qt::UserRole).toString(),
                               item->data(TemplateDelegate::TemplateRole).toJsonObject())
                : PaletteEntry();
}

bool PartPalette::hasChoice() const {
    return palette->currentItem();
}

QString PartPalette::choiceText() const {
    return palette->currentItem() ? palette->currentItem()->text() : QString();
}

void PartPalette::showChoice(bool shown) {
    if (!shown)
        palette->clearSelection();
    else if (palette->currentItem())
        palette->currentItem()->setSelected(true);
}

void PartPalette::select(const QString &key) {
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
        setCategory("all");
        refresh();
        if (!select())
            fail(tr("部品が見つかりません: %1").arg(key));
    }
}

void PartPalette::choose() {
    if (!palette->currentItem())
        return;
    // The element of an atom or a center stays the one listed and marked; a SMILES is built from the
    // search, which is cleared.
    const auto entry = choice();
    if (entry.kind == PaletteKind::Smiles)
        search->clear();
    else if (entry.kind == PaletteKind::Atom)
        paletteElement = entry.value;
    else if (entry.kind == PaletteKind::Center)
        centerElement = entry.value;
    emit chosen(entry);
}

bool PartPalette::growing() const {
    return growth->isChecked();
}

void PartPalette::stopGrowing() {
    growth->setChecked(false);
}

void PartPalette::refreshSelection() {
    selectionControls.refresh(editorContext.selection().size());
}

void PartPalette::savePreferences() {
    uiContext.setPreference(Preference::customParts, customParts);
}

void PartPalette::savePart() {
    auto ids = requiredSelection(editorContext.selection());
    bool ok;
    QString name = QInputDialog::getText(this, tr("マイ部品"), tr("部品名"), QLineEdit::Normal, "", &ok);
    if (!ok)
        return;
    const QStringList kinds{tr("置換基（接続点1つ）"), tr("配位子"), tr("骨格（接続点なし・空白に配置）")};
    QString kind = QInputDialog::getItem(
        this, tr("部品の種類"), tr("種類（置換基・配位子は接続点の原子を最初に選択）"), kinds, 0, false, &ok);
    if (!ok)
        return;
    auto t = makePart(editorContext.document(), ids, name,
                      kind == kinds[1]   ? "ligand"
                      : kind == kinds[2] ? "skeleton"
                                         : "substituent");
    QString p = QFileDialog::getSaveFileName(this, tr("部品を保存"), name + ".molforma-part.json",
                                             tr("部品 (*.molforma-part.json)"));
    if (p.isEmpty())
        return;
    writeFile(p, QJsonDocument(t).toJson());
    customParts.append(t);
    uiContext.setPreference(Preference::customParts, customParts);
    refresh();
}

void PartPalette::loadPart() {
    QString p = QFileDialog::getOpenFileName(this, tr("マイ部品を読み込む"), {},
                                             tr("部品 (*.molforma-part.json *.json)"));
    if (!p.isEmpty())
        loadPartFile(p);
}

void PartPalette::loadPartFile(const QString &p) {
    auto t = parseJson(readFile(p));
    validateTemplate(t);
    if (!t["id"].toString().startsWith("my:"))
        fail(tr("マイ部品のIDはmy:で始まる必要があります。"));
    for (int i = customParts.size() - 1; i >= 0; --i)
        if (customParts[i].toObject()["id"] == t["id"])
            customParts.removeAt(i);
    customParts.append(t);
    uiContext.setPreference(Preference::customParts, customParts);
    setCategory("my");
    emit showRequested();
    refresh();
}

void PartPalette::removePart() {
    auto t = currentTemplate();
    if (!t["id"].toString().startsWith("my:"))
        fail(tr("マイ部品をパレットで選択してください。"));
    for (int i = customParts.size() - 1; i >= 0; --i)
        if (customParts[i].toObject()["id"] == t["id"])
            customParts.removeAt(i);
    uiContext.setPreference(Preference::customParts, customParts);
    refresh();
}
} // namespace mf
