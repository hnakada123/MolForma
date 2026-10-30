// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/atom_properties.h"
#include "editing/peptide.h"
#include "model/geometry.h"
#include "nwchem/nwchem_geometry.h"
#include "ui/coordinates_panel.h"
#include "ui/main_window.h"
#include "ui/structure_import.h"
#include <QtTest>
#include <QtWidgets>
using namespace mf;
// Older Qt6 versions only accept integer positions for drag-enter events (as in McpGui).
using DragPosition =
    std::conditional_t<std::is_constructible_v<QDragEnterEvent, QPointF, Qt::DropActions, const QMimeData *,
                                               Qt::MouseButtons, Qt::KeyboardModifiers>,
                       QPointF, QPoint>;

class CoordinatesTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDir;
    static Document water() {
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        d.envelope["result_reference"] = QJsonObject{{"path", "existing-result.out"}};
        return d;
    }
    static QTableView *table(MainWindow &w) {
        return w.findChild<QTableView *>("coordinateTable");
    }
    // A cell of the coordinate table, read and edited through the table's model as the view does.
    struct Cell {
        QTableView *view;
        QModelIndex index;
        const Cell *operator->() const {
            return this;
        }
        QString text() const {
            return index.data().toString();
        }
        void setText(const QString &text) const {
            view->model()->setData(index, text);
        }
        Qt::CheckState checkState() const {
            return Qt::CheckState(index.data(Qt::CheckStateRole).toInt());
        }
        void setCheckState(Qt::CheckState state) const {
            view->model()->setData(index, state, Qt::CheckStateRole);
        }
        Qt::ItemFlags flags() const {
            return index.flags();
        }
        QVariant data(int role) const {
            return index.data(role);
        }
        bool isSelected() const {
            return view->selectionModel()->isSelected(index);
        }
    };
    static Cell item(QTableView *t, int row, int column) {
        return {t, t->model()->index(row, column)};
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
        qputenv("XDG_CONFIG_HOME", settingsDir.path().toUtf8());
        qputenv("XDG_DATA_HOME", settingsDir.filePath("data").toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settingsDir.path()); // MolForma uses NativeFormat.
    }
    void multiFrameImport_data() {
        QTest::addColumn<bool>("replace");
        QTest::addColumn<QString>("choice");
        for (bool replace : {false, true})
            for (const auto &choice : {"last", "first", "cancel", "stale"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(replace).arg(choice)))
                    << replace << QString(choice);
    }
    void sharedPdbImportChoices_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("text");
        QTest::newRow("pdb")
            << "models.pdb"
            << QByteArray("MODEL        2\n"
                          "ATOM      1  CA AALA A   1       0.000   0.000   0.000  0.60 20.00           C\n"
                          "ENDMDL\nMODEL        7\n"
                          "ATOM      1  CA BALA A   1       1.000   0.000   0.000  0.40 20.00           C\n"
                          "ENDMDL\n");
        QTest::newRow("mmcif")
            << "models.cif"
            << QByteArray("data_MODELS\nloop_\n_atom_site.group_PDB\n_atom_site.id\n"
                          "_atom_site.type_symbol\n_atom_site.label_atom_id\n_atom_site.label_alt_id\n"
                          "_atom_site.label_comp_id\n_atom_site.auth_asym_id\n_atom_site.auth_seq_id\n"
                          "_atom_site.Cartn_x\n_atom_site.Cartn_y\n_atom_site.Cartn_z\n"
                          "_atom_site.occupancy\n_atom_site.pdbx_PDB_model_num\n"
                          "ATOM 1 C CA A ALA A 1 0.000 0.000 0.000 0.60 2\n"
                          "ATOM 2 C CA B ALA A 1 1.000 0.000 0.000 0.40 7\n");
    }
    void sharedPdbImportChoices() {
        QFETCH(QString, name);
        QFETCH(QByteArray, text);
        QTemporaryDir dir;
        const auto path = dir.filePath(name);
        writeFile(path, text);
        QWidget parent;
        QTimer choose;
        int prompts = 0;
        connect(&choose, &QTimer::timeout, &parent, [&] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            if (dialog->windowTitle() == "PDB MODEL")
                dialog->findChild<QComboBox *>()->setCurrentIndex(1);
            else if (dialog->windowTitle() == "PDB ALTLOC")
                dialog->setTextValue("B");
            ++prompts;
            dialog->accept();
        });
        choose.start(0);
        const auto options = chooseStructureImport(&parent, path);
        choose.stop();
        QVERIFY(options);
        QCOMPARE(prompts, 2);
        QCOMPARE(options->model, 7);
        QCOMPARE(options->altloc, QString("B"));
        QTimer::singleShot(0, &parent, [] {
            if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        QVERIFY(!chooseStructureImport(&parent, path));
    }
    void multiFrameImport() {
        QFETCH(bool, replace);
        QFETCH(QString, choice);
        QTemporaryDir dir;
        const auto path = dir.filePath("trajectory.xyz");
        writeFile(path, "1\nfirst\nHe 0 0 0\n1\nlast\nNe 1 0 0\n");
        MainWindow w;
        w.loadDocument(water());
        const auto before = w.document();
        bool prompted = false;
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            prompted = true;
            if (choice == "cancel") {
                dialog->reject();
                return;
            }
            if (choice == "first")
                dialog->findChild<QComboBox *>()->setCurrentIndex(0);
            if (choice == "stale")
                w.edit("intervening edit", [](Document &d) { d.charge = 1; });
            dialog->accept();
        });
        const auto load = [&] {
            if (replace)
                w.findChild<CoordinatesPanel *>()->importPath(path);
            else
                w.loadPath(path);
        };
        if (choice == "stale") {
            QVERIFY_EXCEPTION_THROWN(load(), std::runtime_error);
            QCOMPARE(w.document().charge, 1);
            QCOMPARE(w.document().ids(), before.ids());
            QVERIFY(!w.hasCandidate());
        } else {
            load();
            if (choice == "cancel") {
                QCOMPARE(w.document().project(), before.project());
                QVERIFY(!w.hasCandidate());
                QCOMPARE(w.history()->count(), 0);
            } else if (replace) {
                QVERIFY(w.hasCandidate());
                QCOMPARE(w.document().project(), before.project());
                QCOMPARE(w.displayDocument().formula(), choice == "first" ? QString("He") : QString("Ne"));
                QCOMPARE(w.displayDocument().envelope, before.envelope);
                QCOMPARE(w.displayDocument().charge, before.charge);
                QCOMPARE(w.displayDocument().multiplicity, before.multiplicity);
                w.confirmCandidate();
                w.history()->undo();
                QCOMPARE(w.document().project(), before.project());
            } else
                QCOMPARE(w.document().formula(), choice == "first" ? QString("He") : QString("Ne"));
        }
        QVERIFY(prompted);
    }
    void atomPropertiesAreAtomicAcrossSelection() {
        auto d = Document::fromXyz("2\nmixed\nH 0 0 0\nC 1.5 0 0\n");
        const auto before = d.project();
        AtomProperties properties;
        properties.isotope = 2; // Valid for H, invalid for C.
        properties.charge = 1;
        QVERIFY_EXCEPTION_THROWN(setAtomProperties(d, d.ids(), properties), std::runtime_error);
        QCOMPARE(d.project(), before);
        QVERIFY_EXCEPTION_THROWN(
            setCoordinateCell(d, d.atoms[1].id, CoordinateColumn::Mass, "400", CoordinateUnit::Angstrom),
            std::runtime_error);
        QCOMPARE(d.project(), before);
        properties = atomMassInput("13.0");
        setAtomProperties(d, {d.atoms[1].id}, properties);
        QCOMPARE(d.atoms[1].mass, 13.0);
        QCOMPARE(d.atoms[1].isotope, 0);
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
    void refreshUpdatesCellsInPlace() {
        // A refresh updates the cells of the rows there are; only another number of atoms resets them.
        MainWindow w;
        w.loadDocument(water());
        w.showCoordinateTable();
        auto *t = table(w);
        QSignalSpy resets(t->model(), &QAbstractItemModel::modelReset);
        QSignalSpy cells(t->model(), &QAbstractItemModel::dataChanged);
        item(t, 1, 1)->setText("1.5");
        QCOMPARE(w.document().atoms[1].position.x(), 1.5);
        QCOMPARE(item(t, 1, 1)->text(), QString("1.5"));
        QCOMPARE(resets.count(), 0);
        QVERIFY(cells.count() > 0);
        w.edit("add", [](Document &d) {
            d.atoms << Atom{};
            d.atoms.last().position = Vec(3, 3, 3);
        });
        QCOMPARE(t->model()->rowCount(), 4);
        QCOMPARE(resets.count(), 1);
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
        QCOMPARE(t->model()->rowCount(), 3);
        auto *units = w.findChild<QComboBox *>("coordinateUnits");
        for (int i = 0; i < 20; ++i)
            units->setCurrentIndex(i % 2);
        QCOMPARE(w.document().toJson(), d.toJson());
        QCOMPARE(w.history()->count(), 0);
        QVERIFY(std::abs(item(t, 0, 1)->text().toDouble() - d.atoms[0].position.x() / BohrToAngstrom) <
                1e-13);
        // Shown with six decimals; the cell (and its editor) keeps the full value.
        auto *delegate = qobject_cast<QStyledItemDelegate *>(t->itemDelegateForColumn(1));
        QVERIFY(delegate);
        QCOMPARE(delegate->displayText(item(t, 0, 1)->text(), QLocale()),
                 QString::number(d.atoms[0].position.x() / BohrToAngstrom, 'f', 6));
        item(t, 0, 1)->setText("2");
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
        QVERIFY(!(item(t, 0, 1)->flags() & Qt::ItemIsEditable));
        item(t, 0, 1)->setText("1"); // The model refuses the cell.
        QCOMPARE(w.document().toJson(), d.toJson());
        QCOMPARE(item(t, 0, 1)->text(), QString("0"));
        for (const auto &text : {"nan", "inf", "abc", "1e999"}) {
            item(t, 1, 1)->setText(text);
            QCOMPARE(w.document().toJson(), d.toJson());
            QVERIFY(!w.findChild<QLabel *>("coordinateMessage")->text().isEmpty());
        }
        item(t, 1, 0)->setText("Imaginary");
        QCOMPARE(w.document().toJson(), d.toJson());
        item(t, 1, 4)->setText("-3");
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
        item(t, 0, 6)->setCheckState(Qt::Unchecked);
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
        t->setCurrentIndex(t->model()->index(1, 1));
        t->edit(t->model()->index(1, 1));
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
        QVERIFY(item(t, 2, 0)->isSelected());
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
        // The split is sized after the table is shown (queued): until then the table has no room.
        QTRY_VERIFY(split->sizes().value(1) > 0 && split->sizes().value(0) > split->sizes().value(1));
        QVERIFY(w.findChild<QWidget *>("coordinatesTab")->minimumSizeHint().height() < 300);
        QVERIFY(table(w)->viewport()->height() >= 4 * table(w)->verticalHeader()->defaultSectionSize());
        button(w, "moleculeLayout_view")->click();
    }
    void isotopesGhostAndOverlap() {
        MainWindow w;
        auto d = water();
        w.loadDocument(d);
        auto *t = table(w);
        item(t, 1, 0)->setText("D");
        QCOMPARE(w.document().atoms[1].isotope, 2);
        QCOMPARE(w.document().atoms[1].element, QString("H"));
        item(t, 1, 0)->setText("C");
        QCOMPARE(w.document().atoms[1].isotope, 0);
        item(t, 1, 4)->setText("13");
        item(t, 1, 5)->setCheckState(Qt::Checked);
        QVERIFY(w.document().atoms[1].ghost);
        action(w, "copyNwchemGeometry")->trigger();
        QVERIFY(QApplication::clipboard()->text().contains("bqC"));
        QVERIFY(QApplication::clipboard()->text().contains("mass 13.003"));
        item(t, 1, 5)->setCheckState(Qt::Unchecked);
        QVERIFY(!w.document().atoms[1].ghost);
        auto previous = w.document().toJson();
        int count = w.history()->count();
        item(t, 1, 1)->setText("-0.758");
        QCOMPARE(w.document().toJson(), previous);
        QCOMPARE(w.history()->count(), count);
        QCOMPARE(item(t, 1, 1)->text().toDouble(), 0.758);
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
        QVERIFY(item(t, 2, 0)->isSelected());
        QVERIFY(item(t, 0, 0)->isSelected());
        QVERIFY(!item(t, 1, 0)->isSelected());
        // Deleting from the pages at the right is a candidate to confirm, shown in the table.
        w.setSelection({d.atoms[1].id});
        button(w, "deleteSelection")->click();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().atoms.size(), 3);
        QCOMPARE(t->model()->rowCount(), 2);
        QVERIFY(!action(w, "copyNwchemGeometry")->isEnabled());
        QVERIFY(!action(w, "coordinateImport")->isEnabled());
        QVERIFY(!button(w, "coordinateAdd")->isEnabled());
        QVERIFY(!(item(t, 0, 0)->flags() & Qt::ItemIsEditable));
        button(w, "editorUndo")->click(); // Cancels the candidate.
        QVERIFY(!w.hasCandidate());
        QCOMPARE(t->model()->rowCount(), 3);
        w.setSelection({d.atoms[1].id});
        button(w, "deleteSelection")->click();
        w.confirmCandidate();
        QCOMPARE(w.document().atoms.size(), 2);
        w.history()->undo();
        QCOMPARE(w.document().toJson(), d.toJson());
        w.edit("reorder", [](Document &d) { std::reverse(d.atoms.begin(), d.atoms.end()); });
        item(t, 0, 3)->setText("2.5");
        QCOMPARE(w.document().atom(d.atoms[2].id).position.z(), 2.5);
        QCOMPARE(w.document().atom(d.atoms[0].id).position.z(), 0.0);
    }
    void selectionScrollsIntoTable() {
        // Atoms chosen in the 3D view bring their rows into view when none of them is shown.
        MainWindow w;
        Document d;
        for (int i = 0; i < 40; ++i)
            placeTemplate(d, findTemplate("skeleton:water"), Vec(4.0 * i, 0, 0));
        w.loadDocument(d);
        w.resize(1380, 900);
        w.show();
        button(w, "moleculeLayout_both")->click();
        QTest::qWait(30);
        auto *t = table(w);
        const auto shown = [&](int row) {
            return t->visualRect(t->model()->index(row, 0)).intersects(t->viewport()->rect());
        };
        QVERIFY(shown(0) && !shown(110));
        w.setSelection({w.document().atoms[110].id, w.document().atoms[111].id});
        QVERIFY(shown(110));
        const int scroll = t->verticalScrollBar()->value();
        w.setSelection({w.document().atoms[111].id}); // Already shown: the table stays.
        QCOMPARE(t->verticalScrollBar()->value(), scroll);
    }
    void selectionShownWithTable() {
        // Chosen before the table is first shown: the rows are in view once it has its size.
        MainWindow w;
        Document d;
        for (int i = 0; i < 4; ++i)
            placeTemplate(d, findTemplate("skeleton:water"), Vec(4.0 * i, 0, 0));
        w.loadDocument(d);
        w.resize(1380, 900);
        w.show();
        button(w, "moleculeLayout_view")->click();
        QTest::qWait(30);
        w.setSelection({w.document().atoms[11].id});
        button(w, "moleculeLayout_both")->click();
        QTest::qWait(30);
        auto *t = table(w);
        QVERIFY(t->viewport()->height() < 12 * t->rowHeight(0)); // The rows do not all fit.
        QVERIFY(t->visualRect(t->model()->index(11, 0)).intersects(t->viewport()->rect()));
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
        QCOMPARE(table(w)->model()->rowCount(), 0);
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
        Residue water;
        water.code = "HOH";
        water.number = 1;
        water.atoms = {{"O", d.atoms[0].id}, {"HA", d.atoms[1].id}, {"HB", d.atoms[2].id}};
        d.residues = {water};
        w.loadDocument(d);
        w.showCoordinateTable();
        auto *t = table(w);
        // Cells keep their model columns; the residue is shown beside the atom.
        auto *header = t->horizontalHeader();
        QCOMPARE(header->visualIndex(0), 1);
        QCOMPARE(header->visualIndex(7), 2);
        QCOMPARE(item(t, 1, 7)->text(), QString("HOH 1 · HA"));
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
        QVERIFY(item(t, 0, 0)->data(Qt::UserRole + 1).toBool()); // The lock in the atom cell.
        QVERIFY(!(item(t, 0, 1)->flags() & Qt::ItemIsEditable));
        button(w, "ghostSelectedAtoms")->click();
        QVERIFY(w.document().atoms[0].ghost && w.document().atoms[1].ghost);
        QCOMPARE(item(t, 0, 5)->checkState(), Qt::Checked);
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
        QCOMPARE(t->model()->headerData(9, Qt::Horizontal).toString(), QString("形式電荷"));
        QCOMPARE(item(t, 0, 9)->text(), QString("0"));
        item(t, 0, 9)->setText("-1");
        QCOMPARE(w.document().atoms[0].charge, -1);
        item(t, 0, 10)->setText("1");
        QCOMPARE(w.document().atoms[0].radical, 1);
        item(t, 1, 11)->setText(" HW1 ");
        QCOMPARE(w.document().atoms[1].name, QString("HW1"));
        QCOMPARE(w.history()->count(), 3);
        // Invalid values are refused with a message and leave the document as it was.
        item(t, 0, 10)->setText("9");
        QCOMPARE(w.document().atoms[0].radical, 1);
        QVERIFY(!w.findChild<QLabel *>("coordinateMessage")->isHidden());
        QCOMPARE(item(t, 0, 10)->text(), QString("1"));
        item(t, 0, 9)->setText("x");
        QCOMPARE(w.document().atoms[0].charge, -1);
        QCOMPARE(w.history()->count(), 3);
        w.history()->undo();
        QCOMPARE(w.document().atoms[1].name, d.atoms[1].name);
        QCOMPARE(item(t, 1, 11)->text(), d.atoms[1].name);
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
        QCOMPARE(item(t, 3, 8)->text(), QString("追加"));
        QVERIFY(item(t, 0, 8)->text().isEmpty());
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
        QDragEnterEvent enter(DragPosition(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
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
    void importPreservesEnvelopeAndSettings() {
        MainWindow w;
        auto d = water();
        d.charge = 1;
        d.multiplicity = 2;
        d.data["nwchem"] = QJsonObject{{"geometry_units", "bohr"}, {"future", 42}};
        w.loadDocument(d);
        auto *panel = w.findChild<CoordinatesPanel *>();
        auto imported = Document::fromXyz("2\nH2\nH 0 0 0\nH 0 0 0.74\n");
        panel->previewStructure(imported);
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().toJson(), d.toJson());
        w.confirmCandidate();
        QCOMPARE(w.document().atoms.size(), 2);
        QCOMPARE(w.document().envelope, d.envelope);
        QCOMPARE(w.document().charge, 1);
        QCOMPARE(w.document().multiplicity, 2);
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
