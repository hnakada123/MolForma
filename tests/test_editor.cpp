// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/coordination.h"
#include "editing/library.h"
#include "editing/operations.h"
#include "editing/peptide.h"
#include "model/geometry.h"
#include "model/settings.h"
#include "nwchem/nwchem.h"
#include "ui/editor_chrome.h"
#include "ui/editor_tool.h"
#include "ui/main_window.h"
#include "ui/periodic_table.h"
#include "ui/template_delegate.h"
#include "ui/translations.h"
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
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
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextList>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <QtTest>
using namespace mf;
// Older Qt6 versions only accept integer positions for drag-enter events (as in McpGui).
using DragPosition =
    std::conditional_t<std::is_constructible_v<QDragEnterEvent, QPointF, Qt::DropActions, const QMimeData *,
                                               Qt::MouseButtons, Qt::KeyboardModifiers>,
                       QPointF, QPoint>;
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
        qputenv("XDG_CONFIG_HOME", settingsHome.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsHome.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settingsHome.path()); // MolForma uses NativeFormat.
    }
    void paletteEntries() {
        // Palette rows are keyed by kind; parts by their ID, with the kind from their category.
        for (const auto &[kind, value] : {std::pair{PaletteKind::Atom, "C"},
                                          {PaletteKind::Center, "Fe"},
                                          {PaletteKind::Smiles, "CC(=O)O"}}) {
            const auto entry = paletteEntry(paletteKey(kind, value));
            QCOMPARE(entry.kind, kind);
            QCOMPARE(entry.value, QString(value));
        }
        QVERIFY(paletteEntry("center:Fe").element() && !paletteEntry("smiles:C").part());
        QCOMPARE(paletteEntry({}).kind, PaletteKind::Atom); // The element selector's atom.
        for (const auto &[id, kind] : {std::pair{"skeleton:benzene", PaletteKind::Part},
                                       {"substituent:methyl", PaletteKind::Substituent},
                                       {"ligand:en", PaletteKind::Ligand},
                                       {"amino:ALA:default", PaletteKind::Amino}}) {
            const auto entry = paletteEntry(id, findTemplate(id));
            QCOMPARE(entry.kind, kind);
            QVERIFY(entry.part());
            QCOMPARE(paletteKey(kind, id), QString(id));
        }
    }
    void isotopeValidationMatchesCoordinateTable() {
        MainWindow w;
        const auto carbon = Document::fromXyz("1\ncarbon\nC 0 0 0\n");
        w.loadDocument(carbon);
        w.setSelection(carbon.ids());
        const auto before = w.document().project();
        auto *mass = w.findChild<QLineEdit *>("atomMass");
        auto *apply = w.findChild<QPushButton *>("applyAtomAttributes");
        QVERIFY(mass && apply);
        mass->setText("400");
        bool rejected = false;
        QTimer::singleShot(0, &w, [&] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                rejected = box->text().contains("同位体");
                box->accept();
            }
        });
        apply->click();
        QVERIFY(rejected);
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), 0);
        mass->setText("13");
        apply->click();
        QCOMPARE(w.document().atoms[0].isotope, 13);
        w.history()->undo();
        QCOMPARE(w.document().project(), before);
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
        QDragEnterEvent enter(DragPosition(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &drop);
        QTRY_COMPARE(w.document().formula(), QString("Ne"));
        QCOMPARE(w.recentFiles().size(), 2);
        // The manual is embedded in the program.
        QString text, recipes, recipe;
        bool back = false;
        double imageWidth = 0, pageWidth = 0;
        int images = 0;
        QTimer::singleShot(0, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("manualDialog")) {
                auto *browser = dialog->findChild<QTextBrowser *>("manualText");
                text = browser->toPlainText();
                // The recipes page, and a link from it to one recipe, which "戻る" leaves again.
                dialog->findChild<QComboBox *>("manualPage")->setCurrentIndex(1);
                recipes = browser->toPlainText();
                browser->setSource(QUrl("recipes/atoms_and_bonds.md"));
                recipe = browser->toPlainText();
                // Its screenshots, also those inside the numbered steps, are drawn within the page.
                auto *document = browser->document();
                for (auto block = document->begin(); block.isValid(); block = block.next())
                    for (auto it = block.begin(); !it.atEnd(); ++it)
                        if (it.fragment().charFormat().isImageFormat()) {
                            // Figures inside the steps are indented like Markdown continuation paragraphs.
                            const auto format = block.blockFormat();
                            double indent = format.leftMargin() + format.indent() * document->indentWidth();
                            if (auto *list = block.textList())
                                indent += list->format().indent() * document->indentWidth();
                            ++images;
                            imageWidth =
                                std::max(imageWidth, document->documentMargin() + indent +
                                                         it.fragment().charFormat().toImageFormat().width());
                        }
                pageWidth = browser->viewport()->width();
                back = dialog->findChild<QPushButton *>("manualBack")->isEnabled();
                dialog->reject();
            }
        });
        w.findChild<QAction *>("manualAction")->trigger();
        QVERIFY(text.contains("MolForma") && text.size() > 1000);
        QVERIFY(recipes.contains("題材の一覧"));
        QVERIFY(recipe.contains("題材1 原子と結合から組む"));
        QVERIFY(back);
        QVERIFY(images > 10);
        QVERIFY2(imageWidth > 0 && imageWidth < pageWidth,
                 qPrintable(QString("%1 / %2").arg(imageWidth).arg(pageWidth)));
    }
    void menuIcons() {
        // Commands show an icon; options that are switched on or chosen show their check mark instead
        // (Fusion draws no check box beside an icon).
        MainWindow w;
        QStringList missing, hidden;
        std::function<void(QMenu *)> check = [&](QMenu *menu) {
            for (auto *a : menu->actions()) {
                if (a->isSeparator())
                    continue;
                if (a->isCheckable() == !a->icon().isNull())
                    (a->isCheckable() ? hidden : missing) << a->text();
                if (a->menu())
                    check(a->menu());
            }
        };
        for (auto *a : w.menuBar()->actions())
            check(a->menu());
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(", ")));
        QVERIFY2(hidden.isEmpty(), qPrintable(hidden.join(", ")));
    }
    void chargeCardRows() {
        // The charge page stays open while another molecule comes in: the rows of its first card, which
        // wrap (the value and its link, the electrons with a note), keep their wrapped height and do not
        // cover each other.
        MainWindow w;
        w.show();
        QTemporaryDir dir;
        QVERIFY(w.useSample(":/docs/recipes/samples/na_h2o4_charges.molforma.json",
                            dir.filePath("na.molforma.json")));
        w.findChild<QTabWidget *>("propertyTabs")->setCurrentIndex(5);
        QTest::qWait(50);
        w.findChild<QAction *>("newAction")->trigger(); // An empty molecule: the longest notes.
        QTest::qWait(50);
        QVERIFY(
            w.useSample(":/docs/recipes/samples/ethanol_od.molforma.json", dir.filePath("od.molforma.json")));
        QTest::qWait(50);
        auto *link = w.findChild<QWidget *>("moleculeChargeLink");
        auto *summary = w.findChild<QLabel *>("moleculeChargeSummary");
        auto *input = w.findChild<QWidget *>("moleculeInputState");
        QVERIFY(link && summary && input);
        auto *body = summary->parentWidget();
        const auto top = [body](QWidget *x) { return x->mapTo(body, QPoint()).y(); };
        QVERIFY2(top(link) + link->height() <= top(summary),
                 qPrintable(QString("link ends at %1, summary starts at %2")
                                .arg(top(link) + link->height())
                                .arg(top(summary))));
        QVERIFY2(summary->height() >= summary->heightForWidth(summary->width()),
                 qPrintable(QString("summary %1 px of %2")
                                .arg(summary->height())
                                .arg(summary->heightForWidth(summary->width()))));
        QVERIFY(top(summary) + summary->height() <= top(input));
    }
    void manualSamples() {
        // The practice files of the recipes are embedded: a molecule is opened from a copy that can be
        // saved, and a part joins "マイ部品" when its link in the manual is clicked.
        MainWindow w;
        QTemporaryDir dir;
        const auto copy = dir.filePath("ethanol.molforma.json");
        QVERIFY(w.useSample(":/docs/recipes/samples/ethanol.molforma.json", copy));
        QCOMPARE(w.document().formula(), QString("C2H6O"));
        QVERIFY(QFileInfo(copy).isWritable());
        QCOMPARE(w.recentFiles().first(), copy);
        bool clicked = false, closed = false;
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = w.findChild<QDialog *>("manualDialog");
            auto *browser = dialog->findChild<QTextBrowser *>("manualText");
            browser->setSource(QUrl("qrc:/docs/recipes/my_parts.md"));
            QTextCursor link;
            for (auto block = browser->document()->begin(); block.isValid() && link.isNull();
                 block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it)
                    if (it.fragment().charFormat().anchorHref() ==
                        "samples/trimethylsilyl.molforma-part.json") {
                        link = QTextCursor(browser->document());
                        link.setPosition(it.fragment().position() + 1);
                        break;
                    }
            if (!link.isNull()) {
                clicked = true;
                QTest::mouseClick(browser->viewport(), Qt::LeftButton, {},
                                  browser->cursorRect(link).center());
            }
            closed = !dialog->isVisible();
            if (!closed)
                dialog->reject();
        });
        w.findChild<QAction *>("manualAction")->trigger();
        QVERIFY(clicked);
        QVERIFY(closed);
        QStringList parts;
        for (const auto &part : QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
                                    .value("parts")
                                    .toJsonArray())
            parts << part.toObject()["id"].toString();
        QVERIFY2(parts.contains("my:トリメチルシリル"), qPrintable(parts.join(' ')));
        QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
            .remove("parts"); // Later tests start without parts.
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
            const QDir autosaves(MainWindow::autosaveDirectory());
            QVERIFY(autosaves.absolutePath().startsWith(settingsHome.path()));
            const auto copies = autosaves.entryList({"session-*.molforma.json"});
            QCOMPARE(copies.size(), 1);
            // Simulate a crash: keep the copy and leave a lock file of a process that no longer runs.
            QFile::copy(autosaves.filePath(copies.first()), autosaves.filePath("session-1-2.molforma.json"));
#ifdef Q_OS_WIN
            const auto host = qgetenv("COMPUTERNAME"); // QLockFile writes the NetBIOS name there.
#else
            const auto host = QSysInfo::machineHostName().toUtf8();
#endif
            writeFile(autosaves.filePath("session-1-2.lock"), QByteArray("999999\nmolforma\n") + host + "\n");
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
            QVERIFY(later.document().atoms.isEmpty()); // The window starts with a new document.
        }
        const QDir autosaves(MainWindow::autosaveDirectory());
        QVERIFY(autosaves.exists("session-1-2.molforma.json"));
        MainWindow next;
        QTimer::singleShot(0, &next, [&] { answer("復旧する"); });
        next.offerRecovery();
        QVERIFY(!autosaves.exists("session-1-2.molforma.json"));
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
        w.findChild<QAction *>("viewStyleBackbone")->trigger();
        QCOMPARE(view->style(), QString("backbone"));
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
        QVERIFY(text->text().contains("追加 1") && text->text().contains("削除 4"));
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
        // Labels are chosen in the "表示" menu, also opened by the button over the 3D view.
        w.findChild<QAction *>("labelMode_heavy")->trigger();
        QCOMPARE(view->labels(), QString("heavy"));
        QVERIFY(w.findChild<QToolButton *>("viewMenuButton")
                    ->menu()
                    ->actions()
                    .contains(w.findChild<QAction *>("showHydrogensAction")));
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
        auto *table = w.findChild<QTableView *>("coordinateTable");
        auto *layout = w.findChild<QComboBox *>("moleculeLayout");
        QCOMPARE(layout->currentData().toString(), QString("view")); // The 3D view alone at first.
        QCOMPARE(table->model()->index(0, 1).data().toString().toDouble(), 0.0);
        // While the coordinate table is hidden it waits; showing it under the view brings it up to date.
        w.edit("move", [](Document &d) { d.atoms[0].position.x() = 0.25; });
        QCOMPARE(table->model()->index(0, 1).data().toString().toDouble(), 0.0);
        w.findChild<QToolButton *>("moleculeLayout_both")->click();
        QVERIFY(table->isVisible() && w.canvas()->isVisible());
        QCOMPARE(table->model()->index(0, 1).data().toString().toDouble(), 0.25);
        // The table in view follows undo at once, once the Undo/Redo texts have moved too.
        w.history()->undo();
        QCOMPARE(table->model()->index(0, 1).data().toString().toDouble(), 0.0);
        QVERIFY(w.findChild<QPushButton *>("editorRedo")->isEnabled());
        tabs->setCurrentIndex(1);
        w.history()->redo();
        QCOMPARE(table->model()->index(0, 1).data().toString().toDouble(), 0.0);
        w.refreshStalePanels(); // What McpGui does before it reads or drives the GUI.
        QCOMPARE(table->model()->index(0, 1).data().toString().toDouble(), 0.25);
        // The input preview tab still shows when the input cannot be generated.
        const int preview = 3;
        QCOMPARE(tabs->tabText(preview), QString("4 入力プレビュー"));
        w.edit("multiplicity", [](Document &d) { d.multiplicity = 2; });
        QVERIFY(tabs->tabBar()->tabTextColor(preview).isValid());
        QVERIFY(tabs->tabToolTip(preview).contains("エラー"));
        w.history()->undo();
        QVERIFY(!tabs->tabBar()->tabTextColor(preview).isValid());
        // Selecting every atom selects every table row, keeping the click order; the table alone
        // hides the view, and the choice is kept for the next window.
        tabs->setCurrentIndex(0);
        w.findChild<QToolButton *>("moleculeLayout_table")->click();
        QVERIFY(table->isVisible() && !w.canvas()->isVisible());
        QCOMPARE(QSettings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma")
                     .value("molecule/layout")
                     .toString(),
                 QString("table"));
        const auto ids = w.document().ids();
        w.setSelection({ids[2], ids[0], ids[1]});
        QCOMPARE(table->selectionModel()->selectedRows().size(), 3);
        table->selectionModel()->select(table->model()->index(1, 0),
                                        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QCOMPARE(w.canvas()->selection(), Ids{ids[1]});
        w.findChild<QToolButton *>("moleculeLayout_view")->click(); // The other tests use the view.
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
        Document water;
        placeTemplate(water, findTemplate("skeleton:water"), Vec::Zero());
        w.loadDocument(water);
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
        w.findChild<QToolButton *>("tool_place")->click();
        // The hint under the view says what a click does with this tool and element.
        QVERIFY(w.findChild<QLabel *>("toolHint")->text().startsWith("配置（C）"));
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
        for (int tab : {0, 4})
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
        // Both keep the sliders in a popup under "回転" over the 3D view, a label before each. The
        // results viewer's button waits for a structure, so its popup is shown directly here.
        QWidget *area = nullptr;
        if (tab == 0) {
            w.findChild<QToolButton *>("rotationButton")->click();
            area = w.findChild<QWidget *>("rotationPopup");
        } else {
            area = w.findChild<QWidget *>("resultsRotationPopup");
            area->show();
        }
        QTRY_VERIFY(area->isVisible());
        const auto labels = area->findChildren<QLabel *>();
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
            const QRect labelRect(label->mapTo(area->window(), QPoint()), label->size());
            const QRect sliderRect(slider->mapTo(area->window(), QPoint()), slider->size());
            QVERIFY(label->width() >= label->sizeHint().width());
            QVERIFY(labelRect.right() < sliderRect.left());
            QVERIFY(labelRect.top() < sliderRect.bottom() && sliderRect.top() < labelRect.bottom());
            if (!previous.isNull()) {
                QVERIFY(sliderRect.top() > previous.bottom());
                QCOMPARE(sliderRect.left(), previous.left());
                QCOMPARE(slider->width(), sliderWidth);
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
            QVERIFY(area->window()->grab(controls.adjusted(-4, -4, 4, 4)).save(path + "-controls.png"));
        }
        area->hide();
    }
    void skeletonPalette() {
        MainWindow w;
        w.loadDocument(Document());
        w.show();
        auto *search = w.findChild<QLineEdit *>("paletteSearch");
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        w.findChild<QToolButton *>("paletteCategory_skeleton")->click();
        QTest::qWait(150); // The list lays the tiles out again after a resize.
        QCOMPARE(palette->count(), 19);
        // Equal tiles, several in a row, none cut off at the right.
        const auto first = palette->visualItemRect(palette->item(0));
        QVERIFY(first.height() >= 80);
        QCOMPARE(palette->visualItemRect(palette->item(1)).top(), first.top());
        for (int row = 0; row < palette->count(); ++row) {
            QVERIFY(!palette->item(row)->text().contains('\n'));
            const auto rect = palette->visualItemRect(palette->item(row));
            QCOMPARE(rect.size(), first.size());
            QVERIFY(rect.right() < palette->viewport()->width());
        }
        search->setText("benzene");
        QCOMPARE(palette->count(), 1);
        QCOMPARE(palette->item(0)->text(), QString("ベンゼン"));
        palette->clearSelection();
        auto image = palette->viewport()->grab(palette->visualItemRect(palette->item(0))).toImage();
        int atomPixels = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y) == QColor("#475569"))
                    ++atomPixels;
        QVERIFY2(atomPixels > 20, "The molecule thumbnail is missing from the palette tile");
        QVERIFY(w.document().atoms.isEmpty());
        QTest::mouseClick(palette->viewport(), Qt::LeftButton, Qt::NoModifier,
                          palette->visualItemRect(palette->item(0)).center());
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().formula(), QString("C6H6"));
        w.history()->undo();
        QVERIFY(w.document().atoms.isEmpty());
        w.findChild<QToolButton *>("paletteCategory_amino")->click();
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
        auto *search = w.findChild<QLineEdit *>("paletteSearch");
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        w.findChild<QToolButton *>("paletteCategory_" + categoryId)->click();
        QTest::qWait(150);
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
        const auto image = palette->viewport()->grab(palette->visualItemRect(palette->item(first))).toImage();
        int colorPixels = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y) == element("Zn").color)
                    ++colorPixels;
        QVERIFY2(colorPixels > 100, "The coordination center element icon is missing");

        const auto rowRect = palette->visualItemRect(palette->item(first));
        QTest::mouseClick(palette->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(rowRect.left() + 30, rowRect.center().y()));
        QVERIFY(w.findChild<QToolButton *>("tool_place")->isChecked());
        QCOMPARE(w.findChild<QComboBox *>("elementSelector")->currentData().toString(), QString("Zn"));
        auto *properties = w.findChild<QTabWidget *>("propertyTabs");
        QCOMPARE(properties->tabText(properties->currentIndex()), QString("配位"));
        QVERIFY(w.document().atoms.isEmpty());
        QCOMPARE(w.history()->count(), 0);
        QTest::mouseClick(w.canvas(), Qt::LeftButton, Qt::NoModifier, w.canvas()->rect().center());
        QCOMPARE(w.document().atoms.size(), 1);
        QCOMPARE(w.document().atoms[0].element, QString("Zn"));
        const auto centerRows = w.document().toJson()["centers"].toArray();
        QCOMPARE(w.document().centers.size(), 1);
        const auto &center = w.document().centers[0];
        QCOMPARE(center.atom, w.document().atoms[0].id);
        QCOMPARE(center.geometry, QString("octahedral"));
        QCOMPARE(center.sites.size(), 6);
        w.history()->undo();
        QVERIFY(w.document().atoms.isEmpty());
        QVERIFY(w.document().centers.isEmpty());
        w.history()->redo();
        QCOMPARE(w.document().atoms[0].element, QString("Zn"));
        QCOMPARE(w.document().toJson()["centers"].toArray(), centerRows);

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
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        auto *open = w.findChild<QPushButton *>("openPeriodicTable");
        w.findChild<QToolButton *>("paletteCategory_atom")->click();
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
        w.findChild<QToolButton *>("paletteCategory_skeleton")->click();
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
        w.findChild<QToolButton *>("propertyPage_4")->click(); // The segments over the pages.
        QCOMPARE(tabs->tabText(tabs->currentIndex()), QString("残基"));
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
    void residueReplacementTakesPaletteState() {
        // "選択残基を置換" puts in the state chosen in the palette (HIP), and the total charge follows.
        MainWindow w;
        Document d;
        buildPeptide(d, parseSequence("AHE"), "ACE", "NME", "L", "default");
        d.charge = d.formalCharge();
        w.loadDocument(d);
        w.show();
        QCOMPARE(w.document().charge, -1);
        QString his;
        for (const auto &r : w.document().residues)
            if (r.code == "HIS")
                his = r.atoms["CA"];
        w.setSelection({his});
        w.findChild<QToolButton *>("paletteCategory_amino")->click();
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        for (int i = 0; i < palette->count(); ++i)
            if (palette->item(i)->data(Qt::UserRole) == "amino:HIS:HIP")
                palette->setCurrentRow(i);
        QCOMPARE(palette->currentItem()->data(Qt::UserRole).toString(), QString("amino:HIS:HIP"));
        for (auto *b : w.findChildren<QPushButton *>())
            if (b->text() == "選択残基を置換")
                b->click();
        QVERIFY(w.hasCandidate());
        w.findChild<QPushButton *>("confirmCandidate")->click();
        QCOMPARE(w.document().formula(), QString("C17H26N6O6"));
        QCOMPARE(w.document().charge, 0);
    }
    // "周囲を選択" grows the selection of a residue to the residues around it; "選択範囲を切り出す" makes
    // them a candidate with the cut bonds capped and their atoms fixed.
    void siteCutOutUi() {
        MainWindow w;
        Document d;
        buildPeptide(d, parseSequence("AGKAG"), "charged", "charged");
        d.charge = d.formalCharge();
        w.loadDocument(d);
        w.show();
        const auto lys = w.document().residues[2].atoms.values();
        w.setSelection(lys);
        w.findChild<QDoubleSpinBox *>("siteRadius")->setValue(2);
        w.findChild<QPushButton *>("selectSite")->click();
        int around = 0;
        for (int r = 1; r <= 3; ++r)
            around += w.document().residues[r].atoms.size();
        QCOMPARE(w.selection().size(), around);
        w.findChild<QPushButton *>("cutOutSite")->click();
        QVERIFY(w.hasCandidate());
        QCOMPARE(w.document().atoms.size(), d.atoms.size()); // Nothing changes before the confirmation.
        w.findChild<QPushButton *>("confirmCandidate")->click();
        QCOMPARE(w.document().atoms.size(), around + 2);
        QCOMPARE(w.document().residues.size(), 3);
        QCOMPARE(std::count_if(w.document().atoms.begin(), w.document().atoms.end(),
                               [](const Atom &a) { return a.fixed; }),
                 2);
        QCOMPARE(w.document().charge, 1);
        QVERIFY(w.document().pointCharges.isEmpty());
        // The charged groups around stay as point charges, which the calculation then includes.
        w.loadDocument(d);
        w.setSelection(lys);
        w.findChild<QPushButton *>("selectSite")->click();
        auto *charges = w.findChild<QCheckBox *>("siteCharges");
        QVERIFY(!charges->isChecked() && !w.findChild<QDoubleSpinBox *>("siteChargeRadius")->isEnabled());
        charges->setChecked(true);
        QVERIFY(w.findChild<QDoubleSpinBox *>("siteChargeRadius")->isEnabled());
        w.findChild<QDoubleSpinBox *>("siteChargeRadius")->setValue(100);
        w.findChild<QPushButton *>("cutOutSite")->click();
        w.findChild<QPushButton *>("confirmCandidate")->click();
        QCOMPARE(w.document().pointCharges.size(), 3); // NH3+ +1, the C-terminal carboxylate −0.5 twice.
        QVERIFY(nwchemSettings(w.document())["include_point_charges"].toBool());
    }
    void attributePageHoldsAttributes() {
        MainWindow w;
        const auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        w.loadDocument(d);
        // "属性" has the attributes of atoms only; the rest is where it is used.
        auto *page = w.findChild<QTabWidget *>("propertyTabs")->widget(2);
        QStringList cards;
        for (auto *card : page->findChildren<QWidget *>())
            if (card->objectName().startsWith("moleculeCard_"))
                cards << card->objectName().mid(QString("moleculeCard_").size());
        QCOMPARE(cards, QStringList({"原子の属性", "ゴースト・固定"}));
        // Sorting: the menu "構造" of the coordinate table.
        w.findChild<QAction *>("coordinateSort_element")->trigger();
        QCOMPARE(w.document().atoms.first().element, QString("H"));
        w.history()->undo();
        w.setSelection({d.atoms[2].id});
        w.findChild<QAction *>("coordinateSort_selected")->trigger();
        QCOMPARE(w.document().atoms.first().id, d.atoms[2].id);
        w.history()->undo();
        QCOMPARE(w.document().toJson(), d.toJson());
        // The user's parts: a row of the palette's "マイ部品", saying how to add one while none.
        auto *myParts = w.findChild<QWidget *>("myPartsBar");
        QVERIFY(myParts->isHidden());
        w.findChild<QToolButton *>("paletteCategory_my")->click();
        QVERIFY(!myParts->isHidden());
        QVERIFY(!w.findChild<QLabel *>("myPartsEmpty")->isHidden());
        QVERIFY(w.findChild<QPushButton *>("savePart")->isEnabled());
        w.findChild<QToolButton *>("paletteCategory_all")->click();
        QVERIFY(myParts->isHidden());
        // Refining the selection beside "全体整形"; regenerating in the menus.
        auto *local = w.findChild<QPushButton *>("refineSelectionButton");
        QVERIFY(local->isEnabled());
        w.setSelection({});
        QVERIFY(!local->isEnabled());
        QVERIFY(w.findChild<QAction *>("regenerateAction"));
        // Without point charges, their list says how to add one.
        QVERIFY(w.findChild<QListWidget *>("pointChargeList")->isHidden());
        QVERIFY(!w.findChild<QLabel *>("pointChargeEmpty")->isHidden());
        w.edit("point", [](Document &next) { addPointCharge(next, Vec(3, 0, 0), 0.5); });
        QVERIFY(!w.findChild<QListWidget *>("pointChargeList")->isHidden());
        QVERIFY(w.findChild<QLabel *>("pointChargeEmpty")->isHidden());
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
        // One level of pages, chosen with segments (no tabs in tabs).
        QCOMPARE(names, QStringList({"編集", "変形", "属性", "配位", "残基", "電荷"}));
        QVERIFY(!tabs->tabBar()->isVisibleTo(&w));
        QCOMPARE(tabs->findChildren<QTabWidget *>().size(), 0);
        for (auto *b : tabs->findChildren<QPushButton *>()) {
            QVERIFY(!b->text().contains("FMO"));
            QVERIFY(!b->text().contains("QM/MM"));
            QVERIFY(!b->text().contains("スキャン"));
            QVERIFY(!b->text().contains("拘束に追加"));
        }
        auto *color = w.findChild<QComboBox *>("moleculeColorMode");
        QCOMPARE(color->count(), 4);
        // "単色": its color from a few, or any from the color dialog.
        auto *uniform = w.findChild<QComboBox *>("moleculeUniformColor");
        QVERIFY(!uniform->isEnabled());
        color->setCurrentIndex(color->findData("uniform"));
        QVERIFY(uniform->isEnabled());
        const auto oxygen = d.atoms[0].id;
        QCOMPARE(w.canvas()->atomColor(oxygen), QColor("#c8323c"));
        uniform->setCurrentIndex(uniform->findText("青"));
        QCOMPARE(w.canvas()->atomColor(oxygen), QColor("#3f6fb5"));
        const int other = uniform->count() - 1;
        const auto choose = [&](const QColor &chosen) {
            QTimer::singleShot(0, &w, [chosen] {
                auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                if (chosen.isValid()) {
                    dialog->setCurrentColor(chosen);
                    dialog->accept();
                } else
                    dialog->reject();
            });
            const int last = uniform->count() - 1; // "その他の色…", after the color so chosen.
            uniform->setCurrentIndex(last);
            emit uniform->activated(last);
        };
        choose(QColor()); // Cancelled: the last color stays.
        QCOMPARE(uniform->currentText(), QString("青"));
        QCOMPARE(w.canvas()->atomColor(oxygen), QColor("#3f6fb5"));
        choose(QColor("#123456"));
        QCOMPARE(uniform->currentText(), QString("#123456"));
        QCOMPARE(w.canvas()->atomColor(oxygen), QColor("#123456"));
        choose(QColor("#654321")); // The same item holds the next one.
        QCOMPARE(uniform->count(), other + 2);
        QCOMPARE(w.canvas()->atomColor(oxygen), QColor("#654321"));
        color->setCurrentIndex(color->findData("element"));
        QVERIFY(!uniform->isEnabled());
        QCOMPARE(w.canvas()->atomColor(oxygen), element("O").color);
        QVERIFY(w.findChild<QPushButton *>("preparePdb"));

        // The task is set in "2 計算条件" only.
        QVERIFY(!w.findChild<QPushButton *>("setNwchemOptimization"));
        w.findChild<QToolButton *>("nwchem_task_optimize")->click();
        QVERIFY(nwchemInput(w.document()).endsWith("task scf optimize\n"));
        QCOMPARE(w.document().charge, d.charge);
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
        // The charge and multiplicity are set in "2 計算条件" and shown on the charge page.
        QVERIFY(!w.findChild<QSpinBox *>("moleculeTotalCharge"));
        auto *charge = w.findChild<QSpinBox *>("nwchem_charge");
        auto *mult = w.findChild<QSpinBox *>("nwchem_multiplicity");
        auto *state = w.findChild<QLabel *>("moleculeChargeState");
        auto *summary = w.findChild<QLabel *>("moleculeChargeSummary");
        auto *input = w.findChild<QLabel *>("moleculeInputState");
        QVERIFY(charge && mult && state && summary && input);
        QCOMPARE(state->text(), QString("0 · 多重度 1"));
        charge->setValue(1);
        QCOMPARE(w.document().charge, 1);
        QCOMPARE(state->text(), QString("1 · 多重度 1"));
        QVERIFY(summary->text().contains("整合しません"));
        QVERIFY(input->text().contains("生成できません"));
        QVERIFY(w.findChild<QLabel *>("calculationStatus")->text().contains("生成できません"));
        mult->setValue(2);
        QVERIFY(summary->text().contains("電子数 9"));
        QVERIFY(summary->text().contains("α 5 / β 4"));
        QVERIFY(input->text().contains("入力を生成できます"));
        QVERIFY(nwchemInput(w.document()).contains("charge 1\n"));
        QVERIFY(nwchemInput(w.document()).contains("nopen 1\n"));
        w.history()->undo();
        QCOMPARE(state->text(), QString("1 · 多重度 1"));
        w.history()->undo();
        QCOMPARE(state->text(), QString("0 · 多重度 1"));
        // The link leads to the fields of "2 計算条件".
        w.findChild<QPushButton *>("moleculeChargeLink")->click();
        QCOMPARE(w.findChild<QTabWidget *>("workspaceTabs")->currentIndex(), 1);
        auto *page = w.findChild<QPushButton *>("applyMoleculeFormalCharge");
        w.edit("candidate", [](Document &next) { next.charge = 2; }, true);
        QVERIFY(!page->isEnabled());
        QCOMPARE(state->text(), QString("2 · 多重度 1"));
        QVERIFY(summary->text().contains("未確定候補"));
        w.cancelCandidate();
        QVERIFY(page->isEnabled());
        QCOMPARE(state->text(), QString("0 · 多重度 1"));

        // ECP electrons and ghost centers use the same validation as the input generator.
        d = Document::fromXyz("2\nRb ghost\nRb 0 0 0\nH 0 0 3\n");
        d.atoms[0].charge = 1;
        d.atoms[1].ghost = true;
        d.data["nwchem"] = QJsonObject{{"basis", "def2-svp"}};
        w.loadDocument(d);
        w.findChild<QPushButton *>("applyMoleculeFormalCharge")->click();
        QCOMPARE(w.document().charge, 1);
        QVERIFY(summary->text().contains("電子数 8（ECP適用後）"));
        QVERIFY(summary->text().contains("α 4 / β 4"));
        QVERIFY(validateNwchem(w.document()).errors.isEmpty());
        w.findChild<QPushButton *>("moleculeInputPreview")->click();
        QCOMPARE(w.findChild<QTabWidget *>("workspaceTabs")->currentIndex(), 3);
    }
    void chargeLinksFitTheColumn() {
        // The links to "2 計算条件" go under their values where the column is narrow, never cut at its edge.
        MainWindow w;
        w.loadDocument(Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n"));
        w.show();
        auto *tabs = w.findChild<QTabWidget *>("propertyTabs");
        tabs->setCurrentIndex(5);
        auto *scroll = qobject_cast<QScrollArea *>(tabs->currentWidget());
        QVERIFY(scroll);
        for (const auto size : {QSize(1380, 900), QSize(1100, 720)}) {
            w.resize(size);
            for (const auto *name : {"moleculeChargeLink", "moleculePointChargeLink"}) {
                auto *link = w.findChild<QPushButton *>(name);
                QTRY_VERIFY(link->isVisible() && link->width() >= link->sizeHint().width() &&
                            link->mapTo(scroll->viewport(), QPoint(link->width(), 0)).x() <=
                                scroll->viewport()->width());
            }
        }
    }
    void moleculePointChargesAndCompatibility() {
        MainWindow w;
        auto d = Document::fromXyz("3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n");
        d.data["future_document"] = QJsonObject{{"note", "preserve"}};
        d.envelope["external_field"] = "preserve";
        d.data["nwchem"] = QJsonObject{{"geometry_units", "bohr"}, {"future_setting", 42}};
        w.loadDocument(d);
        const auto original = w.document().project();
        // Whether the input includes the charges is set in "2 計算条件", and shown here.
        QVERIFY(!w.findChild<QCheckBox *>("moleculeIncludePointCharges"));
        auto *enabled = w.findChild<QLabel *>("moleculePointChargeState");
        auto *calculation = w.findChild<QCheckBox *>("nwchem_include_point_charges");
        auto *x = w.findChild<QDoubleSpinBox *>("pointChargeX");
        auto *q = w.findChild<QDoubleSpinBox *>("pointChargeValue");
        QVERIFY(enabled && calculation && x && q);
        QCOMPARE(enabled->text(), QString("NWChem入力に含めない"));
        x->setValue(5);
        q->setValue(-.5);
        w.findChild<QPushButton *>("addPointCharge")->click();
        QCOMPARE(w.document().pointCharges.size(), 1);
        QVERIFY(!nwchemInput(w.document()).contains("\nbq units"));
        calculation->click();
        QCOMPARE(enabled->text(), QString("NWChem入力に含める"));
        QVERIFY(nwchemInput(w.document()).contains("\nbq units angstrom\n"));
        QVERIFY(nwchemInput(w.document()).contains("5 0 0 -0.5"));
        QCOMPARE(validateNwchem(w.document()).electrons, 10);
        QCOMPARE(w.document().charge, d.charge);
        QCOMPARE(w.document().data["future_document"], d.data["future_document"]);
        QCOMPARE(w.document().data["nwchem"].toObject()["future_setting"].toInt(), 42);
        w.history()->undo();
        QCOMPARE(enabled->text(), QString("NWChem入力に含めない"));
        QVERIFY(!calculation->isChecked());
        w.history()->redo();
        QCOMPARE(enabled->text(), QString("NWChem入力に含める"));
        w.findChild<QPushButton *>("moleculePointChargeLink")->click();
        QCOMPARE(w.findChild<QTabWidget *>("workspaceTabs")->currentIndex(), 1);

        auto *list = w.findChild<QListWidget *>("pointChargeList");
        list->setCurrentRow(0);
        const auto id = w.document().pointCharges[0].id;
        x->setValue(6);
        q->setValue(.25);
        w.findChild<QPushButton *>("updatePointCharge")->click();
        QCOMPARE(w.document().pointCharges[0].id, id);
        QVERIFY(nwchemInput(w.document()).contains("6 0 0 0.25"));
        w.findChild<QPushButton *>("deletePointCharge")->click();
        QVERIFY(w.document().pointCharges.isEmpty());
        QVERIFY(!nwchemInput(w.document()).contains("\nbq units"));
        w.history()->undo();
        QVERIFY(nwchemInput(w.document()).contains("6 0 0 0.25"));
        w.edit("candidate", [](Document &next) { next.pointCharges.clear(); }, true);
        QVERIFY(!x->isEnabled());
        w.cancelCandidate();
        QVERIFY(x->isEnabled());

        QTemporaryDir dir;
        const auto path = dir.filePath("legacy.molforma.json");
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
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, w.edit("invalid", [](Document &candidate) {
            candidate.atoms.clear();
            fail("reject");
        }));
        QCOMPARE(w.document().atoms.size(), 3);
        QCOMPARE(w.history()->count(), 0);
    }
    void startsWithNewDocument() {
        // A new window starts like "新規": no molecule and nothing to save.
        {
            MainWindow w;
            QVERIFY(w.document().atoms.isEmpty());
            QVERIFY(w.history()->isClean());
            QVERIFY(!w.windowTitle().contains('*'));
        }
        // The preference still loads a water molecule when it is turned on.
        QSettings settings(SettingsFormat, QSettings::UserScope, "MolForma", "MolForma");
        settings.setValue("startupWater", true);
        MainWindow w;
        settings.remove("startupWater");
        QCOMPARE(w.document().formula(), QString("H2O"));
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
        QTest::keyClick(w.canvas(), Qt::Key_A);
        QVERIFY(w.findChild<QToolButton *>("tool_place")->isChecked());
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
        // In the option bar above the 3D view for the bond tool, and for "配置" of atoms, which
        // bonds them with it too; the selection unit is for the tools that pick atoms.
        auto *kind = w.findChild<QComboBox *>("toolBondKind");
        auto *options = w.findChild<QWidget *>("toolOptions");
        auto *unit = w.findChild<QWidget *>("selectionUnitOption");
        QVERIFY(options->isAncestorOf(kind));
        QVERIFY(!kind->isVisibleTo(options) && unit->isVisibleTo(options));
        w.findChild<QToolButton *>("tool_bond")->click();
        QVERIFY(kind->isVisibleTo(options) && !unit->isVisibleTo(options));
        w.findChild<QToolButton *>("quickElement_C")->click();
        QVERIFY(w.findChild<QToolButton *>("tool_place")->isChecked());
        QVERIFY(kind->isVisibleTo(options));
        QVERIFY(w.findChild<QLabel *>("toolPart")->text().contains("炭素（C）"));
        w.findChild<QToolButton *>("tool_bond")->click();
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
    void bondEditingRoutes_data() {
        QTest::addColumn<QString>("route");
        QTest::addColumn<bool>("hydrogens");
        for (const auto &route : {"menu", "context", "tool", "panel", "shortcut"})
            for (bool hydrogens : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(route).arg(hydrogens)))
                    << QString(route) << hydrogens;
    }
    void bondEditingRoutes() {
        QFETCH(QString, route);
        QFETCH(bool, hydrogens);
        MainWindow w;
        Document d;
        placeTemplate(d, findTemplate("skeleton:ethane"), Vec::Zero());
        w.loadDocument(d);
        Ids carbons;
        for (const auto &a : d.atoms)
            if (a.element == "C")
                carbons << a.id;
        for (auto *box : w.findChildren<QCheckBox *>())
            if (box->text() == "水素自動補完")
                box->setChecked(hydrogens);
        w.setSelection(carbons);
        if (route == "menu")
            w.findChild<QAction *>("bondOrder2Action")->trigger();
        else if (route == "shortcut")
            emit w.canvas()->shortcut(Qt::Key_2);
        else if (route == "context") {
            bool triggered = false;
            QTimer::singleShot(0, &w, [&] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (!menu)
                    return;
                for (auto *action : menu->actions())
                    if (auto *sub = action->menu())
                        for (auto *bond : sub->actions())
                            if (bond->text() == "二重結合") {
                                bond->trigger();
                                triggered = true;
                            }
                menu->close();
            });
            emit w.canvas()->contextRequested(QPoint(10, 10), carbons[0]);
            QVERIFY(triggered);
        } else {
            auto *kind = w.findChild<QComboBox *>("toolBondKind");
            kind->setCurrentIndex(kind->findData("double"));
            if (route == "tool") {
                w.findChild<QToolButton *>("tool_bond")->click();
                w.setSelection({carbons[0]});
                emit w.canvas()->atomClicked(carbons[1], Qt::NoModifier);
            } else
                for (auto *button : w.findChildren<QPushButton *>())
                    if (button->text() == "選択した2原子を結合")
                        button->click();
        }
        QCOMPARE(w.document().bond(carbons[0], carbons[1])->kind, QString("double"));
        QCOMPARE(w.document().formula(), hydrogens ? QString("C2H4") : QString("C2H6"));
        QCOMPARE(w.history()->count(), 1);
        const auto changed = w.document().project();
        w.history()->undo();
        QCOMPARE(w.document().project(), d.project());
        w.history()->redo();
        QCOMPARE(w.document().project(), changed);
    }
    void paletteElementsAndCenters() {
        // What the palette highlights is what a click places: element buttons leave a part category
        // for "原子" and choose the center element under "配位中心", and a category change places the
        // highlighted center rather than the element chosen before.
        MainWindow w;
        w.show();
        w.loadDocument(Document());
        const auto click = [&](const char *name) { w.findChild<QToolButton *>(name)->click(); };
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        const auto current = [&] { return palette->currentItem()->data(Qt::UserRole).toString(); };
        click("paletteCategory_substituent");
        click("quickElement_F");
        QVERIFY(w.findChild<QToolButton *>("paletteCategory_atom")->isChecked());
        QCOMPARE(current(), QString("atom:F"));
        emit w.canvas()->blankClicked(Vec::Zero());
        QCOMPARE(w.document().atoms[0].element, QString("F"));
        click("quickElement_O");
        click("paletteCategory_center");
        QCOMPARE(current(), QString("center:Li"));
        QVERIFY(w.findChild<QLabel *>("toolHint")->text().contains("配位中心 Li"));
        emit w.canvas()->blankClicked(Vec(5, 0, 0));
        QCOMPARE(w.document().atoms.last().element, QString("Li"));
        click("quickElement_S");
        QCOMPARE(current(), QString("center:S"));
        QVERIFY(w.findChild<QToolButton *>("quickElement_S")->isChecked());
        emit w.canvas()->blankClicked(Vec(10, 0, 0));
        QCOMPARE(w.document().atoms.last().element, QString("S"));
        QCOMPARE(w.document().centers.size(), 2);
        // The search under "配位中心" finds any element as a center.
        w.findChild<QLineEdit *>("paletteSearch")->setText("Xe");
        QCOMPARE(palette->item(0)->data(Qt::UserRole).toString(), QString("center:Xe"));
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
        QVERIFY(w.findChild<QLabel *>("toolHint")->text().startsWith("置き換え（酸素）"));
    }
    void placeToolFollowsPalette() {
        // One "配置" tool puts down what the palette has chosen; the clicked target decides how.
        MainWindow w;
        w.loadDocument(Document());
        auto *palette = w.findChild<QListWidget *>("templatePalette");
        auto *place = w.findChild<QToolButton *>("tool_place");
        // The tools sit in the rail at the left edge, apart from the palette.
        auto *rail = w.findChild<QWidget *>("toolRail");
        QCOMPARE(place->parentWidget(), rail);
        QVERIFY(!rail->isAncestorOf(palette));
        QCOMPARE(w.findChildren<QToolButton *>(QRegularExpression("^tool_")).size(), 7);
        const auto hint = [&] { return w.findChild<QLabel *>("toolHint")->text(); };
        const auto choose = [&](const QString &key) {
            for (int i = 0; i < palette->count(); ++i)
                if (palette->item(i)->data(Qt::UserRole) == key) {
                    palette->setCurrentRow(i);
                    emit palette->itemClicked(palette->item(i));
                    return true;
                }
            return false;
        };
        QVERIFY(choose("skeleton:methane"));
        QVERIFY(place->isChecked());
        QVERIFY(hint().startsWith("配置（メタン）"));
        emit w.canvas()->blankClicked(Vec::Zero());
        QCOMPARE(w.document().formula(), QString("CH4"));
        // A part put down returns to "選択", which no longer shows it as chosen; "配置" shows it again.
        QVERIFY(w.findChild<QToolButton *>("tool_select")->isChecked());
        QVERIFY(palette->selectedItems().isEmpty());
        place->click();
        QCOMPARE(palette->selectedItems(), QList{palette->currentItem()});
        QVERIFY(hint().startsWith("配置（メタン）"));
        // A substituent replaces the clicked hydrogen.
        QVERIFY(choose("substituent:methyl"));
        QVERIFY(hint().contains("置き換える水素をクリック"));
        QString hydrogen;
        for (const auto &a : w.document().atoms)
            if (a.element == "H")
                hydrogen = a.id;
        emit w.canvas()->atomClicked(hydrogen, Qt::NoModifier);
        QCOMPARE(w.document().formula(), QString("C2H6"));
        // A ligand goes to a free site of a center, or on its own on a blank.
        QVERIFY(choose("ligand:ammine"));
        QVERIFY(hint().contains("紫の番号"));
        emit w.canvas()->blankClicked(Vec(6, 0, 0));
        QCOMPARE(w.document().formula(), QString("C2H9N"));
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
        QVERIFY(w.findChild<QToolButton *>("tool_place")->isChecked());
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
        // As the layouts settle, before the next click: bars that turn compact where the new tool's
        // settings would wrap them (a wide font) post their changes again.
        for (int pass = 0; pass < 3; ++pass)
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
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
    void deletionFollowsCharge() {
        // Deleting a center takes its oxidation number out of the total charge when the two matched;
        // a charge set by hand stays.
        MainWindow w;
        Document d;
        const auto sodium = addCenter(d, "Na", "tetrahedral", Vec::Zero(), 2.4, 1);
        placeTemplate(d, findTemplate("skeleton:water"), Vec(5, 0, 0));
        w.loadDocument(d);
        QCOMPARE(w.document().charge, 1);
        w.setSelection({sodium});
        w.findChild<QAction *>("deleteAction")->trigger();
        w.confirmCandidate();
        QCOMPARE(w.document().formula(), QString("H2O"));
        QCOMPARE(w.document().charge, 0);
        w.history()->undo();
        w.edit("全電荷", [](Document &d) { d.charge = 2; });
        w.setSelection({sodium});
        w.findChild<QAction *>("deleteAction")->trigger();
        w.confirmCandidate();
        QCOMPARE(w.document().charge, 2);
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
        // One click arms the place tool with a common element.
        w.findChild<QToolButton *>("quickElement_N")->click();
        QVERIFY(w.findChild<QToolButton *>("tool_place")->isChecked());
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
        QVERIFY(w.statusBar()->currentMessage().contains("「2 計算条件」"));
        tabs->setCurrentIndex(1);
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
        // Japanese whatever the system's language, which would otherwise choose it.
        qputenv("MOLFORMA_LANGUAGE", "ja");
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
    void editorChromeFollowsTheWork() {
        // The header steps switch the tabs, the option bar follows the tool, the corners of the 3D
        // view show the sequence and the measurement, and a candidate takes the option bar's place.
        MainWindow w;
        w.resize(1380, 900);
        w.show();
        Document d;
        buildPeptide(d, {"ALA", "GLY", "SER"}, "charged", "charged");
        w.loadDocument(d);
        QTest::qWait(150);
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        QVERIFY(!tabs->tabBar()->isVisible());
        QCOMPARE(w.findChildren<QToolButton *>(QRegularExpression("^workflowStep_")).size(), tabs->count());
        auto *step2 = w.findChild<QToolButton *>("workflowStep_2");
        QCOMPARE(step2->text(), QString("計算条件"));
        QCOMPARE(step2->accessibleName(), QString("2 計算条件"));
        step2->click();
        QCOMPARE(tabs->currentIndex(), 1);
        tabs->setCurrentIndex(0);
        QVERIFY(w.findChild<QToolButton *>("workflowStep_1")->isChecked());
        // An input that cannot be generated marks its step as on the hidden tab.
        auto *step4 = w.findChild<QToolButton *>("workflowStep_4");
        QVERIFY(!step4->property("failed").toBool());
        w.edit("multiplicity", [](Document &x) { x.multiplicity = 2; });
        QVERIFY(step4->property("failed").toBool());
        QVERIFY(step4->toolTip().contains("エラー"));
        w.history()->undo();
        QVERIFY(!step4->property("failed").toBool());
        auto *header = dynamic_cast<HeaderBar *>(w.findChild<QWidget *>("headerBar"));
        QVERIFY(header);
        QVERIFY(header->singleRow());
        QVERIFY(w.findChild<QToolButton *>("documentButton")->text().startsWith("新規分子"));
        // The sequence over the view selects a residue; 2–4 atoms show their measurement.
        auto *chip = w.findChild<QWidget *>("sequenceChip");
        auto *card = w.findChild<QWidget *>("measureCard");
        QVERIFY(chip->isVisible());
        QVERIFY(w.findChild<QLabel *>("sequenceLabel")->text().contains("GLY 2"));
        QVERIFY(!card->isVisible());
        const auto ids = w.document().ids();
        w.setSelection({ids[0], ids[1], ids[2]});
        QVERIFY(card->isVisible());
        QCOMPARE(w.findChild<QLabel *>("measureTitle")->text(), QString("角度"));
        QVERIFY(w.findChild<QLabel *>("measurementLabel")->text().contains("°"));
        QCOMPARE(w.findChild<QLabel *>("selectionTitle")->text(), QString("選択 3 原子"));
        QVERIFY(w.findChild<QLabel *>("selectionList")->text().contains("ALA 1 · "));
        auto *view = w.canvas();
        const auto viewArea = QRect(view->mapTo(&w, QPoint()), view->size());
        for (auto *overlay : {chip, card, w.findChild<QWidget *>("viewControls")}) {
            const QRect area(overlay->mapTo(&w, QPoint()), overlay->size());
            QVERIFY(viewArea.contains(area));
        }
        QVERIFY(!QRect(chip->mapTo(&w, QPoint()), chip->size())
                     .intersects(QRect(w.findChild<QWidget *>("viewControls")->mapTo(&w, QPoint()),
                                       w.findChild<QWidget *>("viewControls")->size())));
        // Tools change the options above the view, not the view itself.
        auto *top = w.findChild<QWidget *>("editorTopRow");
        const int height = top->height();
        for (const auto *tool :
             {"tool_move", "tool_bond", "tool_delete", "tool_place", "tool_charge", "tool_select"}) {
            w.findChild<QToolButton *>(tool)->click();
            // Once the layout has settled (slow builds, such as AddressSanitizer, take a while).
            QTRY_COMPARE(top->height(), height);
            QTRY_COMPARE(QRect(view->mapTo(&w, QPoint()), view->size()), viewArea);
        }
        QVERIFY(w.findChild<QWidget *>("chargeOption")->isVisibleTo(top) == false);
        // A candidate: the banner in the option bar's place and its changes listed in "編集".
        w.findChild<QAction *>("deleteAction")->trigger();
        QVERIFY(w.hasCandidate());
        QTest::qWait(10);
        QVERIFY(w.findChild<QWidget *>("candidateBanner")->isVisible());
        QVERIFY(w.findChild<QLabel *>("candidateBannerTitle")->text().startsWith("変更候補: "));
        QVERIFY(w.findChild<QWidget *>("candidateCard")->isVisible());
        QVERIFY(w.findChild<QLabel *>("candidateChanges")->text().contains("削除"));
        QCOMPARE(top->height(), height);
        QCOMPARE(QRect(view->mapTo(&w, QPoint()), view->size()), viewArea);
        const auto artifacts = qEnvironmentVariable("MOLFORMA_EDITOR_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            QVERIFY(w.grab().save(QDir(artifacts).filePath("candidate-1380.png")));
        }
        w.cancelCandidate();
        QVERIFY(!w.findChild<QWidget *>("candidateCard")->isVisible());
        if (!artifacts.isEmpty())
            QVERIFY(w.grab().save(QDir(artifacts).filePath("selection-1380.png")));
        // A narrow window puts the steps on a second header row, all of them visible.
        w.resize(900, 760);
        QTest::qWait(150);
        QVERIFY(!header->singleRow());
        auto *steps = w.findChild<QWidget *>("workflowSteps");
        QVERIFY(QRect(QPoint(), w.size()).contains(QRect(steps->mapTo(&w, QPoint()), steps->size())));
        if (!artifacts.isEmpty())
            QVERIFY(w.grab().save(QDir(artifacts).filePath("selection-900.png")));
        w.resize(1600, 900);
        QTest::qWait(150);
        QVERIFY(header->singleRow());
        // The accent is MolForma's own blue, not a pale selection color of the desktop theme.
        const auto light = QApplication::palette();
        auto pale = light;
        pale.setColor(QPalette::Highlight, QColor("#b299ec"));
        pale.setColor(QPalette::HighlightedText, QColor(Qt::black));
        QApplication::setPalette(pale);
        QTest::qWait(20);
        QVERIFY(header->styleSheet().contains("#1d5fd1"));
        QVERIFY(!header->styleSheet().contains("#b299ec"));
        QVERIFY(w.findChild<QLabel *>("sequenceLabel")->text().contains("color:#1d5fd1"));
        // A dark palette restyles the header, rail, option bar and overlays from its colors.
        QPalette dark(QColor("#2b2d31"), QColor("#202225"));
        dark.setColor(QPalette::Base, QColor("#1e1f22"));
        dark.setColor(QPalette::Text, QColor("#e3e5e8"));
        dark.setColor(QPalette::WindowText, QColor("#e3e5e8"));
        dark.setColor(QPalette::ButtonText, QColor("#e3e5e8"));
        dark.setColor(QPalette::Highlight, QColor("#4c8dff"));
        dark.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        QApplication::setPalette(dark);
        QTest::qWait(20);
        QVERIFY(header->styleSheet().contains("#1e1f22") && header->styleSheet().contains("#2f6fe4"));
        if (!artifacts.isEmpty()) {
            w.resize(1380, 900);
            QTest::qWait(150);
            QVERIFY(w.grab().save(QDir(artifacts).filePath("dark-1380.png")));
        }
        QApplication::setPalette(light);
        QTest::qWait(20);
        QVERIFY(!header->styleSheet().contains("#1e1f22"));
    }
};
QTEST_MAIN(EditorTest)
#include "test_editor.moc"
