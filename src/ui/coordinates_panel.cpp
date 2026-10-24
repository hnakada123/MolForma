// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/coordinates_panel.h"
#include "editing/chemistry.h"
#include "model/geometry.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#include <QtWidgets>

namespace mf {
namespace {
// Coordinates read at a glance with six decimals, right aligned; editing a cell shows and keeps
// the full value, so an unchanged cell never rounds the geometry.
class CoordinateDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QString displayText(const QVariant &value, const QLocale &locale) const override {
        bool ok;
        const double number = value.toString().toDouble(&ok);
        return ok ? QString::number(number, 'f', 6) : QStyledItemDelegate::displayText(value, locale);
    }

  protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        option->displayAlignment = Qt::AlignRight | Qt::AlignVCenter;
    }
};
} // namespace
CoordinatesPanel::CoordinatesPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("coordinatesTab");
    auto *layout = new QVBoxLayout(this);
    auto *files = new QHBoxLayout;
    layout->addLayout(files);
    button(files, "構造を読込…", "coordinateImport", [this] { importFile(); }, true);
    button(files, "XYZを貼り付け…", "coordinatePaste", [this] { pasteXyz(); }, true);
    xyzCopy = button(files, "XYZをコピー", "coordinateCopyXyz", [this] { exportText(true, false); });
    xyzSave = button(files, "XYZを保存…", "coordinateSaveXyz", [this] { exportText(true, true); });
    files->addStretch();
    files->addWidget(new QLabel("表の単位"));
    units = new QComboBox;
    units->setObjectName("coordinateUnits");
    units->addItems({"Å", "Bohr"});
    files->addWidget(units);
    connect(units, &QComboBox::currentIndexChanged, this, [this] { refresh(); });

    auto *edits = new QHBoxLayout;
    layout->addLayout(edits);
    undoButton = button(edits, "元に戻す", "coordinateUndo", [this] {
        if (window->hasCandidate())
            window->cancelCandidate();
        else
            window->history()->undo();
    });
    redoButton = button(edits, "やり直す", "coordinateRedo", [this] { window->history()->redo(); });
    button(edits, "原子を追加…", "coordinateAdd", [this] { addAtom(); }, true);
    auto remove = [this] {
        auto ids = window->requireSelection();
        window->edit("座標表の選択原子を削除", [ids](Document &d) { d.remove(ids); }, true);
    };
    button(edits, "選択を削除", "coordinateRemove", remove, true);
    button(
        edits, "全原子を削除", "coordinateClear",
        [this] { window->edit("全原子を削除", [](Document &d) { d.remove(d.ids()); }, true); }, true);
    button(
        edits, "選択原子を原点へ", "coordinateOrigin",
        [this] {
            auto id = window->requireSelection(1).first();
            window->edit("選択原子を原点へ", [id](Document &d) {
                Vec shift = -d.atom(id).position;
                if (!shift.isZero(0.))
                    for (const auto &atom : d.atoms)
                        if (atom.fixed)
                            fail("固定原子を動かす操作です。固定を解除してください。");
                transform(d, d.ids(), Mat::Identity(), shift);
            });
            view->fit();
            window->canvas()->fit();
        },
        true);
    edits->addStretch();
    auto *hint =
        new QLabel("セルを編集して確定すると反映します。質量: 整数＝質量数、小数＝amu、空欄＝既定値。"
                   "XYZの入出力は常にÅです。");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    message = new QLabel;
    message->setObjectName("coordinateMessage");
    message->setWordWrap(true);
    auto messagePalette = message->palette();
    messagePalette.setColor(QPalette::WindowText, errorColor(messagePalette));
    message->setPalette(messagePalette);
    message->hide();
    layout->addWidget(message);
    auto *splitter = new QSplitter;
    layout->addWidget(splitter, 1);
    table = new QTableWidget(0, 7);
    table->setObjectName("coordinateTable");
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setSortingEnabled(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    table->verticalHeader()->setDefaultSectionSize(30);
    table->setAlternatingRowColors(true);
    table->setMinimumWidth(360);
    splitter->addWidget(table);
    for (int i = 0; i < 7; ++i)
        table->setColumnWidth(i, i >= 1 && i <= 3 ? 140 : i == 4 ? 115 : 72);
    for (int column = 1; column <= 3; ++column)
        table->setItemDelegateForColumn(column, new CoordinateDelegate(table));
    connect(table, &QTableWidget::itemChanged, this, &CoordinatesPanel::cellChanged);
    connect(table, &QTableWidget::itemSelectionChanged, this, [this] {
        if (busy)
            return;
        QList<bool> marked(table->rowCount(), false);
        for (const auto &range : table->selectionModel()->selection())
            for (int row = range.top(); row <= range.bottom(); ++row)
                marked[row] = true;
        Ids ids;
        for (int row = 0; row < table->rowCount(); ++row)
            if (marked[row])
                ids << table->item(row, 0)->data(Qt::UserRole).toString();
        // Retain click order from the molecule view when adding table rows.
        const QSet<QString> rows(ids.begin(), ids.end());
        Ids ordered;
        QSet<QString> kept;
        for (const auto &id : window->selected)
            if (rows.contains(id) && !kept.contains(id)) {
                ordered << id;
                kept.insert(id);
            }
        for (const auto &id : ids)
            if (!kept.contains(id))
                ordered << id;
        window->setSelection(ordered);
    });
    auto *removeShortcut = new QShortcut(QKeySequence::Delete, table);
    removeShortcut->setContext(Qt::WidgetShortcut);
    connect(removeShortcut, &QShortcut::activated, this, [this, remove] {
        if (editable())
            attempt(remove);
    });
    auto *right = new QWidget;
    auto *rl = new QVBoxLayout(right);
    rl->setContentsMargins(4, 0, 0, 0);
    right->setMinimumWidth(300);
    view = new MoleculeView;
    view->setObjectName("coordinateView");
    view->setTool("select");
    rl->addWidget(view, 1);
    auto *camera = new QHBoxLayout;
    rl->addLayout(camera);
    button(camera, "全体を表示", "coordinateFit", [this] { view->fit(); });
    for (int i = 0; i < 3; ++i)
        button(camera, QString("+") + QString("XYZ")[i], "coordinateView" + QString::number(i), [this, i] {
            view->viewFrom(i);
        })->setToolTip(QString("%1軸の正の側から見ます。").arg(QString("XYZ")[i]));
    camera->addStretch();
    connect(view, &MoleculeView::atomClicked, this, [this](const QString &id, Qt::KeyboardModifiers) {
        auto ids = window->selected;
        if (ids.contains(id))
            ids.removeAll(id);
        else
            ids << id;
        window->setSelection(ids);
    });
    connect(view, &MoleculeView::blankClicked, this, [this] { window->setSelection({}); });
    connect(view, &MoleculeView::bondClicked, this,
            [this](const QString &a, const QString &b) { window->setSelection({a, b}); });
    connect(view, &MoleculeView::rectangleSelected, this, [this](const Ids &ids, bool append) {
        auto next = append ? window->selected : Ids();
        for (const auto &id : ids)
            if (!next.contains(id))
                next << id;
        window->setSelection(next);
    });
    selectionLabel = new QLabel;
    selectionLabel->setWordWrap(true);
    rl->addWidget(selectionLabel);
    auto *exportRow = new QHBoxLayout;
    rl->addLayout(exportRow);
    exportRow->addWidget(new QLabel("NWChem出力単位"));
    outputUnits = new QComboBox;
    outputUnits->setObjectName("nwchemUnits");
    outputUnits->addItems({"Å", "Bohr"});
    exportRow->addWidget(outputUnits);
    editControls << outputUnits;
    exportRow->addStretch();
    connect(outputUnits, &QComboBox::currentIndexChanged, this, [this] {
        if (busy)
            return;
        auto unit = exportUnit() == CoordinateUnit::Bohr ? "bohr" : "angstrom";
        attempt([&] {
            window->edit("NWChem座標の出力単位を変更", [unit](Document &d) {
                auto settings = d.data["nwchem"].toObject();
                settings["geometry_units"] = unit;
                d.data["nwchem"] = settings;
            });
        });
    });
    previewLabel = new QLabel;
    previewLabel->setObjectName("nwchemPreviewLabel");
    previewLabel->setWordWrap(true);
    rl->addWidget(previewLabel);
    preview = new QPlainTextEdit;
    preview->setObjectName("nwchemGeometryPreview");
    preview->setReadOnly(true);
    preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    rl->addWidget(preview, 1);
    auto *exports = new QHBoxLayout;
    rl->addLayout(exports);
    copyButton =
        button(exports, "座標ブロックをコピー", "copyNwchemGeometry", [this] { exportText(false, false); });
    saveButton = button(exports, "保存…", "saveNwchemGeometry", [this] { exportText(false, true); });
    exports->addStretch();
    auto *note =
        new QLabel("座標と固定原子の入力断片です。計算には電荷・基底・手法・taskの設定を追加してください。"
                   "点電荷や旧形式の拘束・断片設定は含みません。");
    note->setWordWrap(true);
    rl->addWidget(note);
    splitter->addWidget(right);
    splitter->setSizes({800, 440});
}
QPushButton *CoordinatesPanel::button(QHBoxLayout *layout, const QString &text, const QString &name,
                                      const std::function<void()> &action, bool editing) {
    auto *b = new QPushButton(text);
    b->setObjectName(name);
    layout->addWidget(b);
    if (editing)
        editControls << b;
    connect(b, &QPushButton::clicked, this, [this, action] { attempt(action); });
    return b;
}
bool CoordinatesPanel::editable() const {
    return !window->hasCandidate() && !window->worker;
}
CoordinateUnit CoordinatesPanel::tableUnit() const {
    return units->currentIndex() == 1 ? CoordinateUnit::Bohr : CoordinateUnit::Angstrom;
}
CoordinateUnit CoordinatesPanel::exportUnit() const {
    return outputUnits->currentIndex() == 1 ? CoordinateUnit::Bohr : CoordinateUnit::Angstrom;
}
void CoordinatesPanel::attempt(const std::function<void()> &action) {
    message->clear();
    message->hide();
    try {
        action();
    } catch (const std::exception &e) {
        // Restore rejected cells immediately; there is no hidden draft to export.
        refresh();
        message->setText(QString::fromUtf8(e.what()) + "\n操作は反映されていません。");
        message->show();
    }
}
void CoordinatesPanel::refresh() {
    if (busy)
        return;
    QScopedValueRollback guard(busy, true);
    const auto &d = window->displayDocument();
    const QSignalBlocker blocker(table);
    const bool changedAtoms = displayedIds != d.ids();
    displayedIds = d.ids();
    const auto unit = tableUnit();
    QString label = unit == CoordinateUnit::Bohr ? "Bohr" : "Å";
    table->setHorizontalHeaderLabels(
        {"元素", "X / " + label, "Y / " + label, "Z / " + label, "質量数 / amu", "ゴースト", "固定"});
    table->setRowCount(d.atoms.size());
    for (int row = 0; row < d.atoms.size(); ++row) {
        const auto &atom = d.atoms[row];
        for (int col = 0; col < table->columnCount(); ++col) {
            auto *item = table->item(row, col);
            if (!item) {
                item = new QTableWidgetItem;
                table->setItem(row, col, item);
            }
            item->setData(Qt::UserRole, atom.id);
            auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (col < 5) {
                if (editable() && !(atom.fixed && col >= 1 && col <= 3))
                    flags |= Qt::ItemIsEditable;
                item->setText(coordinateText(atom, CoordinateColumn(col), unit));
            } else {
                if (editable())
                    flags |= Qt::ItemIsUserCheckable;
                item->setCheckState((col == 5 ? atom.ghost : atom.fixed) ? Qt::Checked : Qt::Unchecked);
            }
            item->setFlags(flags);
            item->setToolTip(col == 4   ? "例: 13 = ¹³C、13.003355 = 質量（amu）、空欄 = 既定値"
                             : col == 5 ? "NWChemではbq元素として出力します。基底の割当が別途必要です。"
                             : col == 6 ? "座標編集・削除を禁止し、NWChemのconstraints fix atomへ出力します。"
                                        : "");
        }
    }
    for (auto *control : editControls)
        control->setEnabled(editable());
    undoButton->setEnabled(!window->worker && (window->hasCandidate() || window->history()->canUndo()));
    redoButton->setEnabled(editable() && window->history()->canRedo());
    outputUnits->setCurrentIndex(d.data["nwchem"].toObject()["geometry_units"] == "bohr" ? 1 : 0);
    QString text;
    bool valid = false;
    try {
        text = nwchemGeometry(d, exportUnit());
        valid = true;
    } catch (const std::exception &e) {
        text = QString::fromUtf8(e.what());
    }
    if (preview->toPlainText() != text)
        preview->setPlainText(text);
    previewLabel->setText(window->hasCandidate() ? "未確定候補のNWChem座標（確定後に出力できます）"
                                                 : "NWChem 座標・固定原子プレビュー");
    copyButton->setEnabled(valid && editable());
    saveButton->setEnabled(valid && editable());
    xyzCopy->setEnabled(!d.atoms.isEmpty() && editable());
    xyzSave->setEnabled(!d.atoms.isEmpty() && editable());
    view->setDocument(d);
    if (changedAtoms)
        view->fit();
    refreshSelection();
}
void CoordinatesPanel::refreshSelection() {
    const QSignalBlocker blocker(table);
    // One selection of row ranges: selecting item by item is quadratic in the selected rows.
    const QSet<QString> chosen(window->selected.begin(), window->selected.end());
    const int last = table->columnCount() - 1;
    QItemSelection rows;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (!chosen.contains(table->item(row, 0)->data(Qt::UserRole).toString()))
            continue;
        if (!rows.isEmpty() && rows.last().bottom() == row - 1)
            rows.last() = QItemSelectionRange(rows.last().topLeft(), table->model()->index(row, last));
        else
            rows.select(table->model()->index(row, 0), table->model()->index(row, last));
    }
    table->selectionModel()->select(rows, QItemSelectionModel::ClearAndSelect);
    view->setSelection(window->selected);
    selectionLabel->setText(window->measurement->text());
}
void CoordinatesPanel::cellChanged(QTableWidgetItem *item) {
    if (busy)
        return;
    // Copy values before edit() refreshes the view/table.
    const QString id = item->data(Qt::UserRole).toString();
    const auto column = CoordinateColumn(item->column());
    const auto text = item->column() < 5 ? item->text() : item->checkState() == Qt::Checked ? "1" : "0";
    const auto unit = tableUnit();
    attempt([&] {
        window->edit("座標表を編集", [=](Document &d) { setCoordinateCell(d, id, column, text, unit); });
    });
    refresh();
}
void CoordinatesPanel::addAtom() {
    if (!editable())
        fail("候補を確定・取消し、実行中の整形を終了してください。");
    const auto base = window->document().toJson();
    const auto unit = tableUnit();
    QDialog dialog(this);
    dialog.setObjectName("coordinateAtomDialog");
    dialog.setWindowTitle("座標を指定して原子を追加");
    dialog.setMinimumWidth(440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel("1原子を末尾に追加します。結合や水素の補完は「1 分子」で行えます。");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    auto *symbol = new QComboBox;
    symbol->setObjectName("coordinateAtomElement");
    for (const auto &e : elements())
        symbol->addItem(e.symbol);
    symbol->addItems({"D", "T"});
    symbol->setCurrentIndex(-1);
    form->addRow("元素", symbol);
    QList<QLineEdit *> xyz;
    for (int i = 0; i < 3; ++i) {
        auto *field = new QLineEdit;
        field->setObjectName(QString("coordinateAtom") + QString("XYZ")[i]);
        field->setPlaceholderText("0.0");
        form->addRow(QString("XYZ")[i] + QString(unit == CoordinateUnit::Bohr ? " / Bohr" : " / Å"), field);
        xyz << field;
    }
    auto *mass = new QLineEdit;
    mass->setObjectName("coordinateAtomMass");
    form->addRow("質量数 / amu", mass);
    auto *ghost = new QCheckBox("ゴースト原子");
    auto *fixed = new QCheckBox("固定原子");
    form->addRow(ghost);
    form->addRow(fixed);
    auto *error = new QLabel;
    error->setWordWrap(true);
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("1原子を追加");
    layout->addWidget(buttons);
    std::optional<Atom> atom;
    auto validate = [&] {
        atom.reset();
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
    for (auto *field : xyz)
        connect(field, &QLineEdit::textChanged, &dialog, validate);
    connect(mass, &QLineEdit::textChanged, &dialog, validate);
    connect(symbol, &QComboBox::currentIndexChanged, &dialog, validate);
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
    if (base != window->document().toJson() || !editable())
        fail("入力中に分子が変更されました。原子を追加し直してください。");
    window->edit("座標から原子を追加", [&](Document &d) {
        d.atoms << *atom;
        d.invalidateAssignments();
    });
    window->setSelection({atom->id});
    table->scrollToItem(table->item(table->rowCount() - 1, 0));
    window->canvas()->fit();
}
void CoordinatesPanel::previewStructure(const Document &structure) {
    window->edit("座標から構造を読込", [&](Document &d) { replaceCoordinateStructure(d, structure); }, true);
}
void CoordinatesPanel::pasteXyz() {
    const auto base = window->document().toJson();
    bool ok;
    const QString text = QInputDialog::getMultiLineText(
        this, "XYZを貼り付け", "原子数、コメント、元素 X Y Z（Å）", QApplication::clipboard()->text(), &ok);
    if (!ok)
        return;
    if (base != window->document().toJson())
        fail("入力中に分子が変更されました。貼り付けをやり直してください。");
    previewStructure(xyzStructure(text));
}
void CoordinatesPanel::importFile() {
    const auto base = window->document().toJson();
    QString path = QFileDialog::getOpenFileName(this, "構造を読み込んで置換候補を作成",
                                                window->settings.value("directory").toString(),
                                                "分子構造 (*.xyz *.mol *.sdf *.pdb *.ent)");
    if (path.isEmpty())
        return;
    int model = 1;
    QString altloc;
    if (path.endsWith(".pdb", Qt::CaseInsensitive) || path.endsWith(".ent", Qt::CaseInsensitive)) {
        const auto text = QString::fromUtf8(readFile(path));
        const auto models = pdbModels(text);
        bool ok;
        if (models.size() > 1) {
            model = QInputDialog::getItem(this, "PDB MODEL", "モデル", models, 0, false, &ok).toInt();
            if (!ok)
                return;
        } else if (!models.isEmpty())
            model = models.first().toInt();
        if (text.contains(QRegularExpression("(?m)^(ATOM  |HETATM).{10}[A-Z]"))) {
            altloc =
                QInputDialog::getItem(this, "PDB ALTLOC", "代替位置", {"自動", "A", "B", "C"}, 0, true, &ok);
            if (!ok)
                return;
            if (altloc == "自動")
                altloc.clear();
        }
    }
    if (base != window->document().toJson())
        fail("読込中に分子が変更されました。読込をやり直してください。");
    previewStructure(importStructure(path, model, altloc));
    window->settings.setValue("directory", QFileInfo(path).absolutePath());
}
void CoordinatesPanel::exportText(bool xyz, bool save) {
    if (!editable())
        fail("候補を確定・取消し、実行中の整形を終了してから出力してください。");
    const QString text = xyz ? window->document().xyz() : nwchemGeometry(window->document(), exportUnit());
    if (!save) {
        QApplication::clipboard()->setText(text);
        window->statusBar()->showMessage(
            xyz ? "XYZ（Å）をコピーしました。" : "NWChem座標ブロックをコピーしました。", 5000);
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, xyz ? "XYZを保存（Å）" : "NWChem座標ブロックを保存",
                                                xyz ? "molecule.xyz" : "geometry.nw",
                                                xyz ? "XYZ (*.xyz)" : "NWChem geometry fragment (*.nw)");
    if (path.isEmpty())
        return;
    QString suffix = xyz ? ".xyz" : ".nw";
    if (!path.endsWith(suffix, Qt::CaseInsensitive))
        path += suffix;
    writeFile(path, text.toUtf8());
}
} // namespace mf
