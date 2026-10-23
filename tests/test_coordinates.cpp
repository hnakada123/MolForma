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
    static QPushButton *button(MainWindow &w, const char *name) {
        return w.findChild<QPushButton *>(name);
    }
  private slots:
    void initTestCase() {
        QVERIFY(settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
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
        QCOMPARE(tabs->count(), 8);
        QCOMPARE(tabs->tabText(1), QString("2 座標"));
        tabs->setCurrentIndex(1);
        auto *t = table(w);
        QCOMPARE(t->rowCount(), 3);
        auto *units = w.findChild<QComboBox *>("coordinateUnits");
        for (int i = 0; i < 20; ++i)
            units->setCurrentIndex(i % 2);
        QCOMPARE(w.document().toJson(), d.toJson());
        QCOMPARE(w.history()->count(), 0);
        QVERIFY(std::abs(t->item(0, 1)->text().toDouble() - d.atoms[0].position.x() / BohrToAngstrom) <
                1e-13);
        t->item(0, 1)->setText("2");
        QCOMPARE(w.document().atoms[0].position.x(), 2 * BohrToAngstrom);
        QCOMPARE(w.document().atoms[0].position.y(), d.atoms[0].position.y());
        QCOMPARE(w.document().atoms[0].position.z(), d.atoms[0].position.z());
        QCOMPARE(w.document().atoms[0].id, d.atoms[0].id);
        QCOMPARE(w.document().bonds.size(), d.bonds.size());
        QCOMPARE(w.document().envelope, d.envelope);
        QCOMPARE(w.history()->count(), 1);
        button(w, "coordinateUndo")->click();
        QCOMPARE(w.document().toJson(), d.toJson());
        button(w, "coordinateRedo")->click();
        QCOMPARE(w.document().atoms[0].position.x(), 2 * BohrToAngstrom);
        button(w, "coordinateCopyXyz")->click();
        auto copied = Document::fromXyz(QApplication::clipboard()->text());
        QVERIFY(std::abs(copied.atoms[0].position.x() - 2 * BohrToAngstrom) < 1e-9);
        // View operations never change physical geometry or add history.
        auto before = w.document().toJson();
        button(w, "coordinateView0")->click();
        button(w, "coordinateFit")->click();
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
        w.setSelection({d.atoms[1].id});
        button(w, "coordinateOrigin")->click();
        QCOMPARE(w.document().toJson(), d.toJson());
        button(w, "coordinateClear")->click();
        QVERIFY(!w.hasCandidate());
        t->item(0, 6)->setCheckState(Qt::Unchecked);
        QVERIFY(!w.document().atoms[0].fixed);
        // Moving H1 to the origin translates the entire molecule rigidly.
        w.setSelection({d.atoms[1].id});
        const double distance = measure(w.document(), {d.atoms[0].id, d.atoms[1].id});
        button(w, "coordinateOrigin")->click();
        QVERIFY(w.document().atoms[1].position.isZero(1e-14));
        QCOMPARE(measure(w.document(), {d.atoms[0].id, d.atoms[1].id}), distance);
        w.history()->undo();
        QCOMPARE(w.document().atoms[1].position, d.atoms[1].position);
    }
    void keyboardEditingAndMoleculeSelection() {
        MainWindow w;
        const auto d = water();
        w.loadDocument(d);
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(1);
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
        auto *v = w.findChild<MoleculeView *>("coordinateView");
        w.setSelection({});
        v->fit();
        QTest::mouseClick(v, Qt::LeftButton, Qt::NoModifier, v->atomScreenPosition(d.atoms[2].id).toPoint());
        QCOMPARE(w.canvas()->selection(), Ids{d.atoms[2].id});
        QVERIFY(t->item(2, 0)->isSelected());
        // Switching between tabs and views cannot commit rounded display values.
        auto before = w.document().toJson();
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(0);
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(1);
        QCOMPARE(w.document().toJson(), before);
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
        auto *preview = w.findChild<QPlainTextEdit *>("nwchemGeometryPreview");
        QVERIFY(preview->toPlainText().contains("bqC"));
        QVERIFY(preview->toPlainText().contains("mass 13.003"));
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
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(1);
        w.show();
        QTest::qWait(30);
        auto *t = table(w);
        t->selectRow(1);
        QCOMPARE(w.canvas()->selection(), Ids{d.atoms[1].id});
        w.setSelection({d.atoms[2].id, d.atoms[0].id});
        QVERIFY(t->item(2, 0)->isSelected());
        QVERIFY(t->item(0, 0)->isSelected());
        QVERIFY(!t->item(1, 0)->isSelected());
        auto *view = w.findChild<MoleculeView *>("coordinateView");
        QCOMPARE(view->selection(), w.canvas()->selection());
        w.setSelection({d.atoms[1].id});
        button(w, "coordinateRemove")->click();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().atoms.size(), 3);
        QCOMPARE(t->rowCount(), 2);
        QVERIFY(!button(w, "copyNwchemGeometry")->isEnabled());
        QVERIFY(!button(w, "coordinateAdd")->isEnabled());
        QVERIFY(!(t->item(0, 0)->flags() & Qt::ItemIsEditable));
        button(w, "coordinateUndo")->click();
        QVERIFY(!w.hasCandidate());
        QCOMPARE(t->rowCount(), 3);
        w.setSelection({d.atoms[1].id});
        button(w, "coordinateRemove")->click();
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
            symbol->setCurrentText("He");
            for (const auto *name : {"coordinateAtomX", "coordinateAtomY", "coordinateAtomZ"})
                dialog->findChild<QLineEdit *>(name)->setText("0");
            // Exact overlap with oxygen must be rejected.
            QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
            dialog->findChild<QLineEdit *>("coordinateAtomX")->setText("4");
            QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
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
        button(w, "coordinateClear")->click();
        QVERIFY(w.hasCandidate());
        QCOMPARE(table(w)->rowCount(), 0);
        w.confirmCandidate();
        QVERIFY(w.document().atoms.isEmpty());
        QVERIFY(!button(w, "copyNwchemGeometry")->isEnabled());
        w.history()->undo();
        QCOMPARE(w.document().atoms.size(), 3);
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
        auto *units = w.findChild<QComboBox *>("nwchemUnits");
        units->setCurrentIndex(1);
        QCOMPARE(w.document().data["nwchem"].toObject()["geometry_units"], QJsonValue("bohr"));
        QCOMPARE(w.document().atoms[0].position, d.atoms[0].position);
        button(w, "copyNwchemGeometry")->click();
        const auto copied = QApplication::clipboard()->text();
        QCOMPARE(copied, nwchemGeometry(w.document(), CoordinateUnit::Bohr));
        QVERIFY(copied.contains("geometry units au"));
        w.history()->undo();
        QCOMPARE(units->currentIndex(), 0);
        // Preserve a real input generated by the exporter for an optional engine smoke check.
        const QString artifacts = qEnvironmentVariable("MOLFORMA_COORDINATE_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QDir().mkpath(artifacts);
            w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(1);
            w.resize(1380, 900);
            w.show();
            QTest::qWait(40);
            button(w, "coordinateFit")->click();
            w.findChild<MoleculeView *>("coordinateView")->setAngles(-20, 40, 0);
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
