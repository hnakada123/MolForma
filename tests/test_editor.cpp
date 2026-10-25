#include "editing/coordination.h"
#include "editing/library.h"
#include "editing/operations.h"
#include "model/geometry.h"
#include "nwchem/nwchem.h"
#include "ui/main_window.h"
#include "ui/periodic_table.h"
#include "ui/template_delegate.h"
#include "ui/translations.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <QtTest>
using namespace mf;
class InspectableTemplateDelegate : public TemplateDelegate {
  public:
    using TemplateDelegate::initStyleOption;
};
class EditorTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsHome;
  private slots:
    void initTestCase() {
        // Autosave copies go to the data directory: keep them out of the user's home.
        qputenv("XDG_DATA_HOME", settingsHome.filePath("data").toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsHome.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settingsHome.path()); // MolForma uses NativeFormat.
    }
    void recentFilesDropsAndManual() {
        QTemporaryDir dir;
        const auto xyz = dir.filePath("water.xyz");
        writeFile(xyz, "3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        MainWindow w;
        w.loadPath(xyz);
        QCOMPARE(w.recentFiles().first(), QFileInfo(xyz).absoluteFilePath());
        auto *recent = w.findChild<QMenu *>("recentMenu");
        QVERIFY(recent && recent->actions().first()->text().contains("water.xyz"));
        // Dropping a structure file opens it; result files go to the result viewer.
        const auto other = dir.filePath("neon.xyz");
        writeFile(other, "1\nneon\nNe 0 0 0\n");
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(other)});
        QDragEnterEvent enter(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &drop);
        QTRY_COMPARE(w.document().formula(), QString("Ne"));
        QCOMPARE(w.recentFiles().size(), 2);
        // The manual is embedded in the program.
        QString text;
        QTimer::singleShot(0, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("manualDialog")) {
                text = dialog->findChild<QTextBrowser *>("manualText")->toPlainText();
                dialog->reject();
            }
        });
        w.findChild<QAction *>("manualAction")->trigger();
        QVERIFY(text.contains("MolForma") && text.size() > 1000);
    }
    void autosaveAndRecovery() {
        QDir(settingsHome.filePath("data")).removeRecursively();
        {
            MainWindow w;
            Document d;
            placeTemplate(d, findTemplate("skeleton:benzene"), Vec::Zero());
            w.loadDocument(d);
            w.startAutosave();
            w.edit("削除", [&](Document &next) { next.remove({next.atoms.last().id}); });
            w.autosave();
            const QDir autosaves(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                                 "/autosave");
            QVERIFY(autosaves.absolutePath().startsWith(settingsHome.path()));
            const auto copies = autosaves.entryList({"session-*.kgui.json"});
            QCOMPARE(copies.size(), 1);
            // Simulate a crash: keep the copy and leave a lock file of a process that no longer runs.
            QFile::copy(autosaves.filePath(copies.first()), autosaves.filePath("session-1-2.kgui.json"));
            writeFile(autosaves.filePath("session-1-2.lock"),
                      QByteArray("999999\nmolforma\n") + QSysInfo::machineHostName().toUtf8() + "\n");
            // Undo and a different edit return to the same undo index: the copy still follows.
            w.history()->undo();
            w.edit("削除", [&](Document &next) { next.remove({next.atoms.first().id}); }, false);
            w.autosave();
            const auto saved = Document::fromProject(parseJson(readFile(autosaves.filePath(copies.first()))));
            QCOMPARE(saved.formula(), w.document().formula());
            w.history()->undo(); // Clean again: the copy of this session is removed.
            w.autosave();
            QVERIFY(!autosaves.exists(copies.first()));
        }
        const auto answer = [](const QString &label) {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                for (auto *b : box->buttons())
                    if (b->text() == label)
                        b->click();
        };
        {
            // "後で決める" keeps the copy, so the next start asks again.
            MainWindow later;
            QTimer::singleShot(0, &later, [&] { answer("後で決める"); });
            later.offerRecovery();
            QCOMPARE(later.document().atoms.size(), 3); // The startup water.
        }
        const QDir autosaves(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                             "/autosave");
        QVERIFY(autosaves.exists("session-1-2.kgui.json"));
        MainWindow next;
        QTimer::singleShot(0, &next, [&] { answer("復旧する"); });
        next.offerRecovery();
        QVERIFY(!autosaves.exists("session-1-2.kgui.json"));
        QCOMPARE(next.document().atoms.size(), 11); // Benzene without one hydrogen.
        QVERIFY(!next.history()->isClean());
    }
    void symmetryAndZMatrix() {
        MainWindow w;
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        d.atoms[1].position += (d.atoms[1].position - d.atoms[0].position).normalized() * .02;
        w.loadDocument(d);
        QString found;
        QTimer::singleShot(0, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("symmetryDialog")) {
                found = dialog->findChild<QLabel *>("symmetryResult")->text();
                dialog->findChild<QPushButton *>("symmetryApply")->click();
            }
        });
        w.findChild<QAction *>("symmetryAction")->trigger();
        QVERIFY(found.contains("C2v"));
        QVERIFY(w.hasCandidate());
        w.confirmCandidate();
        const auto &sym = w.document();
        QVERIFY(std::abs(measure(sym, {sym.atoms[0].id, sym.atoms[1].id}) -
                         measure(sym, {sym.atoms[0].id, sym.atoms[2].id})) < 1e-9);
        // Z-matrix: editing a distance moves that atom.
        QTimer::singleShot(0, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("zMatrixDialog")) {
                auto *table = dialog->findChild<QTableWidget *>("zMatrixTable");
                table->item(1, 2)->setText("1.2");
                dialog->reject();
            }
        });
        w.findChild<QAction *>("zMatrixAction")->trigger();
        const auto &edited = w.document();
        QVERIFY(std::abs(measure(edited, {edited.atoms[0].id, edited.atoms[1].id}) - 1.2) < 1e-9);
    }
    void viewStylesMonitorsAndHydrogenBonds() {
        MainWindow w;
        w.loadDocument(Document::fromXyz("6\nwater dimer\nO -1.551007 -0.114520 0\nH -1.934259 0.762503 0\n"
                                         "H -0.599677 0.040712 0\nO 1.350625 0.111469 0\n"
                                         "H 1.680398 -0.373741 -0.758561\nH 1.680398 -0.373741 0.758561\n"));
        const auto ids = w.document().ids();
        auto *view = w.canvas();
        w.findChild<QAction *>("viewStyleSpacefill")->trigger();
        QCOMPARE(view->style(), QString("spacefill"));
        auto *hbonds = w.findChild<QAction *>("viewHydrogenBonds");
        hbonds->setChecked(true);
        QCOMPARE(view->hydrogenBonds().size(), 1);
        QCOMPARE(view->hydrogenBonds().first(), std::make_pair(ids[2], ids[3]));
        w.setSelection({ids[0], ids[3]});
        w.findChild<QAction *>("viewAddMonitor")->trigger();
        QCOMPARE(view->monitorList().size(), 1);
        w.setSelection({ids[3], ids[4], ids[5]});
        w.findChild<QAction *>("viewHideSelection")->trigger();
        QCOMPARE(view->hiddenAtoms().size(), 3);
        w.findChild<QAction *>("viewShowAll")->trigger();
        QVERIFY(view->hiddenAtoms().isEmpty());
        // Monitors and hidden atoms follow deleted atoms.
        w.edit("削除", [&](Document &d) { d.remove({ids[3], ids[4], ids[5]}); });
        QVERIFY(view->monitorList().isEmpty());
        hbonds->setChecked(false);
        w.findChild<QAction *>("viewStyleBallAndStick")->trigger();
        // A monitor whose atoms became coincident or collinear still paints (as "—").
        auto line = Document::fromXyz("4\nline\nC 0 0 0\nC 1.5 0 0\nC 3 0 0\nC 4.5 0 0\n");
        view->setDocument(line);
        view->setMonitors({line.ids()});
        QVERIFY(!view->grab().isNull());
    }
    void selectionDeletionAndCandidateBanner() {
        MainWindow w;
        w.resize(1300, 900);
        w.show();
        Document d;
        placeTemplate(d, findTemplate("skeleton:ethane"), Vec::Zero());
        w.loadDocument(d);
        QTest::qWait(40);
        const auto ids = w.document().ids();
        Ids carbons;
        for (const auto &a : w.document().atoms)
            if (a.element == "C")
                carbons << a.id;
        auto *view = w.canvas();
        // A click selects only that atom; Shift+click adds in order.
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                          view->atomScreenPosition(carbons[0]).toPoint());
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier,
                          view->atomScreenPosition(carbons[1]).toPoint());
        QCOMPARE(view->selection(), Ids{carbons[1]});
        QTest::mouseClick(view, Qt::LeftButton, Qt::ShiftModifier,
                          view->atomScreenPosition(carbons[0]).toPoint());
        QCOMPARE(view->selection(), (Ids{carbons[1], carbons[0]}));
        // Deleting a carbon takes its own hydrogens and caps the other carbon: ethane → methane.
        w.setSelection({carbons[0]});
        w.findChild<QAction *>("deleteAction")->trigger();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.displayDocument().formula(), QString("CH4"));
        auto *text = w.findChild<QLabel *>("candidateBannerText");
        QVERIFY(text->text().contains("追加 1・削除 4"));
        QVERIFY(w.findChild<QWidget *>("candidateBanner")->isVisible());
        QVERIFY(!w.findChild<QWidget *>("candidateBar")->isVisible());
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        tabs->setCurrentIndex(1);
        QVERIFY(w.findChild<QWidget *>("candidateBar")->isVisible());
        tabs->setCurrentIndex(0);
        w.findChild<QAction *>("escapeAction")->trigger();
        QVERIFY(!w.hasCandidate());
        // Without capping, only the carbon and its hydrogens go.
        w.findChild<QCheckBox *>("deleteTerminate")->setChecked(false);
        w.setSelection({carbons[0]});
        w.findChild<QAction *>("deleteAction")->trigger();
        QCOMPARE(w.displayDocument().formula(), QString("CH3"));
        w.findChild<QAction *>("confirmAction")->trigger();
        QVERIFY(!w.hasCandidate());
        QCOMPARE(w.document().formula(), QString("CH3"));
        QVERIFY(w.findChild<QAction *>("undoAction")->text().contains("選択原子を削除"));
        w.history()->undo();
        w.findChild<QCheckBox *>("deleteTerminate")->setChecked(true);
        // Select all, copy as XYZ, paste as a fragment beside the molecule.
        w.findChild<QAction *>("selectAllAction")->trigger();
        QCOMPARE(view->selection().size(), ids.size());
        w.findChild<QAction *>("copyAction")->trigger();
        QVERIFY(QApplication::clipboard()->text().startsWith("8\n"));
        w.findChild<QAction *>("pasteAction")->trigger();
        QCOMPARE(w.displayDocument().atoms.size(), 16);
        w.cancelCandidate();
        // Labels and tab shortcuts.
        auto *labels = w.findChild<QComboBox *>("labelMode");
        labels->setCurrentIndex(labels->findData("heavy"));
        QCOMPARE(view->labels(), QString("heavy"));
        w.findChild<QAction *>("showTab3Action")->trigger();
        QCOMPARE(tabs->currentIndex(), 2);
        QString listed;
        QTimer::singleShot(0, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("shortcutsDialog")) {
                auto *table = dialog->findChild<QTableWidget *>("shortcutsTable");
                for (int r = 0; r < table->rowCount(); ++r)
                    listed += table->item(r, 1)->text() + "\n";
                dialog->reject();
            }
        });
        w.findChild<QAction *>("shortcutsAction")->trigger();
        QVERIFY(listed.contains("選択原子を削除") && listed.contains("変更を確定") &&
                listed.contains("すべて選択"));
    }
    void hiddenTabsRefreshWhenOpened() {
        MainWindow w;
        QCOMPARE(w.history()->undoLimit(), 100);
        w.loadDocument(Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n"));
        w.show();
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        auto *table = w.findChild<QTableWidget *>("coordinateTable");
        auto *coordinates = tabs->widget(1);
        QCOMPARE(table->item(0, 1)->text().toDouble(), 0.0);
        // While "1 分子" is open the coordinate table waits; opening its tab brings it up to date.
        w.edit("move", [](Document &d) { d.atoms[0].position.x() = 0.25; });
        QCOMPARE(table->item(0, 1)->text().toDouble(), 0.0);
        tabs->setCurrentWidget(coordinates);
        QCOMPARE(table->item(0, 1)->text().toDouble(), 0.25);
        // The open tab follows undo at once, once the Undo/Redo texts have moved too.
        w.history()->undo();
        QCOMPARE(table->item(0, 1)->text().toDouble(), 0.0);
        QVERIFY(w.findChild<QPushButton *>("coordinateRedo")->isEnabled());
        tabs->setCurrentIndex(0);
        w.history()->redo();
        QCOMPARE(table->item(0, 1)->text().toDouble(), 0.0);
        w.refreshStalePanels(); // What McpGui does before it reads or drives the GUI.
        QCOMPARE(table->item(0, 1)->text().toDouble(), 0.25);
        // The input preview tab still shows when the input cannot be generated.
        const int preview = 5;
        QCOMPARE(tabs->tabText(preview), QString("6 入力プレビュー"));
        w.edit("multiplicity", [](Document &d) { d.setJobValue("multiplicity", 2); });
        QVERIFY(tabs->tabBar()->tabTextColor(preview).isValid());
        QVERIFY(tabs->tabToolTip(preview).contains("エラー"));
        w.history()->undo();
        QVERIFY(!tabs->tabBar()->tabTextColor(preview).isValid());
        // Selecting every atom selects every table row, keeping the click order.
        tabs->setCurrentWidget(coordinates);
        const auto ids = w.document().ids();
        w.setSelection({ids[2], ids[0], ids[1]});
        QCOMPARE(table->selectionModel()->selectedRows().size(), 3);
        table->selectionModel()->select(table->model()->index(1, 0),
                                        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QCOMPARE(w.canvas()->selection(), Ids{ids[1]});
    }
    void rotationAndMirrorAboutSelection() {
        MainWindow w;
        // A molecule away from the origin turns and reflects in place by default.
        w.loadDocument(Document::fromXyz("3\nwater\nO 5 0 0\nH 5.758 0 0.504\nH 4.242 0 0.504\n"));
        const auto ids = w.document().ids();
        const Vec before = centroid(w.document(), ids);
        w.setSelection(ids);
        w.findChild<QDoubleSpinBox *>("rotationAngle")->setValue(90);
        w.findChild<QPushButton *>("rotateSelection")->click();
        QVERIFY((centroid(w.document(), ids) - before).norm() < 1e-9);
        QVERIFY(std::abs(w.document().atom(ids[1]).position.y() - .758) < 1e-9);
        auto *plane = w.findChild<QComboBox *>("mirrorPlane");
        plane->setCurrentIndex(plane->findData("1")); // XZ: y → −y about the center
        w.findChild<QPushButton *>("mirrorSelection")->click();
        QVERIFY((centroid(w.document(), ids) - before).norm() < 1e-9);
        QVERIFY(std::abs(w.document().atom(ids[1]).position.y() + .758) < 1e-9);
        // The mirror-image copy stays 6 Å beside the original instead of jumping across the origin.
        w.findChild<QCheckBox *>("mirrorCopy")->setChecked(true);
        w.findChild<QPushButton *>("mirrorSelection")->click();
        QCOMPARE(w.document().atoms.size(), 6);
        Ids copy;
        for (const auto &a : w.document().atoms)
            if (!ids.contains(a.id))
                copy << a.id;
        QVERIFY((centroid(w.document(), copy) - before - Vec(6, 0, 0)).norm() < 1e-9);
        // About the origin, as before.
        w.history()->undo();
        auto *pivot = w.findChild<QComboBox *>("transformPivot");
        pivot->setCurrentIndex(pivot->findData("origin"));
        w.findChild<QDoubleSpinBox *>("rotationAngle")->setValue(180);
        w.findChild<QPushButton *>("rotateSelection")->click();
        QVERIFY((centroid(w.document(), ids) - Vec(-before.x(), -before.y(), before.z())).norm() < 1e-9);
    }
    void trackballZoomAndSliders() {
        MainWindow w;
        w.resize(1300, 900);
        w.show();
        QTest::qWait(40);
        auto *view = w.canvas();
        view->setAngles(30, 20, -10);
        const auto angles = view->angles();
        QVERIFY((angles - Vec(30, 20, -10)).norm() < 1e-9);
        view->viewFrom(0);
        QCOMPARE(w.findChild<QSlider *>("editorYaw")->value(), -90);
        // Zooming keeps the atom under the cursor in place.
        const auto id = w.document().atoms.first().id;
        const auto before = view->atomScreenPosition(id);
        QWheelEvent wheel(before, view->mapToGlobal(before), {}, QPoint(0, 240), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(view, &wheel);
        QVERIFY(QLineF(view->atomScreenPosition(id), before).length() < 1e-6);
    }
    void candidateUndoRedo() {
        MainWindow w;
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        w.loadDocument(d);
        auto id = d.atoms[0].id;
        w.edit("削除", [id](Document &d) { d.remove({id}); }, true);
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().atoms.size(), 3);
        QCOMPARE(w.displayDocument().atoms.size(), 2);
        w.cancelCandidate();
        QCOMPARE(w.document().atoms.size(), 3);
        w.edit("削除", [id](Document &d) { d.remove({id}); }, true);
        w.confirmCandidate();
        QCOMPARE(w.document().atoms.size(), 2);
        w.history()->undo();
        QCOMPARE(w.document().atoms.size(), 3);
        w.history()->redo();
        QCOMPARE(w.document().atoms.size(), 2);
    }
    void paletteAndCanvas() {
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        QTest::qWait(40);
        auto *element = w.findChild<QComboBox *>("elementSelector");
        element->setCurrentIndex(element->findData("C"));
        w.findChild<QToolButton *>("tool_atom")->click();
        // The hint under the view says what a click does with this tool and element.
        QVERIFY(w.findChild<QLabel *>("toolHint")->text().startsWith("原子追加（C）"));
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().formula(), QString("CH4"));
        auto id = w.document().atoms.first().id;
        w.findChild<QToolButton *>("tool_select")->click();
        QVERIFY(w.findChild<QLabel *>("toolHint")->text().startsWith("クリック: 選択"));
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier,
                          w.canvas()->atomScreenPosition(id).toPoint());
        QVERIFY(w.canvas()->selection().contains(id));
        QTest::keyClick(w.canvas(), Qt::Key_Delete);
        QVERIFY(w.hasCandidate());
        QTest::keyClick(w.canvas(), Qt::Key_Escape);
        QVERIFY(!w.hasCandidate());
        QCOMPARE(w.document().formula(), QString("CH4"));
    }
    void rotationControlsLayout_data() {
        QTest::addColumn<int>("tab");
        QTest::addColumn<int>("width");
        for (int tab : {0, 6})
            for (int width : {900, 1380, 2200})
                QTest::newRow(qPrintable(QString("%1-%2").arg(tab == 0 ? "molecule" : "results").arg(width)))
                    << tab << width;
    }
    void rotationControlsLayout() {
        QFETCH(int, tab);
        QFETCH(int, width);
        MainWindow w;
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        tabs->setCurrentIndex(tab);
        w.resize(width, 900);
        w.show();
        QTest::qWait(40);
        const auto labels = tabs->currentWidget()->findChildren<QLabel *>();
        QRect previous, controls;
        int sliderWidth = 0;
        for (const auto &text : {QString("左右回転"), QString("上下回転"), QString("画面内回転")}) {
            QLabel *label = nullptr;
            for (auto *candidate : labels)
                if (candidate->text() == text && qobject_cast<QSlider *>(candidate->buddy()))
                    label = candidate;
            QVERIFY(label);
            auto *slider = qobject_cast<QSlider *>(label->buddy());
            QVERIFY(label->isVisible());
            QVERIFY(slider->isVisible());
            const QRect labelRect(label->mapTo(&w, QPoint()), label->size());
            const QRect sliderRect(slider->mapTo(&w, QPoint()), slider->size());
            QCOMPARE(labelRect.left(), sliderRect.left());
            QVERIFY(labelRect.bottom() < sliderRect.top());
            QVERIFY(sliderRect.top() - labelRect.bottom() <= 6);
            QVERIFY(label->width() >= label->sizeHint().width());
            if (!previous.isNull()) {
                QVERIFY(sliderRect.left() - previous.right() >= 12);
                QVERIFY(std::abs(slider->width() - sliderWidth) <= 1);
            }
            previous = sliderRect;
            sliderWidth = slider->width();
            controls = controls.united(labelRect).united(sliderRect);
        }
        const auto artifacts = qEnvironmentVariable("MOLFORMA_ROTATION_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            const auto path = QDir(artifacts).filePath(QString::fromLatin1(QTest::currentDataTag()));
            QVERIFY(w.grab().save(path + ".png"));
            QVERIFY(w.grab(controls.adjusted(-4, -4, 4, 4)).save(path + "-controls.png"));
        }
    }
    void skeletonPalette() {
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        auto *category = w.findChild<QComboBox *>("paletteCategory");
        auto *search = w.findChild<QLineEdit *>("paletteSearch");
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        category->setCurrentIndex(category->findData("skeleton"));
        QTest::qWait(40);
        QCOMPARE(palette->count(), 19);
        for (int row = 0; row < palette->count(); ++row) {
            QVERIFY(!palette->item(row)->text().contains('\n'));
            QCOMPARE(palette->visualItemRect(palette->item(row)).height(), 48);
        }
        search->setText("benzene");
        QCOMPARE(palette->count(), 1);
        QCOMPARE(palette->item(0)->text(), QString("ベンゼン"));
        palette->clearSelection();
        auto image = palette->viewport()->grab().toImage();
        int atomPixels = 0;
        for (int y = 0; y < qRound(48 * image.devicePixelRatio()); ++y)
            for (int x = 0; x < qRound(64 * image.devicePixelRatio()); ++x)
                if (image.pixelColor(x, y) == QColor("#475569"))
                    ++atomPixels;
        QVERIFY2(atomPixels > 20, "The molecule thumbnail is missing from the palette row");
        QVERIFY(w.document().atoms.isEmpty());
        QTest::mouseClick(palette->viewport(), Qt::LeftButton, Qt::NoModifier,
                          palette->visualItemRect(palette->item(0)).center());
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().formula(), QString("C6H6"));
        w.history()->undo();
        QVERIFY(w.document().atoms.isEmpty());
        category->setCurrentIndex(category->findData("amino"));
        search->setText("Histidine");
        QCOMPARE(palette->count(), 3);
        QSet<QString> names;
        for (int row = 0; row < palette->count(); ++row)
            names.insert(palette->item(row)->text());
        QCOMPARE(names, QSet<QString>({"ヒスチジン（HIE）", "ヒスチジン（HID）", "ヒスチジン（HIP, +1）"}));
    }
    void periodicTableLayoutAndSearch() {
        PeriodicTableDialog dialog("C");
        dialog.show();
        QTest::qWait(40);
        const auto buttons = dialog.findChildren<QAbstractButton *>(QRegularExpression("^periodicElement_"));
        QCOMPARE(buttons.size(), 118);
        auto *grid = dialog.findChild<QGridLayout *>("periodicTableGrid");
        QVERIFY(grid);
        auto check = [&](const QString &symbol, int row, int column) {
            auto *button = dialog.findChild<QAbstractButton *>("periodicElement_" + symbol);
            QVERIFY(button);
            QCOMPARE(grid->itemAtPosition(row, column)->widget(), button);
            QVERIFY(button->isEnabled());
            QVERIFY(button->width() >= 34);
            // Every group is visible without scrolling sideways, also on a small screen.
            auto *viewport = dialog.findChild<QScrollArea *>("periodicTableScroll")->viewport();
            QVERIFY(button->mapTo(viewport, QPoint(button->width(), 0)).x() <= viewport->width());
        };
        check("H", 1, 1);
        check("He", 1, 18);
        check("C", 2, 14);
        check("Fe", 4, 8);
        check("La", 9, 3);
        check("Lu", 9, 17);
        check("Ac", 10, 3);
        check("U", 10, 6);
        check("Lr", 10, 17);
        check("Og", 7, 18);
        auto *search = dialog.findChild<QLineEdit *>("periodicTableSearch");
        auto enabled = [&] {
            QStringList symbols;
            for (auto *button : buttons)
                if (button->isEnabled())
                    symbols << button->text();
            return symbols;
        };
        for (const auto &query : {"ウラン", "uranium", "92", "u"}) {
            search->setText(QString::fromUtf8(query));
            QCOMPARE(enabled(), QStringList{"U"});
        }
        search->setText("118");
        QCOMPARE(enabled(), QStringList{"Og"});
        search->setText("存在しない元素");
        QVERIFY(enabled().isEmpty());
        QTest::keyClick(search, Qt::Key_Return);
        QVERIFY(dialog.isVisible());
        search->clear();
        QCOMPARE(enabled().size(), 118);
        auto *h = dialog.findChild<QAbstractButton *>("periodicElement_H");
        auto *he = dialog.findChild<QAbstractButton *>("periodicElement_He");
        h->setFocus();
        QTest::keyClick(h, Qt::Key_Right);
        QCOMPARE(dialog.focusWidget(), he);
        if (auto path = qEnvironmentVariable("MOLFORMA_PERIODIC_SCREENSHOT"); !path.isEmpty())
            QVERIFY(dialog.grab().save(path));
        search->setText("118");
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.selectedElement(), QString("Og"));
    }
    void coordinationCenterPalette_data() {
        QTest::addColumn<QString>("categoryId");
        QTest::newRow("centers") << QString("center");
        QTest::newRow("all") << QString("all");
    }
    void coordinationCenterPalette() {
        QFETCH(QString, categoryId);
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        auto *category = w.findChild<QComboBox *>("paletteCategory");
        auto *search = w.findChild<QLineEdit *>("paletteSearch");
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        category->setCurrentIndex(category->findData(categoryId));
        QTest::qWait(40);
        int centers = 0;
        for (int row = 0; row < palette->count(); ++row) {
            auto *item = palette->item(row);
            const auto id = item->data(Qt::UserRole).toString();
            if (!id.startsWith("center:"))
                continue;
            ++centers;
            const auto &e = element(id.mid(7));
            QCOMPARE(item->text(), e.nameJa);
            QVERIFY(palette->visualItemRect(item).height() >= 48);
            QVERIFY(item->toolTip().contains(e.nameEn));
            QVERIFY(item->data(Qt::AccessibleTextRole).toString().contains("配位中心"));
        }
        QCOMPARE(centers, 25);
        search->setText("存在しない配位中心");
        QCOMPARE(palette->count(), 0);
        // "すべて" also offers the element itself for the atom tool, before the center.
        const int first = categoryId == "all" ? 1 : 0;
        for (const auto &query : {"亜鉛", "zinc", "zN", "30"}) {
            search->setText(QString::fromUtf8(query));
            QCOMPARE(palette->count(), first + 1);
            if (first)
                QCOMPARE(palette->item(0)->data(Qt::UserRole).toString(), QString("atom:Zn"));
            QCOMPARE(palette->item(first)->data(Qt::UserRole).toString(), QString("center:Zn"));
            QCOMPARE(palette->item(first)->text(), QString("亜鉛"));
        }
        palette->clearSelection();
        const auto image = palette->viewport()->grab().toImage();
        int colorPixels = 0;
        for (int y = 0; y < qRound(48 * image.devicePixelRatio()); ++y)
            for (int x = 0; x < qRound(64 * image.devicePixelRatio()); ++x)
                if (image.pixelColor(x, y) == element("Zn").color)
                    ++colorPixels;
        QVERIFY2(colorPixels > 100, "The coordination center element icon is missing");

        const auto rowRect = palette->visualItemRect(palette->item(first));
        QTest::mouseClick(palette->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(rowRect.left() + 30, rowRect.center().y()));
        QVERIFY(w.findChild<QToolButton *>("tool_center")->isChecked());
        QCOMPARE(w.findChild<QComboBox *>("elementSelector")->currentData().toString(), QString("Zn"));
        QCOMPARE(w.findChild<QTabWidget *>("propertyTabs")->currentIndex(), 1);
        QVERIFY(w.document().atoms.isEmpty());
        QCOMPARE(w.history()->count(), 0);
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().atoms.size(), 1);
        QCOMPARE(w.document().atoms[0].element, QString("Zn"));
        const auto centerRows = w.document().data["centers"].toArray();
        QCOMPARE(centerRows.size(), 1);
        const auto center = centerRows[0].toObject();
        QCOMPARE(center["atom"].toString(), w.document().atoms[0].id);
        QCOMPARE(center["geometry"].toString(), QString("octahedral"));
        QCOMPARE(center["sites"].toArray().size(), 6);
        w.history()->undo();
        QVERIFY(w.document().atoms.isEmpty());
        QVERIFY(w.document().data["centers"].toArray().isEmpty());
        w.history()->redo();
        QCOMPARE(w.document().atoms[0].element, QString("Zn"));
        QCOMPARE(w.document().data["centers"].toArray(), centerRows);

        search->clear();
        QCOMPARE(palette->currentItem()->data(Qt::UserRole).toString(), QString("center:Zn"));
        const auto artifacts = qEnvironmentVariable("MOLFORMA_CENTER_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            for (int width : {1380, 900}) {
                w.resize(width, 900);
                palette->scrollToItem(palette->currentItem(), QAbstractItemView::PositionAtCenter);
                QTest::qWait(40);
                const auto path = QDir(artifacts).filePath(QString("%1-%2").arg(categoryId).arg(width));
                QVERIFY(w.grab().save(path + ".png"));
                QVERIFY(palette->grab().save(path + "-palette.png"));
            }
        }
    }
    void periodicTablePlacement() {
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        QTest::qWait(40);
        auto *category = w.findChild<QComboBox *>("paletteCategory");
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        auto *open = w.findChild<QPushButton *>("openPeriodicTable");
        category->setCurrentIndex(category->findData("atom"));
        QVERIFY(open->isVisible());
        QCOMPARE(palette->count(), 10); // The chosen element (C) and the other common ones.
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = w.findChild<PeriodicTableDialog *>("periodicTableDialog");
            if (dialog)
                dialog->findChild<QAbstractButton *>("periodicElement_U")->click();
        });
        open->click();
        QCOMPARE(palette->count(), 11);
        QCOMPARE(palette->item(0)->data(Qt::UserRole).toString(), QString("atom:U"));
        QCOMPARE(palette->item(0)->text(), QString("ウラン"));
        QVERIFY(palette->visualItemRect(palette->item(0)).height() >= 48);
        QCOMPARE(w.findChild<QComboBox *>("elementSelector")->currentData().toString(), QString("U"));
        QVERIFY(w.document().atoms.isEmpty());
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().atoms.size(), 1);
        QCOMPARE(w.document().atoms[0].element, QString("U"));
        w.history()->undo();
        QVERIFY(w.document().atoms.isEmpty());
        w.history()->redo();
        QCOMPARE(w.document().atoms[0].element, QString("U"));
        QTimer::singleShot(0, &w, [&] {
            if (auto *dialog = w.findChild<PeriodicTableDialog *>("periodicTableDialog"))
                QTest::keyClick(dialog, Qt::Key_Escape);
        });
        open->click();
        QCOMPARE(palette->item(0)->data(Qt::UserRole).toString(), QString("atom:U"));
        QCOMPARE(w.document().atoms.size(), 1);
        category->setCurrentIndex(category->findData("skeleton"));
        QVERIFY(!open->isVisible());
        QCOMPARE(palette->count(), 19);
    }
    void customPartThumbnailRefresh() {
        QStandardItemModel model(1, 1);
        auto t = findTemplate("skeleton:benzene");
        t["id"] = "my:preview";
        auto index = model.index(0, 0);
        model.setData(index, t, TemplateDelegate::TemplateRole);
        InspectableTemplateDelegate delegate;
        QWidget widget;
        QStyleOptionViewItem option;
        option.initFrom(&widget);
        option.widget = &widget;
        delegate.initStyleOption(&option, index);
        const auto ratio = widget.devicePixelRatioF();
        const auto before = option.icon.pixmap(option.decorationSize, ratio).toImage();
        QVERIFY(!before.isNull());
        QCOMPARE(before.size(), QSize(qRound(60 * ratio), qRound(44 * ratio)));
        QCOMPARE(before.pixelColor(0, 0).alpha(), 0);
        // Reimporting a custom part can change its connection without changing its ID.
        t["connections"] = QJsonArray{0};
        model.setData(index, t, TemplateDelegate::TemplateRole);
        delegate.initStyleOption(&option, index);
        const auto after = option.icon.pixmap(option.decorationSize, ratio).toImage();
        QVERIFY(after != before);
        bool hasConnection = false;
        for (int y = 0; y < after.height(); ++y)
            for (int x = 0; x < after.width(); ++x)
                hasConnection |= after.pixelColor(x, y) == QColor("#16a34a");
        QVERIFY(hasConnection);
    }
    void sequenceUi() {
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        auto *tabs = w.findChild<QTabWidget *>("propertyTabs");
        tabs->setCurrentIndex(2);
        auto *input = w.findChild<QLineEdit *>("sequenceInput");
        input->setText("AGS");
        auto *build = w.findChild<QPushButton *>("buildSequence");
        QVERIFY(build);
        build->click();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().atoms.size(), 0);
        QCOMPARE(w.displayDocument().formula(), QString("C8H15N3O5"));
        w.findChild<QPushButton *>("confirmCandidate")->click();
        QCOMPARE(w.document().atoms.size(), 31);
        w.history()->undo();
        QVERIFY(w.document().atoms.isEmpty());
        // Replacing the molecule keeps calculation settings and project envelope fields.
        Document water;
        placeTemplate(water, findTemplate("skeleton:water"), Vec::Zero());
        water.data["nwchem"] = QJsonObject{{"basis", "cc-pvdz"}};
        water.envelope["result_reference"] = "kept";
        w.loadDocument(water);
        QCheckBox *replace = nullptr;
        for (auto *box : w.findChildren<QCheckBox *>())
            if (box->text() == "既存の分子を置き換える")
                replace = box;
        QVERIFY(replace);
        replace->setChecked(true);
        build->click();
        QVERIFY(w.hasCandidate());
        w.findChild<QPushButton *>("confirmCandidate")->click();
        QCOMPARE(w.document().formula(), QString("C8H15N3O5"));
        QCOMPARE(w.document().data["nwchem"].toObject()["basis"].toString(), QString("cc-pvdz"));
        QCOMPARE(w.document().envelope["result_reference"].toString(), QString("kept"));
    }
    void nwchemMoleculeControls() {
        MainWindow w;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        d.atoms[0].fixed = true;
        w.loadDocument(d);
        auto *tabs = w.findChild<QTabWidget *>("propertyTabs");
        QStringList names;
        for (int i = 0; i < tabs->count(); ++i)
            names << tabs->tabText(i);
        QCOMPARE(names, QStringList({"編集", "配位", "残基", "電荷"}));
        for (auto *b : tabs->findChildren<QPushButton *>()) {
            QVERIFY(!b->text().contains("FMO"));
            QVERIFY(!b->text().contains("QM/MM"));
            QVERIFY(!b->text().contains("スキャン"));
            QVERIFY(!b->text().contains("拘束に追加"));
        }
        auto *color = w.findChild<QComboBox *>("moleculeColorMode");
        QCOMPARE(color->count(), 2);
        QVERIFY(w.findChild<QPushButton *>("preparePdb"));

        auto *optimize = w.findChild<QPushButton *>("setNwchemOptimization");
        QVERIFY(optimize);
        optimize->click();
        QVERIFY(nwchemInput(w.document()).endsWith("task scf optimize\n"));
        QCOMPARE(w.document().job(), d.job());
        QCOMPARE(w.findChild<QComboBox *>("nwchem_task")->currentData().toString(), "optimize");
        w.history()->undo();
        QVERIFY(nwchemInput(w.document()).endsWith("task scf energy\n"));
        w.history()->redo();

        // Mixed selections must be fixed explicitly regardless of the first atom's state.
        w.setSelection({d.atoms[0].id, d.atoms[1].id});
        w.findChild<QPushButton *>("fixSelectedAtoms")->click();
        QVERIFY(nwchemInput(w.document()).contains("fix atom 1 2\n"));
        for (int i = 0; i < d.atoms.size(); ++i)
            QCOMPARE(w.document().atoms[i].position, d.atoms[i].position);
        w.history()->undo();
        QVERIFY(w.document().atoms[0].fixed);
        QVERIFY(!w.document().atoms[1].fixed);
        w.history()->redo();
        const auto fixedId = d.atoms[0].id;
        w.edit("reorder", [](Document &next) { std::swap(next.atoms[0], next.atoms[2]); });
        QVERIFY(nwchemInput(w.document()).contains("fix atom 2 3\n"));
        w.setSelection({fixedId});
        w.findChild<QPushButton *>("unfixSelectedAtoms")->click();
        QVERIFY(nwchemInput(w.document()).contains("fix atom 2\n"));
        QVERIFY(!w.document().atom(fixedId).fixed);
        w.history()->undo();
        QVERIFY(w.document().atom(fixedId).fixed);

        const auto artifacts = qEnvironmentVariable("MOLFORMA_MOLECULE_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            w.resize(1380, 1000);
            w.show();
            for (int i = 0; i < tabs->count(); ++i) {
                tabs->setCurrentIndex(i);
                QTest::qWait(40);
                QVERIFY(w.grab().save(QDir(artifacts).filePath(QString("molecule-%1.png").arg(i))));
            }
        }
    }
    void moleculeElectronicState() {
        MainWindow w;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        w.loadDocument(d);
        auto *charge = w.findChild<QSpinBox *>("moleculeTotalCharge");
        auto *mult = w.findChild<QSpinBox *>("moleculeMultiplicity");
        auto *summary = w.findChild<QLabel *>("moleculeChargeSummary");
        QVERIFY(charge && mult && summary);
        // Like "3 計算条件", each value applies at once as its own undoable edit.
        charge->setValue(1);
        QCOMPARE(w.document().job()["charge"].toInt(), 1);
        QVERIFY(summary->text().contains("整合しません"));
        QVERIFY(!w.findChild<QPushButton *>("calculationCopy")->isEnabled());
        mult->setValue(2);
        QVERIFY(summary->text().contains("電子数 9"));
        QVERIFY(summary->text().contains("α 5 / β 4"));
        QVERIFY(nwchemInput(w.document()).contains("charge 1\n"));
        QVERIFY(nwchemInput(w.document()).contains("nopen 1\n"));
        QCOMPARE(w.findChild<QSpinBox *>("nwchem_charge")->value(), 1);
        QCOMPARE(w.findChild<QSpinBox *>("nwchem_multiplicity")->value(), 2);
        w.history()->undo();
        QCOMPARE(mult->value(), 1);
        w.history()->undo();
        QCOMPARE(charge->value(), 0);
        w.history()->redo();
        w.history()->redo();
        w.findChild<QSpinBox *>("nwchem_charge")->setValue(0);
        w.findChild<QSpinBox *>("nwchem_multiplicity")->setValue(1);
        QCOMPARE(charge->value(), 0);
        QCOMPARE(mult->value(), 1);
        w.edit("candidate", [](Document &next) { next.setJobValue("charge", 2); }, true);
        QVERIFY(!charge->isEnabled());
        QCOMPARE(charge->value(), 2);
        QVERIFY(summary->text().contains("未確定候補"));
        w.cancelCandidate();
        QVERIFY(charge->isEnabled());
        QCOMPARE(charge->value(), 0);

        // ECP electrons and ghost centers use the same validation as the input generator.
        d = Document::fromXyz("2\nRb ghost\nRb 0 0 0\nH 0 0 3\n");
        d.atoms[0].charge = 1;
        d.atoms[1].ghost = true;
        d.data["nwchem"] = QJsonObject{{"basis", "def2-svp"}};
        w.loadDocument(d);
        w.findChild<QPushButton *>("applyMoleculeFormalCharge")->click();
        QCOMPARE(charge->value(), 1);
        QVERIFY(summary->text().contains("電子数 8（ECP適用後）"));
        QVERIFY(summary->text().contains("α 4 / β 4"));
        QVERIFY(validateNwchem(w.document()).errors.isEmpty());
        w.findChild<QPushButton *>("moleculeInputPreview")->click();
        QCOMPARE(w.findChild<QTabWidget *>("workspaceTabs")->currentIndex(), 5);
    }
    void moleculePointChargesAndCompatibility() {
        MainWindow w;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        addReference(d, {d.atoms[0].id}, "freeze");
        d.data["assignments"] = QJsonObject{{"legacy", QJsonObject{{"note", "preserve"}}}};
        d.setJobValue("sections", QJsonObject{{"qmmm", QJsonObject{{"enabled", "t"}}}});
        d.envelope["external_field"] = "preserve";
        d.data["nwchem"] = QJsonObject{{"geometry_units", "bohr"}, {"future_setting", 42}};
        w.loadDocument(d);
        const auto original = w.document().project();
        auto *enabled = w.findChild<QCheckBox *>("moleculeIncludePointCharges");
        auto *calculation = w.findChild<QCheckBox *>("nwchem_include_point_charges");
        auto *x = w.findChild<QDoubleSpinBox *>("pointChargeX");
        auto *q = w.findChild<QDoubleSpinBox *>("pointChargeValue");
        QVERIFY(enabled && calculation && x && q);
        QVERIFY(!enabled->isChecked()); // The legacy QM/MM flag is not an NWChem switch.
        x->setValue(5);
        q->setValue(-.5);
        w.findChild<QPushButton *>("addPointCharge")->click();
        QCOMPARE(w.document().data["point_charges"].toArray().size(), 1);
        QVERIFY(!nwchemInput(w.document()).contains("\nbq units"));
        enabled->click();
        QVERIFY(calculation->isChecked());
        QVERIFY(nwchemInput(w.document()).contains("\nbq units angstrom\n"));
        QVERIFY(nwchemInput(w.document()).contains("5 0 0 -0.5"));
        QCOMPARE(validateNwchem(w.document()).electrons, 10);
        QCOMPARE(w.document().job(), d.job());
        QCOMPARE(w.document().data["references"], d.data["references"]);
        QCOMPARE(w.document().data["assignments"], d.data["assignments"]);
        QCOMPARE(w.document().data["nwchem"].toObject()["future_setting"].toInt(), 42);
        w.history()->undo();
        QVERIFY(!enabled->isChecked());
        QVERIFY(!calculation->isChecked());
        w.history()->redo();
        QVERIFY(enabled->isChecked());
        calculation->click();
        QVERIFY(!enabled->isChecked());
        w.history()->undo();
        QVERIFY(enabled->isChecked());

        auto *list = w.findChild<QListWidget *>("pointChargeList");
        list->setCurrentRow(0);
        const auto id = w.document().data["point_charges"].toArray()[0].toObject().value("id");
        x->setValue(6);
        q->setValue(.25);
        w.findChild<QPushButton *>("updatePointCharge")->click();
        QCOMPARE(w.document().data["point_charges"].toArray()[0].toObject()["id"], id);
        QVERIFY(nwchemInput(w.document()).contains("6 0 0 0.25"));
        w.findChild<QPushButton *>("deletePointCharge")->click();
        QVERIFY(w.document().data["point_charges"].toArray().isEmpty());
        QVERIFY(!nwchemInput(w.document()).contains("\nbq units"));
        w.history()->undo();
        QVERIFY(nwchemInput(w.document()).contains("6 0 0 0.25"));
        w.edit("candidate", [](Document &next) { next.data["point_charges"] = QJsonArray(); }, true);
        QVERIFY(!enabled->isEnabled());
        w.cancelCandidate();
        QVERIFY(enabled->isEnabled());

        QTemporaryDir dir;
        const auto path = dir.filePath("legacy.kgui.json");
        w.document().save(path);
        const auto loaded = Document::load(path);
        QCOMPARE(loaded.project(), w.document().project());
        QCOMPARE(loaded.envelope["external_field"].toString(), "preserve");
        while (w.history()->canUndo())
            w.history()->undo();
        QCOMPARE(w.document().project(), original);
    }
    void failedEditIsAtomic() {
        MainWindow w;
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        w.loadDocument(d);
        QVERIFY_EXCEPTION_THROWN(w.edit("invalid",
                                        [](Document &candidate) {
                                            candidate.atoms.clear();
                                            fail("reject");
                                        }),
                                 std::runtime_error);
        QCOMPARE(w.document().atoms.size(), 3);
        QCOMPARE(w.history()->count(), 0);
    }
    void newDocumentIsClean() {
        MainWindow w;
        w.show();
        int questions = 0;
        QTimer watcher;
        QObject::connect(&watcher, &QTimer::timeout, &w, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                ++questions;
                box->button(QMessageBox::Discard)->click();
            }
        });
        watcher.start(20);
        w.edit("原子を追加",
               [](Document &d) { placeTemplate(d, findTemplate("skeleton:methane"), Vec::Zero()); });
        w.canvas()->setAngles(40, 30, 10);
        w.findChild<QAction *>("newAction")->trigger();
        QCOMPARE(questions, 1);                      // The unsaved methane.
        QVERIFY(w.canvas()->angles().norm() < 1e-6); // A new molecule starts from the front.
        QVERIFY(w.document().atoms.isEmpty());
        QVERIFY(w.history()->isClean());
        QVERIFY(!w.windowTitle().contains('*'));
        QCOMPARE(w.findChild<QTabWidget *>("workspaceTabs")->currentIndex(), 0);
        // A new, untouched project has nothing to save.
        w.findChild<QAction *>("newAction")->trigger();
        QCOMPARE(questions, 1);
    }
    void toolKeys() {
        MainWindow w;
        w.show();
        QTest::qWait(20);
        w.canvas()->setFocus();
        QTest::keyClick(w.canvas(), Qt::Key_B);
        QVERIFY(w.findChild<QToolButton *>("tool_bond")->isChecked());
        QVERIFY(w.findChild<QToolButton *>("tool_bond")->toolTip().endsWith("（B）"));
        QTest::keyClick(w.canvas(), Qt::Key_S);
        QVERIFY(w.findChild<QToolButton *>("tool_select")->isChecked());
        // In the search field the letters are typed, not tools chosen.
        auto *search = w.findChild<QLineEdit *>("paletteSearch");
        search->setFocus();
        QTest::keyClick(search, Qt::Key_B);
        QCOMPARE(search->text(), QString("b"));
        QVERIFY(w.findChild<QToolButton *>("tool_select")->isChecked());
    }
    void bondToolChoosesKind() {
        MainWindow w;
        w.show();
        Document d;
        placeTemplate(d, findTemplate("skeleton:ethane"), Vec::Zero());
        w.loadDocument(d);
        auto *kind = w.findChild<QComboBox *>("toolBondKind");
        QVERIFY(kind->isHidden());
        w.findChild<QToolButton *>("tool_bond")->click();
        QVERIFY(!kind->isHidden()); // Beside the tool, not only at the bottom of the edit panel.
        kind->setCurrentIndex(kind->findData("double"));
        QVERIFY(w.findChild<QLabel *>("toolHint")->text().startsWith("結合（二重結合）"));
        Ids carbons;
        for (const auto &a : w.document().atoms)
            if (a.element == "C")
                carbons << a.id;
        emit w.canvas()->atomClicked(carbons[0], Qt::NoModifier);
        emit w.canvas()->atomClicked(carbons[1], Qt::NoModifier);
        QCOMPARE(w.document().bond(carbons[0], carbons[1])->kind, QString("double"));
    }
    void replaceToolKeepsWorking() {
        MainWindow w;
        Document d;
        placeTemplate(d, findTemplate("skeleton:methane"), Vec::Zero());
        w.loadDocument(d);
        w.findChild<QToolButton *>("tool_replace")->click();
        w.findChild<QToolButton *>("quickElement_O")->click();
        // Choosing what to put in stays with the replace tool.
        QVERIFY(w.findChild<QToolButton *>("tool_replace")->isChecked());
        QVERIFY(w.findChild<QLabel *>("toolHint")->text().startsWith("置換（酸素）"));
    }
    void moveToolCarriesHydrogens() {
        // The dragged atom moves with its own hydrogens, shown while dragging and applied on release.
        MainWindow w;
        w.resize(1300, 900);
        w.show();
        auto d = Document::fromXyz("6\nmethanol\nC 0 0 0\nO 1.43 0 0\nH 1.75 .9 0\nH -.36 1.03 0\n"
                                   "H -.36 -.51 .89\nH -.36 -.51 -.89\n");
        w.loadDocument(d);
        QTest::qWait(40);
        w.findChild<QToolButton *>("tool_move")->click();
        auto *view = w.canvas();
        const auto oxygen = w.document().atoms[1].id, hydroxyl = w.document().atoms[2].id,
                   carbon = w.document().atoms[0].id;
        const QPoint start = view->atomScreenPosition(oxygen).toPoint(), end = start + QPoint(0, -60);
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(view, start + QPoint(0, -30));
        QTest::mouseMove(view, end);
        QVERIFY(view->documentRef().atom(oxygen).position.y() > 0.3); // Moving with the cursor.
        QCOMPARE(w.document().atom(oxygen).position.y(), 0.0);        // Not applied yet.
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, end);
        const Vec shift = w.document().atom(oxygen).position - d.atoms[1].position;
        QVERIFY(shift.norm() > 0.3);
        QVERIFY((w.document().atom(hydroxyl).position - d.atoms[2].position - shift).norm() < 1e-9);
        QCOMPARE(w.document().atom(carbon).position, d.atoms[0].position);

        // A drag including a fixed atom is refused on release, so its preview does not move either.
        w.edit("固定", [&](Document &x) { x.atom(carbon).fixed = true; });
        const auto before = w.document().atom(oxygen).position;
        const QPoint from = view->atomScreenPosition(carbon).toPoint();
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(view, from + QPoint(0, -30));
        QTest::mouseMove(view, from + QPoint(0, -60));
        QCOMPARE(view->documentRef().atom(carbon).position, d.atoms[0].position);
        QTimer::singleShot(0, &w, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                box->accept();
        });
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, from + QPoint(0, -60));
        QCOMPARE(w.document().atom(carbon).position, d.atoms[0].position);
        QCOMPARE(w.document().atom(oxygen).position, before);
    }
    void ligandsPlaceOneAfterAnother() {
        // Clicking the next free site confirms the ligand placed before, without an error dialog.
        MainWindow w;
        w.loadDocument(Document());
        w.edit("配位中心を配置", [](Document &d) { addCenter(d, "Zn", "tetrahedral", Vec::Zero(), 2.1, 2); });
        const auto zinc = w.document().atoms.first().id;
        auto *search = w.findChild<QLineEdit *>("paletteSearch");
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        search->setText("アンミン");
        palette->setCurrentRow(0);
        emit palette->itemClicked(palette->item(0));
        QVERIFY(w.findChild<QToolButton *>("tool_ligand")->isChecked());
        emit w.canvas()->siteClicked(zinc, 0);
        QVERIFY(w.hasCandidate());
        emit w.canvas()->siteClicked(zinc, 1);
        QVERIFY(w.hasCandidate());
        QVERIFY(!QApplication::activeModalWidget());
        QCOMPARE(w.document().formula(), QString("H3NZn")); // The first ligand is confirmed.
        QCOMPARE(w.displayDocument().formula(), QString("H6N2Zn"));
    }
    void editsKeepTheCamera() {
        // Edits leave the camera alone while the molecule stays in view, and a candidate does not
        // resize the 3D view: atoms stay under the cursor for the next click.
        MainWindow w;
        w.resize(1300, 900);
        w.show();
        Document d;
        placeTemplate(d, findTemplate("skeleton:ethane"), Vec::Zero());
        w.loadDocument(d);
        QTest::qWait(40);
        auto *view = w.canvas();
        const auto size = view->size();
        const auto carbon = w.document().atoms.first().id;
        const auto before = view->atomScreenPosition(carbon);
        w.findChild<QToolButton *>("quickElement_O")->click();
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, (before + QPointF(-120, 80)).toPoint());
        QCOMPARE(w.document().atoms.size(), 8 + 3);
        QCOMPARE(view->atomScreenPosition(carbon), before);
        Ids hydrogens;
        for (const auto &a : w.document().atoms)
            if (a.element == "H")
                hydrogens << a.id;
        w.setSelection({hydrogens.first()});
        w.findChild<QAction *>("deleteAction")->trigger();
        QVERIFY(w.hasCandidate());
        QTest::qWait(20);
        QCOMPARE(view->size(), size);
        QCOMPARE(view->atomScreenPosition(carbon), before);
    }
    void deleteHydrogenWithCapping() {
        // Capping applies to bonds cut at heavy atoms: a deleted hydrogen is not put back.
        MainWindow w;
        Document d;
        placeTemplate(d, findTemplate("skeleton:methane"), Vec::Zero());
        w.loadDocument(d);
        QVERIFY(w.findChild<QCheckBox *>("deleteTerminate")->isChecked());
        Ids hydrogens;
        for (const auto &a : w.document().atoms)
            if (a.element == "H")
                hydrogens << a.id;
        w.setSelection({hydrogens[0]});
        w.findChild<QAction *>("deleteAction")->trigger();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.displayDocument().formula(), QString("CH3"));
    }
    void selectionFieldsFollowSelection() {
        MainWindow w;
        Document d;
        placeTemplate(d, findTemplate("skeleton:ethane"), Vec::Zero());
        w.loadDocument(d);
        auto *x = w.findChild<QDoubleSpinBox *>("dimensionValue");
        auto *apply = w.findChild<QPushButton *>("applyDimension");
        auto *remove = w.findChild<QPushButton *>("deleteSelection");
        const auto ids = w.document().ids();
        w.setSelection({ids[0], ids[1]});
        QVERIFY(apply->isEnabled() && remove->isEnabled());
        QVERIFY(x->value() > 1);
        // Without a selection nothing shows values of an earlier atom, and selection actions wait.
        w.setSelection({});
        QCOMPARE(x->value(), x->minimum());
        QCOMPARE(x->text(), QString("—"));
        QVERIFY(!apply->isEnabled() && !remove->isEnabled());
        QVERIFY(apply->toolTip().contains("2〜4原子"));
        w.setSelection({ids[0]});
        QVERIFY(!apply->isEnabled() && remove->isEnabled());
        // Delete without a selection only says what to select.
        w.setSelection({});
        QTest::keyClick(w.canvas(), Qt::Key_Delete);
        QVERIFY(!w.hasCandidate());
        QVERIFY(!QApplication::activeModalWidget());
    }
    void commonElementsAndPaletteSearch() {
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        auto *search = w.findChild<QLineEdit *>("paletteSearch");
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        // One click arms the atom tool with a common element.
        w.findChild<QToolButton *>("quickElement_N")->click();
        QVERIFY(w.findChild<QToolButton *>("tool_atom")->isChecked());
        QCOMPARE(w.findChild<QComboBox *>("elementSelector")->currentData().toString(), QString("N"));
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().formula(), QString("H3N"));
        // A symbol or Japanese name finds the element first.
        for (const auto &query : {"O", "酸素", "8"}) {
            search->setText(QString::fromUtf8(query));
            QVERIFY(palette->count() >= 1);
            QCOMPARE(palette->item(0)->data(Qt::UserRole).toString(), QString("atom:O"));
        }
        search->setText("N");
        QCOMPARE(palette->item(0)->data(Qt::UserRole).toString(), QString("atom:N"));
        // A search that is no part's name can be built as SMILES.
        search->setText("CCO");
        QCOMPARE(palette->count(), 1);
        QCOMPARE(palette->item(0)->data(Qt::UserRole).toString(), QString("smiles:CCO"));
        search->setText("存在しない部品");
        QCOMPARE(palette->count(), 0);
        // Without a search, "すべて" lists atoms, skeletons, … and metal centers under headings.
        search->clear();
        QStringList headings;
        for (int i = 0; i < palette->count(); ++i)
            if (palette->item(i)->data(TemplateDelegate::HeadingRole).toBool()) {
                QVERIFY(!(palette->item(i)->flags() & Qt::ItemIsSelectable));
                headings << palette->item(i)->text();
            }
        QCOMPARE(headings.size(), 6);
        QVERIFY(headings[0].startsWith("原子") && headings[1] == "骨格" &&
                headings[5].startsWith("配位中心"));
        QVERIFY(palette->currentItem()->flags() & Qt::ItemIsSelectable);
    }
    void undoNamesTheChangedTab() {
        MainWindow w;
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        w.loadDocument(d);
        // Nothing to undo, redo or confirm yet: the controls say so by being off.
        auto *undoButton = w.findChild<QPushButton *>("editorUndo");
        QVERIFY(!undoButton->isEnabled() && !w.findChild<QPushButton *>("editorRedo")->isEnabled());
        QVERIFY(!w.findChild<QAction *>("undoAction")->isEnabled());
        QVERIFY(!w.findChild<QAction *>("confirmAction")->isEnabled());
        QVERIFY(!w.findChild<QAction *>("stopCalculationAction")->isEnabled());
        w.edit("NWChem計算条件を変更", [](Document &next) {
            auto s = next.data["nwchem"].toObject();
            s["basis"] = "6-31g*";
            next.data["nwchem"] = s;
        });
        QVERIFY(undoButton->isEnabled());
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        tabs->setCurrentIndex(0);
        w.history()->undo();
        QCOMPARE(nwchemSettings(w.document())["basis"].toString(), QString("sto-3g"));
        QVERIFY(w.findChild<QPushButton *>("editorRedo")->isEnabled());
        QVERIFY(w.statusBar()->currentMessage().startsWith("元に戻しました: NWChem計算条件を変更"));
        QVERIFY(w.statusBar()->currentMessage().contains("「3 計算条件」"));
        tabs->setCurrentIndex(2);
        w.history()->redo();
        QCOMPARE(w.statusBar()->currentMessage(), QString("やり直しました: NWChem計算条件を変更"));
    }
    void readErrorsNameFileAndLine() {
        QTemporaryDir dir;
        const auto path = dir.filePath("broken.xyz");
        writeFile(path, "2\nbroken\nO 0 0 0\nH 1 x 0\n");
        MainWindow w;
        try {
            w.loadPath(path);
            QFAIL("opened a broken file");
        } catch (const std::exception &e) {
            const auto message = QString::fromUtf8(e.what());
            QVERIFY2(message.contains("broken.xyz") && message.contains("4行目"), qPrintable(message));
        }
    }
    void qtDialogTextsInJapanese() {
        // Qt's standard buttons follow the Japanese interface, with or without Qt's translation.
        installTranslations(*QCoreApplication::instance());
        QDialogButtonBox box(QDialogButtonBox::Save | QDialogButtonBox::Discard | QDialogButtonBox::Cancel |
                             QDialogButtonBox::Close);
        QCOMPARE(box.button(QDialogButtonBox::Cancel)->text(), QString("キャンセル"));
        QCOMPARE(box.button(QDialogButtonBox::Close)->text(), QString("閉じる"));
        QCOMPARE(box.button(QDialogButtonBox::Save)->text(), QString("保存"));
        QCOMPARE(box.button(QDialogButtonBox::Discard)->text(), QString("変更を破棄"));
        // Qt's own file dialog (used while MCP drives the window) has Japanese labels too.
        QFileDialog files;
        files.setOption(QFileDialog::DontUseNativeDialog);
        for (const auto *name : {"lookInLabel", "fileTypeLabel"}) {
            auto *label = files.findChild<QLabel *>(name);
            QVERIFY(label);
            QVERIFY2(!label->text().contains(QRegularExpression("[A-Za-z]{3}")), qPrintable(label->text()));
        }
    }
};
QTEST_MAIN(EditorTest)
#include "test_editor.moc"
