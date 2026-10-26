#include "editing/peptide.h"
#include "model/geometry.h"
#include "ui/coordinates_panel.h"
#include "ui/main_window.h"
#include <QtTest>
#include <QtWidgets>
using namespace mf;

class CoordinatesTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDir;
    static Document water() {
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        d.envelope["result_reference"] = QJsonObject{{"path", "existing-result.out"}};
        return d;
    }
    static QTableWidget *table(MainWindow &w) {
        return w.findChild<QTableWidget *>("coordinateTable");
    }
    static QAbstractButton *button(MainWindow &w, const char *name) {
        return w.findChild<QAbstractButton *>(name);
    }
    // The commands of the table's menus ("構造", "書き出し").
    static QAction *action(MainWindow &w, const char *name) {
        return w.findChild<QAction *>(name);
    }
  private slots:
    void initTestCase() {
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settingsDir.path()); // MolForma uses NativeFormat.
    }
    void nativeAtomInput() {
        auto a = coordinateAtom("D", {"1D0", "-2e-1", "0"}, "", CoordinateUnit::Bohr);
        QCOMPARE(a.element, QString("H"));
        QCOMPARE(a.isotope, 2);
        QCOMPARE(a.position.x(), BohrToAngstrom);
        QCOMPARE(a.position.y(), -0.2 * BohrToAngstrom);
        auto b = coordinateAtom("@cl", {"1", "2", "3"}, "35", CoordinateUnit::Angstrom);
        QCOMPARE(b.element, QString("Cl"));
        QVERIFY(b.ghost);
        QCOMPARE(b.isotope, 35);
        b = coordinateAtom("C", {"1", "2", "3"}, "13.0", CoordinateUnit::Angstrom);
        QCOMPARE(b.mass, 13.0);
        QCOMPARE(b.isotope, 0);
        QCOMPARE(coordinateText(b, CoordinateColumn::Mass, CoordinateUnit::Angstrom), QString("13.0"));
        for (const auto &bad : {"nan", "inf", "1,2", "1e999", ""})
            QVERIFY_EXCEPTION_THROWN(coordinateAtom("C", {bad, "0", "0"}, "", CoordinateUnit::Angstrom),
                                     std::runtime_error);
        for (const auto &bad : {"0", "-1", "nan", "999", "1e999"})
            QVERIFY_EXCEPTION_THROWN(coordinateAtom("C", {"0", "0", "0"}, bad, CoordinateUnit::Angstrom),
                                     std::runtime_error);
    }
    void tabEditingUnitsUndoAndPrecision() {
        MainWindow w;
        auto d = water();
        d.atoms[0].position = Vec(0.12345678901234567, -0.4321987654321987, 0.33333333333333331);
        w.loadDocument(d);
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 6);
        for (int i = 0; i < tabs->count(); ++i)
            QVERIFY(!tabs->tabText(i).contains("座標")); // The table is part of "1 分子".
        w.showCoordinateTable();
        QCOMPARE(tabs->currentIndex(), 0);
        auto *t = table(w);
        QVERIFY(w.findChild<QWidget *>("moleculeSplit")->isAncestorOf(t));
        QCOMPARE(t->rowCount(), 3);
        auto *units = w.findChild<QComboBox *>("coordinateUnits");
        for (int i = 0; i < 20; ++i)
            units->setCurrentIndex(i % 2);
        QCOMPARE(w.document().toJson(), d.toJson());
        QCOMPARE(w.history()->count(), 0);
        QVERIFY(std::abs(t->item(0, 1)->text().toDouble() - d.atoms[0].position.x() / BohrToAngstrom) <
                1e-13);
        // Shown with six decimals; the cell (and its editor) keeps the full value.
        auto *delegate = qobject_cast<QStyledItemDelegate *>(t->itemDelegateForColumn(1));
        QVERIFY(delegate);
        QCOMPARE(delegate->displayText(t->item(0, 1)->text(), QLocale()),
                 QString::number(d.atoms[0].position.x() / BohrToAngstrom, 'f', 6));
        t->item(0, 1)->setText("2");
        QCOMPARE(w.document().atoms[0].position.x(), 2 * BohrToAngstrom);
        QCOMPARE(w.document().atoms[0].position.y(), d.atoms[0].position.y());
        QCOMPARE(w.document().atoms[0].position.z(), d.atoms[0].position.z());
        QCOMPARE(w.document().atoms[0].id, d.atoms[0].id);
        QCOMPARE(w.document().bonds.size(), d.bonds.size());
        QCOMPARE(w.document().envelope, d.envelope);
        QCOMPARE(w.history()->count(), 1);
        button(w, "editorUndo")->click();
        QCOMPARE(w.document().toJson(), d.toJson());
        button(w, "editorRedo")->click();
        QCOMPARE(w.document().atoms[0].position.x(), 2 * BohrToAngstrom);
        action(w, "coordinateCopyXyz")->trigger();
        auto copied = Document::fromXyz(QApplication::clipboard()->text());
        QVERIFY(std::abs(copied.atoms[0].position.x() - 2 * BohrToAngstrom) < 1e-9);
        // View operations never change physical geometry or add history.
        auto before = w.document().toJson();
        button(w, "viewAxisX")->click();
        button(w, "viewFit")->click();
        QCOMPARE(w.document().toJson(), before);
        QCOMPARE(w.history()->count(), 1);
    }
    void validationAndFixedAtoms() {
        MainWindow w;
        auto d = water();
        d.atoms[0].fixed = true;
        w.loadDocument(d);
        auto *t = table(w);
        QVERIFY(!(t->item(0, 1)->flags() & Qt::ItemIsEditable));
        t->item(0, 1)->setText("1");
        QCOMPARE(w.document().toJson(), d.toJson());
        QCOMPARE(t->item(0, 1)->text(), QString("0"));
        QVERIFY(!w.findChild<QLabel *>("coordinateMessage")->text().isEmpty());
        for (const auto &text : {"nan", "inf", "abc", "1e999"}) {
            t->item(1, 1)->setText(text);
            QCOMPARE(w.document().toJson(), d.toJson());
        }
        t->item(1, 0)->setText("Imaginary");
        QCOMPARE(w.document().toJson(), d.toJson());
        t->item(1, 4)->setText("-3");
        QCOMPARE(w.document().toJson(), d.toJson());
        QCOMPARE(w.history()->count(), 0);
        // "原点へ" is "向きをそろえる" of "変形" with one atom, and refuses to move the fixed atom.
        w.setSelection({d.atoms[1].id});
        QTimer::singleShot(0, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->accept();
        });
        button(w, "orientSelection")->click();
        QCOMPARE(w.document().toJson(), d.toJson());
        action(w, "coordinateClear")->trigger();
        QVERIFY(!w.hasCandidate());
        t->item(0, 6)->setCheckState(Qt::Unchecked);
        QVERIFY(!w.document().atoms[0].fixed);
        // Moving H1 to the origin translates the entire molecule rigidly.
        w.setSelection({d.atoms[1].id});
        const double distance = measure(w.document(), {d.atoms[0].id, d.atoms[1].id});
        button(w, "orientSelection")->click();
        QVERIFY(w.document().atoms[1].position.isZero(1e-14));
        QCOMPARE(measure(w.document(), {d.atoms[0].id, d.atoms[1].id}), distance);
        w.history()->undo();
        QCOMPARE(w.document().atoms[1].position, d.atoms[1].position);
    }
    void keyboardEditingAndMoleculeSelection() {
        MainWindow w;
        const auto d = water();
        w.loadDocument(d);
        w.showCoordinateTable();
        w.show();
        QTest::qWait(30);
        auto *t = table(w);
        t->setCurrentCell(1, 1);
        t->editItem(t->item(1, 1));
        auto *editor = t->findChild<QLineEdit *>();
        QVERIFY(editor);
        editor->selectAll();
        QTest::keyClicks(editor, "1.125");
        QTest::keyClick(editor, Qt::Key_Return);
        // QStyledItemDelegate queues Return-key commits; wait before inspecting
        // the document or destroying the editor's parent window.
        QTRY_COMPARE(w.document().atoms[1].position.x(), 1.125);
        QCOMPARE(w.history()->count(), 1);
        // The 3D view above the table selects its rows.
        auto *v = w.canvas();
        w.setSelection({});
        v->fit();
        QTest::mouseClick(v, Qt::LeftButton, Qt::NoModifier, v->atomScreenPosition(d.atoms[2].id).toPoint());
        QCOMPARE(w.canvas()->selection(), Ids{d.atoms[2].id});
        QVERIFY(t->item(2, 0)->isSelected());
        // Switching tabs and layouts cannot commit rounded display values.
        auto before = w.document().toJson();
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(1);
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(0);
        button(w, "moleculeLayout_table")->click();
        button(w, "moleculeLayout_both")->click();
        QCOMPARE(w.document().toJson(), before);
        // "表示 → 1 分子の並べ方" is the same choice, from any tab, and follows the segments.
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        auto *layout = w.findChild<QComboBox *>("moleculeLayout");
        tabs->setCurrentIndex(1);
        action(w, "moleculeLayoutAction_table")->trigger();
        QCOMPARE(tabs->currentIndex(), 0);
        QCOMPARE(layout->currentData().toString(), QString("table"));
        button(w, "moleculeLayout_both")->click();
        QVERIFY(action(w, "moleculeLayoutAction_both")->isChecked());
        QVERIFY(!action(w, "moleculeLayoutAction_table")->isChecked());
    }
    void paletteFoldsForTheTable() {
        MainWindow w;
        w.loadDocument(water());
        w.resize(1100, 720);
        w.show();
        auto *toggle = button(w, "paletteToggle");
        auto *palette = w.findChild<QWidget *>("paletteColumn");
        QVERIFY(toggle->isChecked() && palette->isVisible());
        toggle->click();
        QVERIFY(!palette->isVisible() && !toggle->isChecked());
        // The tools that place from the palette open it again.
        button(w, "tool_place")->click();
        QVERIFY(palette->isVisible() && toggle->isChecked());
        button(w, "tool_select")->click();
        // Beside the 3D view, the table keeps four rows even in a small window, where the bars above it
        // wrap: none of it is cut off.
        button(w, "moleculeLayout_both")->click();
        auto *split = w.findChild<QSplitter *>("moleculeSplit");
        auto *t = table(w);
        auto *toolbar = w.findChild<QWidget *>("coordinateToolbar");
        QTRY_VERIFY(split->sizes().value(1) > 0 && toolbar->height() > toolbar->minimumSizeHint().height());
        QTRY_VERIFY(t->parentWidget()->height() >= t->height());
        QVERIFY(t->viewport()->height() >= 4 * t->verticalHeader()->defaultSectionSize());
        button(w, "moleculeLayout_view")->click();
    }
    void viewKeepsRoomBesideTheTable() {
        // The table keeps four rows, and no more than its share: the page shown without atoms
        // is not part of its minimum height.
        MainWindow w;
        w.loadDocument(water());
        w.resize(1380, 900);
        w.show();
        button(w, "moleculeLayout_both")->click();
        auto *split = w.findChild<QSplitter *>("moleculeSplit");
        QTRY_VERIFY(split->sizes().value(0) > split->sizes().value(1));
        QVERIFY(w.findChild<QWidget *>("coordinatesTab")->minimumSizeHint().height() < 300);
        QVERIFY(table(w)->viewport()->height() >= 4 * table(w)->verticalHeader()->defaultSectionSize());
        button(w, "moleculeLayout_view")->click();
    }
    void isotopesGhostAndOverlap() {
        MainWindow w;
        auto d = water();
        w.loadDocument(d);
        auto *t = table(w);
        t->item(1, 0)->setText("D");
        QCOMPARE(w.document().atoms[1].isotope, 2);
        QCOMPARE(w.document().atoms[1].element, QString("H"));
        t->item(1, 0)->setText("C");
        QCOMPARE(w.document().atoms[1].isotope, 0);
        t->item(1, 4)->setText("13");
        t->item(1, 5)->setCheckState(Qt::Checked);
        QVERIFY(w.document().atoms[1].ghost);
        action(w, "copyNwchemGeometry")->trigger();
        QVERIFY(QApplication::clipboard()->text().contains("bqC"));
        QVERIFY(QApplication::clipboard()->text().contains("mass 13.003"));
        t->item(1, 5)->setCheckState(Qt::Unchecked);
        QVERIFY(!w.document().atoms[1].ghost);
        auto previous = w.document().toJson();
        int count = w.history()->count();
        t->item(1, 1)->setText("-0.758");
        QCOMPARE(w.document().toJson(), previous);
        QCOMPARE(w.history()->count(), count);
        QCOMPARE(t->item(1, 1)->text().toDouble(), 0.758);
    }
    void selectionCandidatesAndStableIds() {
        MainWindow w;
        auto d = water();
        w.loadDocument(d);
        w.showCoordinateTable();
        w.show();
        QTest::qWait(30);
        auto *t = table(w);
        t->selectRow(1);
        QCOMPARE(w.canvas()->selection(), Ids{d.atoms[1].id});
        w.setSelection({d.atoms[2].id, d.atoms[0].id});
        QVERIFY(t->item(2, 0)->isSelected());
        QVERIFY(t->item(0, 0)->isSelected());
        QVERIFY(!t->item(1, 0)->isSelected());
        // Deleting from the pages at the right is a candidate to confirm, shown in the table.
        w.setSelection({d.atoms[1].id});
        button(w, "deleteSelection")->click();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().atoms.size(), 3);
        QCOMPARE(t->rowCount(), 2);
        QVERIFY(!action(w, "copyNwchemGeometry")->isEnabled());
        QVERIFY(!action(w, "coordinateImport")->isEnabled());
        QVERIFY(!button(w, "coordinateAdd")->isEnabled());
        QVERIFY(!(t->item(0, 0)->flags() & Qt::ItemIsEditable));
        button(w, "editorUndo")->click(); // Cancels the candidate.
        QVERIFY(!w.hasCandidate());
        QCOMPARE(t->rowCount(), 3);
        w.setSelection({d.atoms[1].id});
        button(w, "deleteSelection")->click();
        w.confirmCandidate();
        QCOMPARE(w.document().atoms.size(), 2);
        w.history()->undo();
        QCOMPARE(w.document().toJson(), d.toJson());
        w.edit("reorder", [](Document &d) { std::reverse(d.atoms.begin(), d.atoms.end()); });
        t->item(0, 3)->setText("2.5");
        QCOMPARE(w.document().atom(d.atoms[2].id).position.z(), 2.5);
        QCOMPARE(w.document().atom(d.atoms[0].id).position.z(), 0.0);
    }
    void addAtomDialogAndClear() {
        MainWindow w;
        w.loadDocument(water());
        w.findChild<QComboBox *>("coordinateUnits")->setCurrentIndex(1);
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("coordinateAtomDialog");
            QVERIFY(dialog);
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
            auto *symbol = dialog->findChild<QComboBox *>("coordinateAtomElement");
            // The common elements are buttons that choose in the list.
            auto *nitrogen = dialog->findChild<QToolButton *>("coordinateAtomQuickN");
            nitrogen->click();
            QCOMPARE(symbol->currentText(), QString("N"));
            QVERIFY(nitrogen->isChecked());
            QCOMPARE(dialog->findChild<QLabel *>("coordinateAtomName")->text(), QString("窒素"));
            symbol->setCurrentText("He");
            QVERIFY(!nitrogen->isChecked());
            for (const auto *name : {"coordinateAtomX", "coordinateAtomY", "coordinateAtomZ"})
                dialog->findChild<QLineEdit *>(name)->setText("0");
            QVERIFY(dialog->findChild<QLabel *>("coordinateAtomNearest")->text().contains("O1、0.000 Å"));
            // Exact overlap with oxygen must be rejected.
            QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
            dialog->findChild<QLineEdit *>("coordinateAtomX")->setText("4");
            QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
            // 4 Bohr along X: nearest to the hydrogen on that side.
            QVERIFY(dialog->findChild<QLabel *>("coordinateAtomNearest")->text().contains("H2、1.449 Å"));
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        button(w, "coordinateAdd")->click();
        QCOMPARE(w.document().atoms.size(), 4);
        QCOMPARE(w.document().atoms.last().element, QString("He"));
        QCOMPARE(w.document().atoms.last().position.x(), 4 * BohrToAngstrom);
        QCOMPARE(w.document().bonds.size(), 2);
        QCOMPARE(w.canvas()->selection(), Ids{w.document().atoms.last().id});
        w.history()->undo();
        QCOMPARE(w.document().atoms.size(), 3);
        action(w, "coordinateClear")->trigger();
        QVERIFY(w.hasCandidate());
        QCOMPARE(table(w)->rowCount(), 0);
        w.confirmCandidate();
        QVERIFY(w.document().atoms.isEmpty());
        QVERIFY(!action(w, "copyNwchemGeometry")->isEnabled());
        QVERIFY(!button(w, "coordinateExport")->isEnabled());
        w.history()->undo();
        QCOMPARE(w.document().atoms.size(), 3);
    }
    void candidateChangesByOrder() {
        const auto before = water();
        // Read again from a file: new IDs, the same atoms in the same order, one appended.
        auto after = Document::fromXyz("4\nagain\nO 0.0004 0 0\nH 0.758 0 0.504\nH -0.758 0 0.6\nHe 3 0 0\n");
        auto changes = atomChanges(before, after);
        QVERIFY(changes.byOrder);
        QCOMPARE(changes.added, QSet<QString>{after.atoms[3].id});
        QCOMPARE(changes.moved, QSet<QString>{after.atoms[2].id}); // 0.0004 Å counts as the same place.
        QVERIFY(changes.removed.isEmpty());
        // Fewer atoms: the rest of the original goes.
        after.atoms.remove(1, 3);
        changes = atomChanges(before, after);
        QVERIFY(changes.byOrder && changes.added.isEmpty() && changes.moved.isEmpty());
        QCOMPARE(changes.removed.size(), 2);
        QCOMPARE(changes.removed[0].id, before.atoms[1].id);
        // Elements in another order make another structure: all removed and added.
        after = Document::fromXyz("3\nother\nH 0.758 0 0.504\nO 0 0 0\nH -0.758 0 0.504\n");
        changes = atomChanges(before, after);
        QVERIFY(!changes.byOrder);
        QCOMPARE(changes.added.size(), 3);
        QCOMPARE(changes.removed.size(), 3);
        // Atoms that keep their IDs are matched by ID, also in another order.
        after = before;
        std::reverse(after.atoms.begin(), after.atoms.end());
        after.atoms[0].position.x() += 0.01;
        changes = atomChanges(before, after);
        QVERIFY(!changes.byOrder);
        QCOMPARE(changes.moved, QSet<QString>{before.atoms[2].id});
        QVERIFY(changes.added.isEmpty() && changes.removed.isEmpty());
    }
    void labelsFilterAndSelectionBar() {
        MainWindow w;
        auto d = water();
        d.data["residues"] = QJsonArray{QJsonObject{
            {"id", "r1"},
            {"code", "HOH"},
            {"number", 1},
            {"atoms", QJsonObject{{"O", d.atoms[0].id}, {"HA", d.atoms[1].id}, {"HB", d.atoms[2].id}}}}};
        w.loadDocument(d);
        w.showCoordinateTable();
        auto *t = table(w);
        // Cells keep their model columns; the residue is shown beside the atom.
        auto *header = t->horizontalHeader();
        QCOMPARE(header->visualIndex(0), 1);
        QCOMPARE(header->visualIndex(7), 2);
        QCOMPARE(t->item(1, 7)->text(), QString("HOH 1 · HA"));
        QCOMPARE(atomPlace(w.document(), d.atoms[2].id), QString("HOH 1 · HB"));
        QVERIFY(t->isColumnHidden(8));
        auto *filter = w.findChild<QLineEdit *>("coordinateFilter");
        filter->setText("h2");
        QVERIFY(t->isRowHidden(0) && !t->isRowHidden(1) && t->isRowHidden(2));
        QVERIFY(w.findChild<QLabel *>("coordinateCount")->text().contains("表示 1"));
        filter->setText("HOH");
        QVERIFY(!t->isRowHidden(0) && !t->isRowHidden(2));
        filter->clear();
        // The selection bar shows the selection, and measures it only while the 3D view (with its
        // measurement panel) is hidden. Acting on the selection is for the pages at the right.
        w.setSelection({d.atoms[1].id, d.atoms[0].id});
        QCOMPARE(w.findChild<QLabel *>("coordinateSelectionTitle")->text(), QString("2 原子を選択"));
        QVERIFY(w.findChild<QLabel *>("coordinateSelectionAtom0")->text().contains("HOH 1 · HA"));
        auto *measurement = w.findChild<QLabel *>("coordinateMeasurement");
        QVERIFY(measurement->isHidden());
        button(w, "moleculeLayout_table")->click();
        QVERIFY(!measurement->isHidden());
        QCOMPARE(measurement->text(),
                 QString("距離 %1 Å").arg(measure(d, {d.atoms[1].id, d.atoms[0].id}), 0, 'f', 4));
        button(w, "moleculeLayout_both")->click();
        QVERIFY(measurement->isHidden());
        for (auto *name :
             {"coordinateOrigin", "coordinateFixSelection", "coordinateGhostSelection", "coordinateRemove"})
            QVERIFY(!button(w, name));
        // The pages at the right fix and ghost the selection; the table shows it.
        button(w, "fixSelectedAtoms")->click();
        QVERIFY(w.document().atoms[0].fixed && w.document().atoms[1].fixed && !w.document().atoms[2].fixed);
        QCOMPARE(w.history()->count(), 1);
        QVERIFY(t->item(0, 0)->data(Qt::UserRole + 1).toBool()); // The lock in the atom cell.
        QVERIFY(!(t->item(0, 1)->flags() & Qt::ItemIsEditable));
        button(w, "ghostSelectedAtoms")->click();
        QVERIFY(w.document().atoms[0].ghost && w.document().atoms[1].ghost);
        QCOMPARE(t->item(0, 5)->checkState(), Qt::Checked);
        action(w, "copyNwchemGeometry")->trigger();
        QVERIFY(QApplication::clipboard()->text().contains("bqO"));
        button(w, "unfixSelectedAtoms")->click();
        QVERIFY(!w.document().atoms[0].fixed && !w.document().atoms[1].fixed);
        // "座標（1原子）" waits while the table shows the same values.
        auto *position = w.findChild<QWidget *>("moleculeCard_座標（1原子）");
        QVERIFY(position->isHidden());
        button(w, "moleculeLayout_view")->click();
        QVERIFY(!position->isHidden());
        button(w, "moleculeLayout_both")->click();
        button(w, "coordinateDeselect")->click();
        QVERIFY(w.canvas()->selection().isEmpty());
        QVERIFY(!w.findChild<QLabel *>("coordinateSelectionHint")->isHidden());
        // What is written out: the NWChem fragment or XYZ, each copied or saved from one menu.
        auto *menu = qobject_cast<QPushButton *>(button(w, "coordinateExport"))->menu();
        QCOMPARE(menu->actions().size(), 5); // Two of each, with a separator between.
        action(w, "coordinateCopyXyz")->trigger();
        QVERIFY(QApplication::clipboard()->text().startsWith("3\n"));
    }
    void attributeColumns() {
        // The atom attributes of the former "座標と原子属性の一覧" dialog are columns of the table.
        MainWindow w;
        const auto d = water();
        w.loadDocument(d);
        auto *t = table(w);
        QVERIFY(t->isColumnHidden(9) && t->isColumnHidden(10) && t->isColumnHidden(11));
        w.findChild<QAction *>("coordinateAttributesAction")->trigger();
        QCOMPARE(w.findChild<QTabWidget *>("workspaceTabs")->currentIndex(), 0);
        QVERIFY(w.findChild<QComboBox *>("moleculeLayout")->currentData() != "view");
        QVERIFY(!t->isColumnHidden(9) && !t->isColumnHidden(10) && !t->isColumnHidden(11));
        QCOMPARE(t->horizontalHeaderItem(9)->text(), QString("形式電荷"));
        QCOMPARE(t->item(0, 9)->text(), QString("0"));
        t->item(0, 9)->setText("-1");
        QCOMPARE(w.document().atoms[0].charge, -1);
        t->item(0, 10)->setText("1");
        QCOMPARE(w.document().atoms[0].radical, 1);
        t->item(1, 11)->setText(" HW1 ");
        QCOMPARE(w.document().atoms[1].name, QString("HW1"));
        QCOMPARE(w.history()->count(), 3);
        // Invalid values are refused with a message and leave the document as it was.
        t->item(0, 10)->setText("9");
        QCOMPARE(w.document().atoms[0].radical, 1);
        QVERIFY(!w.findChild<QLabel *>("coordinateMessage")->isHidden());
        QCOMPARE(t->item(0, 10)->text(), QString("1"));
        t->item(0, 9)->setText("x");
        QCOMPARE(w.document().atoms[0].charge, -1);
        QCOMPARE(w.history()->count(), 3);
        w.history()->undo();
        QCOMPARE(w.document().atoms[1].name, d.atoms[1].name);
        QCOMPARE(t->item(1, 11)->text(), d.atoms[1].name);
        // The columns stay as chosen (a preference of the table).
        button(w, "coordinateAttributeColumns")->click();
        QVERIFY(t->isColumnHidden(9));
    }
    void candidateChangesAndEmptyTable() {
        MainWindow w;
        const auto d = water();
        w.loadDocument(d);
        w.showCoordinateTable();
        auto *panel = w.findChild<CoordinatesPanel *>();
        auto *t = table(w);
        // Read again with an atom appended: only that atom is new.
        panel->previewStructure(
            Document::fromXyz("4\nwith helium\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\nHe 3 0 0\n"));
        QVERIFY(w.hasCandidate());
        QVERIFY(!t->isColumnHidden(8));
        QCOMPARE(t->item(3, 8)->text(), QString("追加"));
        QVERIFY(t->item(0, 8)->text().isEmpty());
        QVERIFY(w.findChild<QLabel *>("candidateBannerText")->text().contains("追加 1"));
        QVERIFY(w.findChild<QLabel *>("candidateBannerText")->text().contains("削除 0"));
        QVERIFY(!w.findChild<QLabel *>("coordinateCompareNote")->text().isEmpty());
        QCOMPARE(button(w, "coordinateShowChanged")->text(), QString("変化あり 1"));
        button(w, "coordinateShowChanged")->click();
        QVERIFY(t->isRowHidden(0) && !t->isRowHidden(3));
        QVERIFY(w.findChild<QLabel *>("coordinateCount")->text().contains("候補 4 原子（今は 3）"));
        QVERIFY(!action(w, "copyNwchemGeometry")->isEnabled()); // Written out once confirmed.
        w.cancelCandidate();
        QVERIFY(t->isColumnHidden(8) && !t->isRowHidden(0));
        // A structure file dropped on the tab becomes a replacement candidate (other files go on to
        // the window, which opens them).
        QTemporaryDir dir;
        const auto path = dir.filePath("dropped.xyz");
        writeFile(path, "2\nH2\nH 0 0 0\nH 0 0 0.74\n");
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(panel, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(panel, &drop);
        QTRY_VERIFY(w.hasCandidate());
        QCOMPARE(w.displayDocument().atoms.size(), 2);
        w.cancelCandidate();
        // Without atoms the table gives way to what to do first.
        auto *empty = w.findChild<QWidget *>("coordinateEmpty");
        auto *pages = qobject_cast<QStackedWidget *>(empty->parentWidget());
        QVERIFY(pages->currentWidget() != empty);
        w.loadDocument(Document());
        QCOMPARE(pages->currentWidget(), empty);
        QVERIFY(button(w, "coordinateEmptyImport")->isEnabled());
        QVERIFY(w.findChild<QWidget *>("coordinateSelectionBar")->isHidden());
        QVERIFY(w.findChild<QLabel *>("coordinateEmptyNote")->text().contains("ドロップ"));
    }
    void importPreservesEnvelopeAndInvalidatesReferences() {
        MainWindow w;
        auto d = water();
        d.setJobValue("basis", "cc-pVDZ");
        d.data["nwchem"] = QJsonObject{{"geometry_units", "bohr"}, {"future", 42}};
        d.data["references"] = QJsonArray{QJsonObject{{"section", "optimize"},
                                                      {"key", "freeze"},
                                                      {"text", "2"},
                                                      {"ids", QJsonArray{d.atoms[1].id}},
                                                      {"span", QJsonArray{0, 1}}}};
        w.loadDocument(d);
        auto *panel = w.findChild<CoordinatesPanel *>();
        auto imported = Document::fromXyz("2\nH2\nH 0 0 0\nH 0 0 0.74\n");
        panel->previewStructure(imported);
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().toJson(), d.toJson());
        w.confirmCandidate();
        QCOMPARE(w.document().atoms.size(), 2);
        QCOMPARE(w.document().envelope, d.envelope);
        QCOMPARE(w.document().job()["basis"], QJsonValue("cc-pVDZ"));
        QVERIFY(w.document().data["references"].toArray().isEmpty());
        QVERIFY(!w.document().data["invalid_settings"].toArray().isEmpty());
        QCOMPARE(w.document().data["nwchem"], d.data["nwchem"]);
        auto roundtrip = Document::fromProject(w.document().project());
        QCOMPARE(roundtrip.data["nwchem"], d.data["nwchem"]);
        QCOMPARE(roundtrip.envelope["result_reference"], d.envelope["result_reference"]);
        w.history()->undo();
        QCOMPARE(w.document().toJson(), d.toJson());
        w.edit("fix", [](Document &d) { d.atoms[0].fixed = true; });
        QVERIFY_EXCEPTION_THROWN(panel->previewStructure(imported), std::runtime_error);
        QVERIFY(!w.hasCandidate());
        QCOMPARE(w.document().atoms.size(), 3);
    }
    void nwchemExportAndSettings() {
        auto d = water();
        d.atoms[1].isotope = 2;
        d.atoms[0].fixed = true;
        auto ghost = coordinateAtom("@C", {"3", "0", "0"}, "13", CoordinateUnit::Angstrom);
        ghost.charge = -1; // Formal charge must never become a nuclear charge override.
        d.atoms << ghost;
        const auto text = nwchemGeometry(d, CoordinateUnit::Angstrom);
        QVERIFY(text.contains("geometry units angstrom nocenter noautosym noautoz angstrom_to_au"));
        QVERIFY(text.contains("mass 2.014"));
        QVERIFY(text.contains("mass 13.003"));
        QVERIFY(text.contains("bqC 3 0 0"));
        QVERIFY(text.contains("bqC library C <basis-name>"));
        QVERIFY(text.contains("constraints\n  clear\n  fix atom 1\nend"));
        QVERIFY(!text.contains("charge -1"));
        std::reverse(d.atoms.begin(), d.atoms.end());
        QVERIFY(nwchemGeometry(d, CoordinateUnit::Bohr).contains("fix atom 4"));
        MainWindow w;
        w.loadDocument(d);
        // The unit of the input is a setting of "2 計算条件"; the export menu names it.
        auto *units = w.findChild<QComboBox *>("nwchem_geometry_units");
        auto *copy = action(w, "copyNwchemGeometry");
        QCOMPARE(copy->text(), QString("NWChem座標ブロックをコピー（Å）"));
        units->setCurrentIndex(units->findData("bohr"));
        QCOMPARE(w.document().data["nwchem"].toObject()["geometry_units"], QJsonValue("bohr"));
        QCOMPARE(copy->text(), QString("NWChem座標ブロックをコピー（Bohr）"));
        QCOMPARE(w.document().atoms[0].position, d.atoms[0].position);
        copy->trigger();
        const auto copied = QApplication::clipboard()->text();
        QCOMPARE(copied, nwchemGeometry(w.document(), CoordinateUnit::Bohr));
        QVERIFY(copied.contains("geometry units au"));
        w.history()->undo();
        QCOMPARE(copy->text(), QString("NWChem座標ブロックをコピー（Å）"));
        // Preserve a real input generated by the exporter for an optional engine smoke check.
        const QString artifacts = qEnvironmentVariable("MOLFORMA_COORDINATE_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QDir().mkpath(artifacts);
            w.showCoordinateTable();
            w.resize(1380, 900);
            w.show();
            QTest::qWait(40);
            button(w, "viewFit")->click();
            w.canvas()->setAngles(-20, 40, 0);
            QVERIFY(w.grab().save(artifacts + "/coordinates.png"));
            for (const auto &unit : {CoordinateUnit::Angstrom, CoordinateUnit::Bohr}) {
                QString name = unit == CoordinateUnit::Bohr ? "bohr" : "angstrom";
                QString input = "start " + name + "\n" + nwchemGeometry(d, unit) +
                                "\ncharge 0\nbasis\n * library sto-3g\n bqC library C sto-3g\nend\n"
                                "scf\n rhf\n singlet\nend\ntask scf energy\n";
                writeFile(artifacts + "/" + name + ".nw", input.toUtf8());
            }
        }
    }
};
QTEST_MAIN(CoordinatesTest)
#include "test_coordinates.moc"
