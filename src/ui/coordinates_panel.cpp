// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/coordinates_panel.h"
#include "editing/chemistry.h"
#include "editing/peptide.h"
#include "io/structure_io.h"
#include "model/geometry.h"
#include "model/structure_file.h"
#include "nwchem/nwchem_geometry.h"
#include "ui/editor_chrome.h"
#include "ui/main_window.h"
#include "ui/periodic_table.h"
#include "ui/structure_import.h"
#include "ui/theme.h"
#include <QtWidgets>

namespace mf {
namespace {
// Columns by their number in the table model. The residue column is shown second and the change
// column first (moved header sections), so cells keep their numbers for MCP clients.
enum Column {
    AtomColumn,
    XColumn,
    YColumn,
    ZColumn,
    MassColumn,
    GhostColumn,
    FixedColumn,
    PlaceColumn,
    ChangeColumn,
    // The atom attributes, shown with "属性の列".
    ChargeColumn,
    RadicalColumn,
    NameColumn,
    ColumnCount
};
// The cell of the document that a column edits.
CoordinateColumn documentColumn(int column) {
    return column == ChargeColumn    ? CoordinateColumn::Charge
           : column == RadicalColumn ? CoordinateColumn::Radical
           : column == NameColumn    ? CoordinateColumn::Name
                                     : CoordinateColumn(column);
}
bool textColumn(int column) {
    return column <= MassColumn || column >= ChargeColumn;
}
constexpr int IdRole = Qt::UserRole, FixedRole = Qt::UserRole + 1;
// The structure files that "構造を読み込む…" and a drop on the tab read as a replacement candidate.
const QStringList structureSuffixes{"xyz", "mol", "sdf", "pdb", "ent", "cif", "mmcif"};
// Takes the width left over in its row of a FlowLayout; hidden, it takes none.
QWidget *gap() {
    auto *w = new QWidget;
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return w;
}
// Three faint atoms joined by bonds, above the text of the empty table.
QPixmap emptyPicture(const ChromeColors &c) {
    QPixmap pixmap(QSize(112, 112));
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(c.line, 3, Qt::SolidLine, Qt::RoundCap));
    for (const QPointF end : {QPointF(28, 12), QPointF(12, 43), QPointF(44, 43)})
        p.drawLine(QPointF(28, 30), end);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(210, 63, 49, 140));
    p.drawEllipse(QPointF(28, 11), 7, 7);
    p.setBrush(QColor(69, 76, 84, 140));
    p.drawEllipse(QPointF(28, 30), 8, 8);
    p.setPen(QPen(c.muted, 1.5));
    p.setBrush(c.base);
    p.drawEllipse(QPointF(12, 43), 5, 5);
    p.drawEllipse(QPointF(44, 43), 5, 5);
    return pixmap;
}
} // namespace

QString CoordinateDelegate::displayText(const QVariant &value, const QLocale &locale) const {
    if (kind == CoordinateCell) {
        bool ok;
        const double number = value.toString().toDouble(&ok);
        if (ok) {
            // A value that rounds to zero reads 0.000000, not -0.000000.
            const auto text = QString::number(number, 'f', 6);
            return text == "-0.000000" ? text.mid(1) : text;
        }
    }
    return QStyledItemDelegate::displayText(value, locale);
}
void CoordinateDelegate::initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const {
    QStyledItemDelegate::initStyleOption(option, index);
    if (kind == CoordinateCell)
        option->displayAlignment = Qt::AlignRight | Qt::AlignVCenter;
    else if (kind == AtomCell) {
        option->text += QString::number(index.row() + 1);
        option->font.setBold(true);
    } else {
        option->displayAlignment = Qt::AlignCenter;
        if (option->text.isEmpty()) {
            option->text = tr("既定");
            for (const auto role : {QPalette::Text, QPalette::HighlightedText})
                option->palette.setColor(role, ChromeColors(option->palette).muted);
        }
    }
}
void CoordinateDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                               const QModelIndex &index) const {
    QStyledItemDelegate::paint(painter, option, index);
    if (kind != AtomCell || !index.data(FixedRole).toBool())
        return;
    const int side = 12;
    lineIcon("lock", ChromeColors(option.palette).muted)
        .paint(painter,
               QRect(option.rect.right() - side - 6, option.rect.center().y() - side / 2, side, side));
}

CoordinateModel::CoordinateModel(QObject *parent, Edit edit, QStringList titles, QStringList tips)
    : QAbstractTableModel(parent), edit(std::move(edit)), titles(std::move(titles)), tips(std::move(tips)) {}
void CoordinateModel::show(View next) {
    if (next.document.atoms.size() != shown.document.atoms.size()) {
        beginResetModel();
        shown = std::move(next);
        endResetModel();
    } else {
        shown = std::move(next);
        if (rowCount() > 0)
            emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
    }
    emit headerDataChanged(Qt::Horizontal, XColumn, ZColumn);
}
void CoordinateModel::clearIcons() {
    icons.clear();
}
QString CoordinateModel::id(int row) const {
    return row >= 0 && row < shown.document.atoms.size() ? shown.document.atoms[row].id : QString();
}
int CoordinateModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(shown.document.atoms.size());
}
int CoordinateModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}
QVariant CoordinateModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || section < 0 || section >= ColumnCount)
        return QAbstractTableModel::headerData(section, orientation, role);
    if (role == Qt::DisplayRole)
        return section >= XColumn && section <= ZColumn
                   ? QString("%1 / %2")
                         .arg(QString("XYZ")[section - XColumn])
                         .arg(shown.unit == CoordinateUnit::Bohr ? "Bohr" : "Å")
                   : titles[section];
    if (role == Qt::ToolTipRole)
        return tips[section];
    return {};
}
Qt::ItemFlags CoordinateModel::flags(const QModelIndex &index) const {
    if (!index.isValid())
        return Qt::NoItemFlags;
    const auto &atom = shown.document.atoms[index.row()];
    const int col = index.column();
    auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (shown.editable && textColumn(col) && !(atom.fixed && col >= XColumn && col <= ZColumn))
        flags |= Qt::ItemIsEditable;
    if (shown.editable && (col == GhostColumn || col == FixedColumn))
        flags |= Qt::ItemIsUserCheckable;
    return flags;
}
QVariant CoordinateModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid())
        return {};
    const auto &atom = shown.document.atoms[index.row()];
    const int col = index.column();
    const bool added = shown.changes.added.contains(atom.id), moved = shown.changes.moved.contains(atom.id);
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        if (textColumn(col))
            return coordinateText(atom, documentColumn(col), shown.unit);
        if (col == PlaceColumn)
            return shown.places.value(atom.id);
        if (col == ChangeColumn)
            return added ? tr("追加", "atom change") : moved ? tr("移動", "atom change") : QString();
        return {};
    case Qt::CheckStateRole:
        if (col == GhostColumn || col == FixedColumn)
            return (col == GhostColumn ? atom.ghost : atom.fixed) ? Qt::Checked : Qt::Unchecked;
        return {};
    case Qt::DecorationRole:
        if (col == AtomColumn)
            return atomDot(atom);
        if (col == ChangeColumn && (added || moved))
            return mark(added ? "added" : "moved");
        return {};
    case Qt::ForegroundRole:
        if (col == PlaceColumn || (atom.fixed && col >= XColumn && col <= ZColumn))
            return QBrush(shown.muted);
        if (col == ChangeColumn)
            return QBrush(added ? shown.good : shown.changed);
        return {};
    case Qt::ToolTipRole:
        return col >= ChargeColumn  ? tips[col]
               : col == MassColumn  ? tr("例: 13 = ¹³C、13.003355 = 質量（amu）、空欄 = 既定値")
               : col == GhostColumn ? tr("NWChemではbq元素として出力します。基底の割当が別途必要です。")
               : col == FixedColumn ? tr("座標編集・削除を禁止し、NWChemのconstraints fix atomへ出力します。")
               : col >= XColumn && col <= ZColumn && atom.fixed ? tr("固定原子は座標を編集できません")
                                                                : QString();
    case IdRole:
        return atom.id;
    case FixedRole:
        return col == AtomColumn ? QVariant(atom.fixed) : QVariant();
    default:
        return {};
    }
}
bool CoordinateModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (!index.isValid() ||
        !(flags(index) & (role == Qt::CheckStateRole ? Qt::ItemIsUserCheckable : Qt::ItemIsEditable)))
        return false;
    const int col = index.column();
    if (role == Qt::CheckStateRole)
        edit(id(index.row()), documentColumn(col), value.toInt() == Qt::Checked ? "1" : "0");
    else if (role == Qt::EditRole && textColumn(col))
        edit(id(index.row()), documentColumn(col), value.toString());
    else
        return false;
    return true;
}
QIcon CoordinateModel::atomDot(const Atom &atom) const {
    const auto key = (atom.ghost ? "@" : "") + atom.element;
    if (const auto it = icons.constFind(key); it != icons.constEnd())
        return *it;
    QPixmap pixmap(QSize(28, 28));
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    const auto color = element(atom.element).color;
    // A ghost has no nucleus: only the outline.
    p.setPen(QPen(color.darker(160), atom.ghost ? 1.6 : 1));
    p.setBrush(atom.ghost ? QBrush(Qt::NoBrush) : QBrush(color));
    p.drawEllipse(QRectF(2.5, 2.5, 9, 9));
    p.end();
    return *icons.insert(key, QIcon(pixmap));
}
QIcon CoordinateModel::mark(const QString &kind) const {
    auto &icon = icons["#" + kind];
    if (icon.isNull())
        icon = changeIcon(kind);
    return icon;
}

CoordinatesPanel::CoordinatesPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("coordinatesTab");
    setAcceptDrops(true);
    // Commands, the selection (or the candidate), the table and a footer, under the 3D view of
    // "1 分子" or in its place.
    auto *ll = new QVBoxLayout(this);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(0);
    auto *toolbar = new QFrame;
    toolbar->setObjectName("coordinateToolbar");
    toolbar->setProperty("toolbar", true);
    auto *tools = new FlowLayout(toolbar, 8, 6);
    tools->setContentsMargins(12, 8, 12, 8);
    ll->addWidget(toolbar);
    // Replacing the structure (a candidate to confirm), then adding one atom.
    auto *structure = new QPushButton(tr("構造"));
    structure->setObjectName("coordinateStructure");
    structure->setIcon(lineIcon("import"));
    structure->setToolTip(tr("構造ファイル・XYZで今の構造を置き換える候補を作るか、原子を並べ替えるか、"
                             "全原子を削除します。"));
    auto *structureMenu = new QMenu(structure);
    structureMenu->setObjectName("coordinateStructureMenu");
    structureMenu->setToolTipsVisible(true);
    const auto structureAction = [&](const QString &text, const QString &name, const QString &icon,
                                     const QString &tip, const std::function<void()> &action) {
        auto *a = structureMenu->addAction(lineIcon(icon), text);
        a->setObjectName(name);
        a->setToolTip(tip);
        connect(a, &QAction::triggered, this, [this, action] { attempt(action); });
        editActions << a;
    };
    structureAction(tr("構造ファイルを読み込む…"), "coordinateImport", "import",
                    tr("XYZ・MOL・SDF・PDB・mmCIFを読み込み、今の構造を置き換える候補を作ります。"),
                    [this] { importFile(); });
    structureAction(tr("XYZを貼り付けて置き換え…"), "coordinatePaste", "paste",
                    tr("クリップボードのXYZ（Å）で今の構造を置き換える候補を作ります。"),
                    [this] { pasteXyz(); });
    // The order of the atoms, as the table and the NWChem geometry list them.
    auto *sortMenu = structureMenu->addMenu(lineIcon("sort"), tr("原子を並べ替え"));
    sortMenu->setObjectName("coordinateSortMenu");
    for (const auto &[text, mode] : {std::pair{tr("元素順"), "element"},
                                     {tr("残基順"), "residue"},
                                     {tr("選択した原子を先頭へ"), "selected"}}) {
        auto *a = sortMenu->addAction(text);
        a->setObjectName(QString("coordinateSort_") + mode);
        connect(a, &QAction::triggered, this,
                [this, mode = QString(mode)] { attempt([this, mode] { window->sortAtoms(mode); }); });
        editActions << a;
    }
    connect(sortMenu, &QMenu::aboutToShow, this, [this] {
        findChild<QAction *>("coordinateSort_selected")
            ->setEnabled(editable() && !window->selection().isEmpty());
    });
    structureMenu->addSeparator();
    structureAction(
        tr("全原子を削除"), "coordinateClear", "delete", tr("すべての原子を削除する候補を作ります。"),
        [this] { window->edit(tr("全原子を削除"), [](Document &d) { d.remove(d.ids()); }, true); });
    structure->setMenu(structureMenu);
    tools->addWidget(structure);
    editControls << structure;
    button(tools, tr("原子を追加…"), "coordinateAdd", "plus", [this] { addAtom(); }, true);
    tools->addWidget(gap());
    filter = new QLineEdit;
    filter->setObjectName("coordinateFilter");
    filter->setPlaceholderText(tr("原子・元素・残基で絞り込み"));
    filter->setToolTip(tr("番号つきの原子名（C14）、元素、残基（GLY 2）で表の行を絞り込みます。"));
    filter->setClearButtonEnabled(true);
    filter->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    filter->setFixedWidth(160);
    tools->addWidget(filter);
    connect(filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    attributes = new QPushButton(tr("属性の列"));
    attributes->setObjectName("coordinateAttributeColumns");
    attributes->setCheckable(true);
    attributes->setToolTip(tr("形式電荷・不対電子・原子名の列を表示して編集します。"));
    attributes->setChecked(window->preference("coordinates/attributes", false).toBool());
    tools->addWidget(attributes);
    connect(attributes, &QPushButton::toggled, this, [this](bool on) {
        window->setPreference("coordinates/attributes", on);
        for (const int column : {ChargeColumn, RadicalColumn, NameColumn})
            table->setColumnHidden(column, !on);
    });
    auto *unitBox = new QWidget;
    auto *unitRowLayout = new QHBoxLayout(unitBox);
    unitRowLayout->setContentsMargins(0, 0, 0, 0);
    unitRowLayout->addWidget(new QLabel(tr("単位")));
    units = new QComboBox;
    units->setToolTip(tr("表に表示する座標の単位です（NWChem入力の単位は「2 計算条件」で設定します）。"));
    units->setObjectName("coordinateUnits");
    units->addItems({"Å", "Bohr"});
    unitRowLayout->addWidget(units);
    tools->addWidget(unitBox);
    connect(units, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    // The geometry as an NWChem fragment (in the unit of the input) or as XYZ (always Å).
    exportButton = new QPushButton(tr("書き出し"));
    exportButton->setObjectName("coordinateExport");
    exportButton->setIcon(lineIcon("export"));
    exportButton->setToolTip(tr("座標と固定原子のNWChem入力断片か、XYZ（常にÅ）をコピー・保存します。"));
    auto *exportMenu = new QMenu(exportButton);
    exportMenu->setObjectName("coordinateExportMenu");
    exportMenu->setToolTipsVisible(true);
    const auto exportAction = [&](const QString &text, const QString &name, bool xyz, bool save) {
        auto *a = exportMenu->addAction(lineIcon(save ? "export" : "copy"), text);
        a->setObjectName(name);
        connect(a, &QAction::triggered, this, [this, xyz, save] { attempt([&] { exportText(xyz, save); }); });
        exportActions << a;
        return a;
    };
    exportAction(tr("NWChem座標ブロックをコピー"), "copyNwchemGeometry", false, false);
    exportAction(tr("NWChem座標ブロックを書き出す…"), "saveNwchemGeometry", false, true);
    exportMenu->addSeparator();
    exportAction(tr("XYZ（Å）をコピー"), "coordinateCopyXyz", true, false)
        ->setToolTip(tr("ゴーストは@元素、質量は第5列で書き出します（MolFormaの拡張）。"));
    exportAction(tr("XYZ（Å）を書き出す…"), "coordinateSaveXyz", true, true);
    for (auto *a : exportActions.mid(0, 2))
        a->setToolTip(tr("geometryブロックと固定原子のconstraintsです。電荷・基底・手法・taskと点電荷は"
                         "含みません。入力全体は「入力プレビュー」で確かめます。"));
    exportButton->setMenu(exportMenu);
    connect(exportMenu, &QMenu::aboutToShow, this, &CoordinatesPanel::refreshExport);
    tools->addWidget(exportButton);
    message = new QLabel;
    message->setObjectName("coordinateMessage");
    message->setWordWrap(true);
    message->setContentsMargins(12, 6, 12, 6);
    auto messagePalette = message->palette();
    messagePalette.setColor(QPalette::WindowText, errorColor(messagePalette));
    message->setPalette(messagePalette);
    message->hide();
    ll->addWidget(message);

    // The selection, and its measurement while the 3D view is hidden; a candidate replaces it. What
    // can be done to the selection is in the pages at the right of "1 分子", next to the table.
    selectionBar = new QFrame;
    selectionBar->setObjectName("coordinateSelectionBar");
    auto *bar = new FlowLayout(selectionBar, 8, 6);
    bar->setContentsMargins(12, 6, 12, 6);
    bar->setUniformRows(true);
    ll->addWidget(selectionBar);
    selectionHint =
        new ElidedLabel(tr("原子名か3D画面の原子をクリックして選択します（Shift／Ctrl＋クリックで追加）。"));
    selectionHint->setObjectName("coordinateSelectionHint");
    selectionHint->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed); // Elided only when narrow.
    bar->addWidget(selectionHint);
    selectionTitle = new QLabel;
    selectionTitle->setObjectName("coordinateSelectionTitle");
    selectionItems << selectionTitle;
    for (int i = 0; i < 3; ++i) {
        auto *chip = new QLabel;
        chip->setObjectName(QString("coordinateSelectionAtom%1").arg(i));
        chip->setProperty("chip", true);
        chip->setTextFormat(Qt::RichText);
        chips << chip;
        selectionItems << chip;
    }
    moreAtoms = new QLabel;
    measurement = new QLabel;
    measurement->setObjectName("coordinateMeasurement");
    selectionItems << moreAtoms << measurement << gap();
    for (auto *item : std::as_const(selectionItems))
        bar->addWidget(item);
    auto *deselect = new QToolButton;
    deselect->setObjectName("coordinateDeselect");
    deselect->setIcon(lineIcon("close"));
    deselect->setToolTip(tr("選択を解除"));
    bar->addWidget(deselect);
    connect(deselect, &QToolButton::clicked, this, [this] { window->setSelection({}); });
    selectionItems << deselect;
    auto *candidateIcon = new QLabel;
    candidateIcon->setObjectName("coordinateCandidateIcon");
    auto *candidateTitle = new QLabel(tr("候補の座標を表示中"));
    candidateTitle->setObjectName("coordinateCandidateTitle");
    auto *candidateHint = new QLabel(tr("確定するまで表は編集できません"));
    auto *changeFilter = new QFrame;
    changeFilter->setObjectName("coordinateChangeFilter");
    changeFilter->setProperty("choice", "segments");
    auto *changeRow = new QHBoxLayout(changeFilter);
    changeRow->setContentsMargins(2, 2, 2, 2);
    changeRow->setSpacing(2);
    auto *changeGroup = new QButtonGroup(this);
    for (auto **b : {&showAll, &showChanged}) {
        *b = new QToolButton;
        (*b)->setCheckable(true);
        changeRow->addWidget(*b);
        changeGroup->addButton(*b);
    }
    showAll->setObjectName("coordinateShowAll");
    showAll->setChecked(true);
    showChanged->setObjectName("coordinateShowChanged");
    showChanged->setToolTip(tr("今の構造から追加・移動する原子の行だけを表示します。"));
    connect(changeGroup, &QButtonGroup::buttonClicked, this, [this] { applyFilter(); });
    compareNote = new QLabel;
    compareNote->setObjectName("coordinateCompareNote");
    candidateItems << candidateIcon << candidateTitle << candidateHint << gap() << changeFilter
                   << compareNote;
    for (auto *item : std::as_const(candidateItems))
        bar->addWidget(item);

    // The table, or what to do while there are no atoms.
    tablePages = new QStackedWidget;
    ll->addWidget(tablePages, 1);
    table = new QTableView;
    table->setObjectName("coordinateTable");
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setSortingEnabled(false);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->setFrameShape(QFrame::NoFrame);
    table->setMinimumWidth(360);
    table->verticalHeader()->setDefaultSectionSize(30);
    const QStringList titles{tr("原子"), "X / Å",        "Y / Å",        "Z / Å",
                             tr("質量"), tr("ゴースト"), tr("固定"),     tr("残基 · 名前"),
                             tr("変化"), tr("形式電荷"), tr("不対電子"), tr("原子名")};
    const QStringList tips{
        tr("元素記号を編集します（D・T、@Cでゴースト）。表示は3D画面と同じ番号つきの原子名です。"),
        {},
        {},
        {},
        tr("整数＝質量数、小数＝amu、空欄＝既定値"),
        tr("NWChemではbq元素として出力します。基底の割当が別途必要です。"),
        tr("座標編集・削除を禁止し、NWChemのconstraints fix atomへ出力します。"),
        tr("残基と、その中での原子名"),
        tr("候補で今の構造から追加・移動する原子"),
        tr("水素補完などの構造準備に使う形式電荷です。計算の全電荷は「2 計算条件」で設定します。"),
        tr("原子のラジカル属性です。計算のスピン多重度は「2 計算条件」で設定します。"),
        tr("PDB・残基で使う原子名です（例: CA）。")};
    model = new CoordinateModel(
        this,
        [this](const QString &id, CoordinateColumn column, const QString &text) {
            const auto unit = tableUnit();
            attempt([&] {
                window->edit(tr("座標表を編集"),
                             [=](Document &d) { setCoordinateCell(d, id, column, text, unit); });
            });
            refresh();
        },
        titles, tips);
    table->setModel(model);
    // Four rows at least: the 3D view above it gives way first.
    table->setMinimumHeight(table->horizontalHeader()->sizeHint().height() + 4 * 30 +
                            table->horizontalScrollBar()->sizeHint().height());
    auto *header = table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(PlaceColumn, QHeaderView::Stretch);
    header->moveSection(header->visualIndex(PlaceColumn), 1);
    header->moveSection(header->visualIndex(ChangeColumn), 0);
    const QHash<int, int> widths{{AtomColumn, 92},    {XColumn, 132},     {YColumn, 132},
                                 {ZColumn, 132},      {MassColumn, 92},   {GhostColumn, 72},
                                 {FixedColumn, 56},   {ChangeColumn, 76}, {ChargeColumn, 76},
                                 {RadicalColumn, 76}, {NameColumn, 76}};
    for (auto it = widths.begin(); it != widths.end(); ++it)
        table->setColumnWidth(it.key(), it.value());
    table->setItemDelegateForColumn(AtomColumn, new CoordinateDelegate(table, CoordinateDelegate::AtomCell));
    for (int column = XColumn; column <= ZColumn; ++column)
        table->setItemDelegateForColumn(column, new CoordinateDelegate(table));
    table->setItemDelegateForColumn(MassColumn, new CoordinateDelegate(table, CoordinateDelegate::MassCell));
    for (const int column : {ChargeColumn, RadicalColumn, NameColumn})
        table->setColumnHidden(column, !attributes->isChecked());
    tablePages->addWidget(table);
    connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        if (busy)
            return;
        QList<bool> marked(model->rowCount(), false);
        for (const auto &range : table->selectionModel()->selection())
            for (int row = range.top(); row <= range.bottom(); ++row)
                marked[row] = true;
        Ids ids;
        for (int row = 0; row < model->rowCount(); ++row)
            if (marked[row])
                ids << model->id(row);
        // Retain click order from the molecule view when adding table rows.
        const QSet<QString> rows(ids.begin(), ids.end());
        Ids ordered;
        QSet<QString> kept;
        for (const auto &id : window->selection())
            if (rows.contains(id) && !kept.contains(id)) {
                ordered << id;
                kept.insert(id);
            }
        for (const auto &id : ids)
            if (!kept.contains(id))
                ordered << id;
        window->setSelection(ordered);
    });
    auto *empty = new QWidget;
    empty->setObjectName("coordinateEmpty");
    // Not counted in the panel's minimum height (that of the table): the view above keeps its room.
    empty->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    auto *emptyLayout = new QGridLayout(empty);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    auto *card = new QFrame;
    card->setObjectName("coordinateEmptyCard");
    card->setMaximumWidth(560);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(32, 28, 32, 28);
    cardLayout->setSpacing(12);
    auto *picture = new QLabel;
    picture->setObjectName("coordinateEmptyPicture");
    picture->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(picture);
    auto *emptyTitle = new QLabel(tr("原子がありません"));
    emptyTitle->setObjectName("coordinateEmptyTitle");
    emptyTitle->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(emptyTitle);
    auto *emptyText = new QLabel(tr("構造ファイルを読み込むか、XYZを貼り付けて始めます。"
                                    "読み込んだ構造は候補として表示し、確定すると反映します。"));
    emptyText->setWordWrap(true);
    emptyText->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(emptyText);
    auto *emptyActions = new QHBoxLayout;
    emptyActions->addStretch();
    button(
        emptyActions, tr("構造を読み込む…"), "coordinateEmptyImport", "import", [this] { importFile(); },
        true)
        ->setProperty("primary", true);
    button(emptyActions, tr("XYZを貼り付け…"), "coordinateEmptyPaste", "paste", [this] { pasteXyz(); }, true);
    button(emptyActions, tr("原子を追加…"), "coordinateEmptyAdd", "plus", [this] { addAtom(); }, true);
    emptyActions->addStretch();
    cardLayout->addLayout(emptyActions);
    auto *emptyNote = new QLabel(
        tr("XYZ・MOL・SDF・PDB（.ent）・mmCIF（.cif）のファイルは、ここへドロップしても読み込めます。"
           "部品やアミノ酸配列から組み立てるときは左のパレットを使います。"));
    emptyNote->setObjectName("coordinateEmptyNote");
    emptyNote->setWordWrap(true);
    emptyNote->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(emptyNote);
    emptyLayout->addWidget(card, 0, 0, Qt::AlignCenter);
    tablePages->addWidget(empty);

    auto *footer = new QFrame;
    footer->setObjectName("coordinateFooter");
    auto *footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(12, 6, 12, 6);
    footerRow->setSpacing(16);
    footerRow->addWidget(
        new ElidedLabel(tr("ダブルクリックかF2で編集、Enterで確定。質量は整数＝質量数、小数＝amu、"
                           "空欄＝既定値。XYZの入出力は常にÅです。")),
        1);
    count = new QLabel;
    count->setObjectName("coordinateCount");
    footerRow->addWidget(count);
    ll->addWidget(footer);
}
QPushButton *CoordinatesPanel::button(QLayout *layout, const QString &text, const QString &name,
                                      const QString &icon, const std::function<void()> &action,
                                      bool editing) {
    auto *b = new QPushButton(text);
    b->setObjectName(name);
    if (!icon.isEmpty())
        b->setIcon(lineIcon(icon));
    layout->addWidget(b);
    if (editing)
        editControls << b;
    connect(b, &QPushButton::clicked, this, [this, action] { attempt(action); });
    return b;
}
void CoordinatesPanel::showAttributes() {
    window->showCoordinateTable();
    attributes->setChecked(true);
    table->scrollTo(model->index(std::max(0, table->currentIndex().row()), ChargeColumn));
}
bool CoordinatesPanel::editable() const {
    return !window->hasCandidate() && !window->chemistryRunning();
}
CoordinateUnit CoordinatesPanel::tableUnit() const {
    return units->currentIndex() == 1 ? CoordinateUnit::Bohr : CoordinateUnit::Angstrom;
}
CoordinateUnit CoordinatesPanel::exportUnit() const {
    return window->displayDocument().data["nwchem"].toObject()["geometry_units"] == "bohr"
               ? CoordinateUnit::Bohr
               : CoordinateUnit::Angstrom;
}
void CoordinatesPanel::attempt(const std::function<void()> &action) {
    message->clear();
    message->hide();
    try {
        action();
    } catch (const std::exception &e) {
        // Restore rejected cells immediately; there is no hidden draft to export.
        refresh();
        message->setText(tr("%1\n操作は反映されていません。").arg(QString::fromUtf8(e.what())));
        message->show();
    }
}
void CoordinatesPanel::refresh() {
    if (busy)
        return;
    QScopedValueRollback guard(busy, true);
    const auto &d = window->displayDocument();
    const bool candidate = window->hasCandidate();
    const auto changes = candidate ? atomChanges(window->document(), d) : AtomChanges{};
    const ChromeColors colors(palette());
    model->show({d, changes, atomPlaces(d), tableUnit(), editable(), colors.muted, colors.good,
                 colors.candidateText});
    table->setColumnHidden(ChangeColumn, !candidate);
    if (!candidate)
        showAll->setChecked(true);
    showAll->setText(tr("すべて %1").arg(d.atoms.size()));
    showChanged->setText(tr("変化あり %1").arg(changes.added.size() + changes.moved.size()));
    compareNote->setText(
        changes.byOrder ? tr("原子の順番で今の構造と比べています（0.001 Å未満の差は同じ位置）") : QString());
    tablePages->setCurrentIndex(d.atoms.isEmpty() && !candidate ? 1 : 0);
    selectionBar->setVisible(tablePages->currentIndex() == 0);
    filter->setEnabled(!d.atoms.isEmpty());
    for (auto *control : std::as_const(editControls))
        control->setEnabled(editable());
    for (auto *action : std::as_const(editActions))
        action->setEnabled(editable());
    refreshExport();
    applyFilter();
    refreshSelection();
}
void CoordinatesPanel::refreshExport() {
    const auto &d = window->displayDocument();
    bool valid = !d.atoms.isEmpty();
    try {
        nwchemGeometry(d, exportUnit());
    } catch (const std::exception &) {
        valid = false;
    }
    const auto unit = exportUnit() == CoordinateUnit::Bohr ? "Bohr" : "Å";
    exportActions[0]->setText(tr("NWChem座標ブロックをコピー（%1）").arg(unit));
    exportActions[1]->setText(tr("NWChem座標ブロックを書き出す…（%1）").arg(unit));
    for (int i = 0; i < exportActions.size(); ++i)
        exportActions[i]->setEnabled((i < 2 ? valid : !d.atoms.isEmpty()) && editable());
    exportButton->setEnabled(!d.atoms.isEmpty());
}
void CoordinatesPanel::applyFilter() {
    const auto needle = filter->text().trimmed();
    const bool changedOnly = window->hasCandidate() && showChanged->isChecked();
    const auto text = [this](int row, int column) { return model->index(row, column).data().toString(); };
    for (int row = 0; row < model->rowCount(); ++row) {
        bool shown = !changedOnly || !text(row, ChangeColumn).isEmpty();
        if (shown && !needle.isEmpty())
            shown =
                (text(row, AtomColumn) + QString::number(row + 1)).contains(needle, Qt::CaseInsensitive) ||
                text(row, PlaceColumn).contains(needle, Qt::CaseInsensitive);
        if (table->isRowHidden(row) == shown)
            table->setRowHidden(row, !shown);
    }
    refreshCount();
}
void CoordinatesPanel::refreshCount() {
    const auto &d = window->displayDocument();
    if (window->hasCandidate()) {
        count->setText(
            tr("候補 %n 原子（今は %1）", nullptr, int(d.atoms.size())).arg(window->document().atoms.size()));
        return;
    }
    int hidden = 0;
    for (int row = 0; row < model->rowCount(); ++row)
        hidden += table->isRowHidden(row);
    const auto fixed = std::count_if(d.atoms.begin(), d.atoms.end(), [](const Atom &a) { return a.fixed; });
    const int n = d.atoms.size();
    count->setText(
        hidden ? tr("%n 原子・選択 %1・固定 %2・表示 %3", nullptr, n)
                     .arg(window->selection().size())
                     .arg(fixed)
                     .arg(n - hidden)
               : tr("%n 原子・選択 %1・固定 %2", nullptr, n).arg(window->selection().size()).arg(fixed));
}
void CoordinatesPanel::refreshSelection() {
    QScopedValueRollback guard(busy, true);
    // One selection of row ranges: selecting item by item is quadratic in the selected rows.
    const QSet<QString> chosen(window->selection().begin(), window->selection().end());
    const int last = ColumnCount - 1;
    QItemSelection rows;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (!chosen.contains(model->id(row)))
            continue;
        if (!rows.isEmpty() && rows.last().bottom() == row - 1)
            rows.last() = QItemSelectionRange(rows.last().topLeft(), model->index(row, last));
        else
            rows.select(model->index(row, 0), model->index(row, last));
    }
    table->selectionModel()->select(rows, QItemSelectionModel::ClearAndSelect);
    // Atoms chosen in the 3D view: bring their rows into the table when none of them shows.
    const auto viewport = table->viewport()->rect();
    int first = -1;
    bool visible = false;
    for (const auto &range : std::as_const(rows))
        for (int row = range.top(); !visible && row <= range.bottom(); ++row)
            if (!table->isRowHidden(row)) {
                if (first < 0)
                    first = row;
                visible = table->visualRect(model->index(row, 0)).intersects(viewport);
            }
    if (!visible && first >= 0)
        table->scrollTo(model->index(first, 0), QAbstractItemView::PositionAtTop);
    refreshSelectionBar();
}
void CoordinatesPanel::refreshSelectionBar() {
    const auto &d = window->displayDocument();
    const bool candidate = window->hasCandidate();
    if (selectionBar->property("candidate").toBool() != candidate) {
        selectionBar->setProperty("candidate", candidate);
        selectionBar->style()->unpolish(selectionBar);
        selectionBar->style()->polish(selectionBar);
    }
    QHash<QString, int> index;
    for (int i = 0; !window->selection().isEmpty() && i < d.atoms.size(); ++i)
        index.insert(d.atoms[i].id, i);
    // Atoms that a candidate removes are left out until it is confirmed or cancelled.
    Ids shown;
    for (const auto &id : window->selection())
        if (index.contains(id))
            shown << id;
    const bool selecting = !candidate && !shown.isEmpty();
    selectionHint->setVisible(!candidate && shown.isEmpty());
    for (auto *w : std::as_const(selectionItems))
        w->setVisible(selecting);
    for (auto *w : std::as_const(candidateItems))
        w->setVisible(candidate);
    compareNote->setVisible(candidate && !compareNote->text().isEmpty());
    if (selecting) {
        const int n = shown.size();
        selectionTitle->setText(tr("%n 原子を選択", nullptr, n));
        const ChromeColors colors(palette());
        for (int i = 0; i < chips.size(); ++i) {
            chips[i]->setVisible(i < n);
            if (i >= n)
                continue;
            const int row = index[shown[i]];
            const auto &atom = d.atoms[row];
            auto color = element(atom.element).color;
            if (color.lightness() > 200) // Hydrogen's white would not show on the chip.
                color = colors.muted;
            const auto place = n <= 2 ? atomPlace(d, atom.id) : QString();
            chips[i]->setText(QString("<span style=\"color:%1\">●</span> <b>%2%3</b>%4")
                                  .arg(color.name(), atom.element)
                                  .arg(row + 1)
                                  .arg(place.isEmpty()
                                           ? QString()
                                           : QString(" <span style=\"color:%1\">%2</span>")
                                                 .arg(colors.muted.name(), place.toHtmlEscaped())));
        }
        moreAtoms->setVisible(n > chips.size());
        moreAtoms->setText(tr("ほか %n 原子", nullptr, n - int(chips.size())));
        QString measured;
        if (n >= 2 && n <= 4 && n == window->selection().size()) {
            try {
                const double m = measure(d, shown);
                measured = QString("%1 %2 %3")
                               .arg(QStringList{tr("距離"), tr("角度"), tr("二面角")}[n - 2])
                               .arg(m, 0, 'f', 4)
                               .arg(n == 2 ? "Å" : "°");
            } catch (const std::exception &e) {
                measured = QString::fromUtf8(e.what());
            }
        }
        measurement->setText(measured);
        // The measurement panel of the 3D view shows it, unless the table is shown in its place.
        measurement->setVisible(!measured.isEmpty() && window->moleculeLayoutMode() == "table");
    }
    refreshCount();
}
void CoordinatesPanel::addAtom() {
    if (!editable())
        fail(tr("候補を確定・取消し、実行中の整形を終了してください。"));
    const auto base = window->revision();
    const auto unit = tableUnit();
    QDialog dialog(this);
    dialog.setObjectName("coordinateAtomDialog");
    dialog.setWindowTitle(tr("座標を指定して原子を追加"));
    dialog.setMinimumWidth(480);
    dialog.setStyleSheet(chromeRules(R"(
        QToolButton { border: 1px solid @line; border-radius: 5px; background: @base; color: @text; }
        QToolButton:hover { background: @hover; }
        QToolButton:checked { background: @accent; border-color: @accent; color: @onAccent; font-weight: bold; }
        QLabel[note="true"] { color: @muted; }
    )",
                                     static_cast<QWidget *>(window)->palette()));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setSpacing(8);
    // A muted explanation, indented under the check box it explains.
    const auto note = [](const QString &text, bool indented = false) {
        auto *label = new QLabel(text);
        label->setWordWrap(true);
        label->setProperty("note", true);
        if (indented)
            label->setContentsMargins(24, 0, 0, 0);
        return label;
    };
    const auto heading = [&](const QString &text) {
        auto *label = new QLabel(text);
        auto font = label->font();
        font.setBold(true);
        label->setFont(font);
        layout->addSpacing(4);
        layout->addWidget(label);
    };
    auto *hint = new QLabel(tr("1原子を末尾（%1 番）に追加します。結合や水素の補完は「1 分子」で行えます。")
                                .arg(window->document().atoms.size() + 1));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    heading(tr("元素"));
    auto *symbol = new QComboBox;
    symbol->setObjectName("coordinateAtomElement");
    for (const auto &e : elements())
        symbol->addItem(e.symbol);
    symbol->addItems({"D", "T"});
    symbol->setCurrentIndex(-1);
    auto *common = new QGridLayout;
    common->setSpacing(6);
    QList<QToolButton *> quick;
    const QStringList symbols{"H", "C", "N", "O", "F", "P", "S", "Cl", "Br", "I", "D", "T"};
    for (int i = 0; i < symbols.size(); ++i) {
        auto *b = new QToolButton;
        b->setObjectName("coordinateAtomQuick" + symbols[i]);
        b->setText(symbols[i]);
        b->setCheckable(true);
        b->setMinimumHeight(30);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        common->addWidget(b, i / 6, i % 6);
        connect(b, &QToolButton::clicked, &dialog, [symbol, s = symbols[i]] { symbol->setCurrentText(s); });
        quick << b;
    }
    layout->addLayout(common);
    auto *otherRow = new QHBoxLayout;
    auto *periodic = new QPushButton(tr("周期表から選ぶ…"));
    periodic->setObjectName("coordinateAtomPeriodicTable");
    otherRow->addWidget(periodic);
    otherRow->addWidget(symbol);
    auto *elementName = new QLabel;
    elementName->setObjectName("coordinateAtomName");
    otherRow->addWidget(elementName, 1);
    layout->addLayout(otherRow);
    connect(periodic, &QPushButton::clicked, &dialog, [&] {
        PeriodicTableDialog table(symbol->currentText(), {}, &dialog);
        if (table.exec() == QDialog::Accepted && !table.selectedElement().isEmpty())
            symbol->setCurrentText(table.selectedElement());
    });
    heading(tr("位置 / %1").arg(unit == CoordinateUnit::Bohr ? "Bohr" : "Å"));
    auto *positionRow = new QHBoxLayout;
    QList<QLineEdit *> xyz;
    for (int i = 0; i < 3; ++i) {
        auto *label = new QLabel(QString("XYZ")[i]);
        auto *field = new QLineEdit;
        field->setObjectName(QString("coordinateAtom") + QString("XYZ")[i]);
        field->setPlaceholderText("0.0");
        field->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        field->setAccessibleName(
            QString("%1 / %2").arg(QString("XYZ")[i]).arg(unit == CoordinateUnit::Bohr ? "Bohr" : "Å"));
        label->setBuddy(field);
        positionRow->addWidget(label);
        positionRow->addWidget(field, 1);
        xyz << field;
    }
    layout->addLayout(positionRow);
    auto *nearest = new QLabel;
    nearest->setObjectName("coordinateAtomNearest");
    nearest->setWordWrap(true);
    layout->addWidget(nearest);
    heading(tr("質量（省略可）"));
    auto *mass = new QLineEdit;
    mass->setObjectName("coordinateAtomMass");
    mass->setPlaceholderText(tr("既定"));
    layout->addWidget(mass);
    layout->addWidget(note(tr("整数＝質量数（例: 13 = ¹³C）、小数＝amu、空欄＝既定値")));
    layout->addSpacing(4);
    auto *ghost = new QCheckBox(tr("ゴースト原子"));
    layout->addWidget(ghost);
    layout->addWidget(note(tr("NWChemではbq元素として出力します。基底の割当が別途必要です。"), true));
    auto *fixed = new QCheckBox(tr("固定原子"));
    layout->addWidget(fixed);
    layout->addWidget(note(tr("座標編集・削除を禁止し、NWChemのconstraints fix atomへ出力します。"), true));
    auto *error = new QLabel;
    error->setWordWrap(true);
    auto errorPalette = error->palette();
    errorPalette.setColor(QPalette::WindowText, errorColor(errorPalette));
    error->setPalette(errorPalette);
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("1原子を追加"));
    layout->addWidget(buttons);
    std::optional<Atom> atom;
    const auto &current = window->document();
    const auto places = atomPlaces(current);
    auto validate = [&] {
        atom.reset();
        // The nearest atom as soon as X, Y and Z are numbers, before an element is chosen too.
        nearest->clear();
        try {
            const Vec p =
                coordinateAtom("H", {xyz[0]->text(), xyz[1]->text(), xyz[2]->text()}, "", unit).position;
            int closest = -1;
            double distance = 0;
            for (int i = 0; i < current.atoms.size(); ++i)
                if (const double r = (current.atoms[i].position - p).norm(); closest < 0 || r < distance) {
                    closest = i;
                    distance = r;
                }
            if (closest >= 0) {
                const auto &a = current.atoms[closest];
                const auto place = places.value(a.id);
                const auto number = QString::number(closest + 1);
                nearest->setText((place.isEmpty()
                                      ? tr("最も近い原子は %1%2、%3 Å").arg(a.element, number)
                                      : tr("最も近い原子は %1%2（%3）、%4 Å").arg(a.element, number, place))
                                     .arg(distance, 0, 'f', 3));
            }
        } catch (const std::exception &) {
        }
        try {
            auto entered =
                coordinateAtom(symbol->currentText(), {xyz[0]->text(), xyz[1]->text(), xyz[2]->text()},
                               mass->text(), unit, ghost->isChecked(), fixed->isChecked());
            Document next = window->document();
            next.atoms << entered;
            auto errors = next.validate();
            if (!errors.isEmpty())
                fail(errors.join('\n'));
            atom = entered;
            error->clear();
        } catch (const std::exception &e) {
            error->setText(QString::fromUtf8(e.what()));
        }
        buttons->button(QDialogButtonBox::Ok)->setEnabled(atom.has_value());
    };
    const auto chosen = [&] {
        const auto s = symbol->currentText();
        for (auto *b : std::as_const(quick))
            b->setChecked(b->text() == s);
        elementName->setText(s == "D"      ? tr("重水素")
                             : s == "T"    ? tr("三重水素")
                             : s.isEmpty() ? QString()
                                           : element(s).name());
        validate();
    };
    for (auto *field : xyz)
        connect(field, &QLineEdit::textChanged, &dialog, validate);
    connect(mass, &QLineEdit::textChanged, &dialog, validate);
    connect(symbol, &QComboBox::currentIndexChanged, &dialog, chosen);
    connect(ghost, &QCheckBox::toggled, &dialog, validate);
    connect(fixed, &QCheckBox::toggled, &dialog, validate);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        validate();
        if (atom)
            dialog.accept();
    });
    validate();
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (base != window->revision() || !editable())
        fail(tr("入力中に分子が変更されました。原子を追加し直してください。"));
    window->edit(tr("座標から原子を追加"), [&](Document &d) { d.atoms << *atom; });
    window->setSelection({atom->id});
    table->scrollTo(model->index(model->rowCount() - 1, 0));
    window->canvas()->keepInView();
}
void CoordinatesPanel::previewStructure(const Document &structure) {
    window->edit(
        tr("座標から構造を読込"), [&](Document &d) { replaceCoordinateStructure(d, structure); }, true);
}
void CoordinatesPanel::pasteXyz() {
    const auto base = window->revision();
    bool ok;
    const QString text = QInputDialog::getMultiLineText(
        this, tr("XYZを貼り付けて置き換え"),
        tr("原子数、コメント、元素 X Y Z（Å）。今の構造を置き換える候補を作ります。"),
        QApplication::clipboard()->text(), &ok);
    if (!ok)
        return;
    if (base != window->revision())
        fail(tr("入力中に分子が変更されました。貼り付けをやり直してください。"));
    previewStructure(xyzStructure(text));
}
void CoordinatesPanel::importFile() {
    const auto base = window->revision();
    QString path = QFileDialog::getOpenFileName(
        this, tr("構造を読み込んで置換候補を作成"), window->preference("directory").toString(),
        tr("分子構造 (*.xyz *.mol *.sdf *.pdb *.ent *.cif *.mmcif *.gz)"));
    if (path.isEmpty())
        return;
    if (base != window->revision())
        fail(tr("読込中に分子が変更されました。読込をやり直してください。"));
    importPath(path);
}
void CoordinatesPanel::importPath(const QString &path) {
    window->checkMcpPath(path);
    if (!editable())
        fail(tr("候補を確定・取消し、実行中の整形を終了してから読み込んでください。"));
    const auto base = window->revision();
    const auto options = chooseStructureImport(this, path);
    if (!options)
        return;
    if (base != window->revision())
        fail(tr("読込中に分子が変更されました。読込をやり直してください。"));
    previewStructure(importStructure(path, options->model, options->altloc, options->frame));
    window->setPreference("directory", QFileInfo(path).absolutePath());
}
void CoordinatesPanel::dragEnterEvent(QDragEnterEvent *event) {
    // Structure files become a replacement candidate here; other files (results, projects) go on to
    // the window, which opens them.
    const auto urls = event->mimeData()->urls();
    if (urls.size() == 1 && urls.first().isLocalFile() &&
        structureSuffixes.contains(structureSuffix(urls.first().toLocalFile())))
        event->acceptProposedAction();
    else
        event->ignore();
}
void CoordinatesPanel::dropEvent(QDropEvent *event) {
    const auto path = event->mimeData()->urls().value(0).toLocalFile();
    if (path.isEmpty())
        return;
    event->acceptProposedAction();
    // Read after the drop finishes, so the PDB dialogs do not run inside the drag-and-drop loop.
    QTimer::singleShot(0, this, [this, path] { attempt([&] { importPath(path); }); });
}
QSize CoordinatesPanel::minimumSizeHint() const {
    // The toolbar and the selection bar wrap where the panel is narrow. The splitter beside the 3D view
    // reads the minimum, not the height for width, and would otherwise squeeze the table under them.
    auto size = QWidget::minimumSizeHint();
    if (layout()->hasHeightForWidth())
        size.setHeight(std::max(size.height(), layout()->totalMinimumHeightForWidth(width())));
    return size;
}
void CoordinatesPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (event->size().width() != event->oldSize().width())
        updateGeometry(); // The bars may wrap differently: the minimum height follows.
}
void CoordinatesPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    // Shown with "1 分子" or by its layout: catch up with what changed while hidden.
    window->refreshPanel(this);
}
void CoordinatesPanel::exportText(bool xyz, bool save) {
    if (!editable())
        fail(tr("候補を確定・取消し、実行中の整形を終了してから出力してください。"));
    const QString text = xyz ? window->document().xyz() : nwchemGeometry(window->document(), exportUnit());
    if (!save) {
        QApplication::clipboard()->setText(text);
        window->statusBar()->showMessage(
            xyz ? tr("XYZ（Å）をコピーしました。") : tr("NWChem座標ブロックをコピーしました。"), 5000);
        return;
    }
    QString path = QFileDialog::getSaveFileName(
        this, xyz ? tr("XYZを保存（Å）") : tr("NWChem座標ブロックを保存"),
        xyz ? "molecule.xyz" : "geometry.nw", xyz ? "XYZ (*.xyz)" : "NWChem geometry fragment (*.nw)");
    if (path.isEmpty())
        return;
    QString suffix = xyz ? ".xyz" : ".nw";
    if (!path.endsWith(suffix, Qt::CaseInsensitive))
        path += suffix;
    writeFile(path, text.toUtf8());
}
void CoordinatesPanel::applyChromeStyle() {
    const auto pal = static_cast<QWidget *>(window)->palette();
    const ChromeColors c(pal);
    setStyleSheet(chromeRules(settingsRules() + R"(
        #coordinateSelectionBar, #coordinateFooter, #coordinateEmpty { background: @base; }
        #coordinateSelectionBar { border-bottom: 1px solid @line; }
        #coordinateFooter { border-top: 1px solid @line; }
        #coordinateFooter QLabel, QLabel#coordinateSelectionHint, #coordinateEmptyCard QLabel { color: @muted; }
        QLineEdit#coordinateFilter { border: 1px solid @line; border-radius: 6px; padding: 4px 4px;
                                     background: @base; color: @text; }
        QPushButton#coordinateAttributeColumns:checked { background: @tint; border-color: @accent;
                                                         color: @accentText; font-weight: bold; }
        QLabel#coordinateSelectionTitle, QLabel#coordinateCandidateTitle { font-weight: bold; }
        QLabel[chip="true"] { border: 1px solid @line; border-radius: 11px; padding: 2px 8px; background: @base;
                              color: @text; }
        QLabel#coordinateMeasurement { color: @accentText; font-weight: bold; }
        QToolButton#coordinateDeselect { border: none; border-radius: 6px; padding: 4px; }
        QToolButton#coordinateDeselect:hover { background: @hover; }
        #coordinateSelectionBar[candidate="true"] { background: @candidate; border-bottom: 1px solid @candidateLine; }
        #coordinateSelectionBar[candidate="true"] QLabel { color: @candidateText; }
        #coordinateEmptyCard { border: 1px dashed @line; border-radius: 12px; }
        QLabel#coordinateEmptyTitle { color: @text; font-weight: bold; font-size: 16px; }
    )",
                              pal));
    for (auto *icon : findChildren<QLabel *>("coordinateCandidateIcon"))
        icon->setPixmap(lineIcon("layers", c.candidateText).pixmap(QSize(16, 16)));
    findChild<QLabel *>("coordinateEmptyPicture")->setPixmap(emptyPicture(c));
    // Selected rows in MolForma's tint, whatever the desktop's selection color.
    auto tablePalette = table->palette();
    tablePalette.setColor(QPalette::Highlight, c.tint);
    tablePalette.setColor(QPalette::HighlightedText, c.text);
    table->setPalette(tablePalette);
    model->clearIcons();
    refresh();
}
} // namespace mf
