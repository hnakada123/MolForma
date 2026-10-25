#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include "ui/input_preview_panel.h"
#include "ui/main_window.h"
#include <QtTest>
#include <QtWidgets>
using namespace mf;

class InputPreviewTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings, files;
    static Document water() {
        return Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
    }
    static void set(Document &d, const QString &key, const QJsonValue &value) {
        auto s = d.data["nwchem"].toObject();
        s[key] = value;
        d.data["nwchem"] = s;
    }
    template <typename T> static T *widget(MainWindow &w, const char *name) {
        auto *result = w.findChild<T *>(name);
        Q_ASSERT(result);
        return result;
    }
    static QPlainTextEdit *text(MainWindow &w) {
        return widget<QPlainTextEdit>(w, "inputPreviewText");
    }
    static InputPreviewPanel *panel(MainWindow &w) {
        return widget<InputPreviewPanel>(w, "inputPreviewTab");
    }
    static void openPreview(MainWindow &w) {
        widget<QTabWidget>(w, "workspaceTabs")->setCurrentIndex(5);
    }
  private slots:
    void initTestCase() {
        QVERIFY(settings.isValid());
        QVERIFY(files.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
    }
    void nativeTabReadOnlyCopyAndSave() {
        auto d = water();
        d.envelope["execution"] = QJsonObject{{"legacy_exe", "preserved"}};
        d.envelope["result_reference"] = QJsonObject{{"job_dir", "old-result"}};
        d.data["future_field"] = QJsonObject{{"value", 42}};
        MainWindow w;
        w.loadDocument(d);
        const auto before = w.document().project();
        auto *tabs = widget<QTabWidget>(w, "workspaceTabs");
        QCOMPARE(tabs->count(), 8);
        QCOMPARE(tabs->tabText(5), "6 入力プレビュー");
        widget<QPushButton>(w, "calculationShowPreview")->click(); // The link of "3 計算条件".
        QCOMPARE(tabs->currentIndex(), 5);
        QVERIFY(text(w)->isReadOnly());
        QCOMPARE(text(w)->toPlainText(), nwchemInput(d));
        widget<QPushButton>(w, "inputPreviewCopy")->click();
        QCOMPARE(QApplication::clipboard()->text(), nwchemInput(d));
        const auto path = files.filePath("日本語 input.nw");
        panel(w)->saveInput(path);
        QCOMPARE(readFile(path), nwchemInput(d).toUtf8());
        panel(w)->refresh();
        QCOMPARE(w.document().project(), before);
        QCOMPARE(w.history()->count(), 0);
        QVERIFY(w.history()->isClean());
        // No runtime installation is required to preview/export either input type.
        w.edit("missing engine", [&](Document &next) {
            setExecutionSetting(next, "executable", files.filePath("missing-nwchem"));
            setExecutionSetting(next, "launcher", files.filePath("missing-mpirun"));
        });
        widget<QComboBox>(w, "inputPreviewMode")->setCurrentIndex(1);
        QVERIFY(widget<QPushButton>(w, "inputPreviewSave")->isEnabled());
        QCOMPARE(text(w)->toPlainText(), nwchemInput(d));
    }
    void editsUndoCandidatesAndInvalidation() {
        MainWindow w;
        w.loadDocument(water());
        openPreview(w);
        const auto original = text(w)->toPlainText();
        w.edit("DFT", [](Document &d) { set(d, "method", "dft"); });
        QVERIFY(text(w)->toPlainText().contains("task dft energy"));
        w.history()->undo();
        QCOMPARE(text(w)->toPlainText(), original);
        w.history()->redo();
        QVERIFY(text(w)->toPlainText().contains("task dft energy"));
        w.edit("move candidate", [](Document &d) { d.atoms[1].position.x() += 0.1; }, true);
        QCOMPARE(text(w)->toPlainText(), nwchemInput(w.displayDocument()));
        QVERIFY(!widget<QPushButton>(w, "inputPreviewCopy")->isEnabled());
        const auto path = files.filePath("protect.nw");
        writeFile(path, "keep");
        QVERIFY_EXCEPTION_THROWN(panel(w)->saveInput(path), std::runtime_error);
        QCOMPARE(readFile(path), QByteArray("keep"));
        w.cancelCandidate();
        QCOMPARE(text(w)->toPlainText(), nwchemInput(w.document()));
        QVERIFY(widget<QPushButton>(w, "inputPreviewSave")->isEnabled());
        w.edit("confirm candidate", [](Document &d) { d.atoms[1].position.x() += 0.2; }, true);
        w.confirmCandidate();
        QCOMPARE(text(w)->toPlainText(), nwchemInput(w.document()));
        QVERIFY(widget<QPushButton>(w, "inputPreviewSave")->isEnabled());
        w.edit("invalid spin", [](Document &d) { d.setJobValue("multiplicity", 2); });
        QVERIFY(text(w)->toPlainText().isEmpty());
        QVERIFY(!widget<QPushButton>(w, "inputPreviewSave")->isEnabled());
        QVERIFY(widget<QTextBrowser>(w, "inputPreviewDiagnostics")->toPlainText().contains("多重度"));
        QVERIFY_EXCEPTION_THROWN(panel(w)->saveInput(path), std::runtime_error);
        QCOMPARE(readFile(path), QByteArray("keep"));
        w.history()->undo();
        QVERIFY(!text(w)->toPlainText().isEmpty());
        w.loadDocument(Document());
        QVERIFY(text(w)->toPlainText().isEmpty());
        QVERIFY(!widget<QPushButton>(w, "inputPreviewCopy")->isEnabled());
    }
    void executionPathsMatchSavedBundle() {
        auto d = water();
        setExecutionSetting(d, "job_root", "preview-jobs");
        setExecutionSetting(d, "reference_directory", "orbitals");
        setNwchemAdvanced(d, "vectors_guess", "file");
        setNwchemAdvanced(d, "vectors_input", "initial.movecs");
        setNwchemAdvanced(d, "vectors_output", "output/final.movecs");
        setNwchemAdvanced(d, "scratch_dir", "scratch");
        const auto project = files.filePath("preview.kgui.json");
        d.save(project);
        MainWindow w;
        w.loadPath(project);
        openPreview(w);
        const auto before = w.document().project();
        QVERIFY(text(w)->toPlainText().contains("input \"initial.movecs\""));
        auto *mode = widget<QComboBox>(w, "inputPreviewMode");
        mode->setCurrentIndex(1);
        const auto generated = text(w)->toPlainText();
        QVERIFY(generated.contains(files.filePath("orbitals/initial.movecs")));
        QVERIFY(!QFileInfo::exists(files.filePath("preview-jobs")));
        QCOMPARE(generated, executionPreviewPlan(w.document(), files.path()).input);
        const auto plan = saveExecutionBundle(w.document(), files.path());
        QCOMPARE(generated.toUtf8(), readFile(QDir(plan.directory).filePath("input.nw")));
        panel(w)->saveInput(files.filePath("resolved.nw"));
        QCOMPARE(readFile(files.filePath("resolved.nw")), generated.toUtf8());
        QCOMPARE(w.document().project(), before);
        QVERIFY(w.history()->isClean());
        // Execution-only path restrictions must not block ordinary input export.
        w.edit("outside job", [](Document &next) { setNwchemAdvanced(next, "scratch_dir", "../outside"); });
        QVERIFY(text(w)->toPlainText().isEmpty());
        mode->setCurrentIndex(0);
        QVERIFY(text(w)->toPlainText().contains("scratch_dir ../outside\n"));
        QVERIFY(widget<QPushButton>(w, "inputPreviewSave")->isEnabled());
    }
    void chemistryWorkerBlocksExportUntilCancelled() {
        MainWindow w;
        w.loadDocument(water());
        openPreview(w);
        widget<QPushButton>(w, "refineButton")->click();
        QVERIFY(!widget<QPushButton>(w, "inputPreviewSave")->isEnabled());
        QVERIFY(widget<QLabel>(w, "inputPreviewHint")->text().contains("整形中"));
        QVERIFY_EXCEPTION_THROWN(panel(w)->saveInput(files.filePath("during-worker.nw")), std::runtime_error);
        QVERIFY(!QFileInfo::exists(files.filePath("during-worker.nw")));
        // Cancel synchronously, before the test executable used as worker can deliver any result.
        QPushButton *cancel = nullptr;
        for (auto *button : w.findChildren<QPushButton *>())
            if (button->text() == "整形を中止")
                cancel = button;
        QVERIFY(cancel);
        cancel->click();
        QVERIFY(widget<QPushButton>(w, "inputPreviewSave")->isEnabled());
        QCOMPARE(text(w)->toPlainText(), nwchemInput(w.document()));
    }
    void warningsNavigationAndEscaping() {
        MainWindow w;
        auto d = water();
        setNwchemAdvanced(d, "dft_energy", 1e-9);
        setNwchemAdvanced(d, "<future>&", 3);
        w.loadDocument(d);
        openPreview(w);
        auto *checks = widget<QTextBrowser>(w, "inputPreviewDiagnostics");
        QVERIFY(checks->toPlainText().contains("保留"));
        QVERIFY(checks->toPlainText().contains("<future>&"));
        QVERIFY(widget<QPushButton>(w, "inputPreviewSave")->isEnabled());
        QVERIFY(!text(w)->toPlainText().contains("convergence energy"));
        QVERIFY(QMetaObject::invokeMethod(checks, "anchorClicked", Q_ARG(QUrl, QUrl("tab:3"))));
        QCOMPARE(widget<QTabWidget>(w, "workspaceTabs")->currentIndex(), 3);
        openPreview(w);
        QVERIFY(QMetaObject::invokeMethod(widget<QLabel>(w, "inputPreviewPaths"), "linkActivated",
                                          Q_ARG(QString, "tab:4")));
        QCOMPARE(widget<QTabWidget>(w, "workspaceTabs")->currentIndex(), 4);
        w.edit("apply DFT", [](Document &next) { set(next, "method", "dft"); });
        QVERIFY(text(w)->toPlainText().contains("convergence energy 1e-09"));
        QVERIFY(!checks->toPlainText().contains("保留"));
    }
    void searchSectionsAndUnchangedRefresh() {
        MainWindow w;
        w.loadDocument(water());
        openPreview(w);
        auto *search = widget<QLineEdit>(w, "inputPreviewSearch");
        search->setText("TASK SCF");
        widget<QPushButton>(w, "inputPreviewNext")->click();
        QCOMPARE(text(w)->textCursor().selectedText(), "task scf");
        const int position = text(w)->textCursor().position();
        panel(w)->refresh();
        QCOMPARE(text(w)->textCursor().position(), position);
        QCOMPARE(text(w)->textCursor().selectedText(), "task scf");
        widget<QPushButton>(w, "inputPreviewNext")->click(); // Wraps at EOF.
        QCOMPARE(text(w)->textCursor().position(), position);
        widget<QPushButton>(w, "inputPreviewPrevious")->click();
        QCOMPARE(text(w)->textCursor().position(), position);
        search->setText("absent-word");
        widget<QPushButton>(w, "inputPreviewNext")->click();
        QVERIFY(widget<QLabel>(w, "inputPreviewSearchStatus")->text().contains("ありません"));
        QCOMPARE(text(w)->textCursor().position(), position);
        auto *outline = widget<QListWidget>(w, "inputPreviewOutline");
        int geometry = -1;
        for (int i = 0; i < outline->count(); ++i)
            if (outline->item(i)->toolTip().startsWith("geometry"))
                geometry = i;
        QVERIFY(geometry >= 0);
        QCOMPARE(outline->item(geometry)->toolTip(), QString("geometry（3 原子）"));
        outline->setCurrentRow(geometry);
        QCOMPARE(text(w)->textCursor().blockNumber(), outline->item(geometry)->data(Qt::UserRole).toInt());
        QVERIFY(text(w)->textCursor().block().text().startsWith("geometry"));
        QCOMPARE(widget<QLabel>(w, "inputPreviewPosition")->text(),
                 QString("行 %1 / 列 1").arg(text(w)->textCursor().blockNumber() + 1));
    }
    void outlineMarksAdvancedAndExtraLines() {
        auto d = water();
        set(d, "method", "dft");
        set(d, "task", "optimize");
        set(d, "extra_input", "set cphf:maxiter 50");
        setNwchemAdvanced(d, "dft_grid_nodisk", true);
        setNwchemAdvanced(d, "driver_trust", 0.2);
        setNwchemAdvanced(d, "scratch_dir", "scratch");
        setNwchemAdvanced(d, "freq_animate", true); // Kept for frequency tasks: no line.
        const auto input = nwchemInput(d);
        const auto lines = input.split('\n');
        const auto outline = nwchemInputOutline(d, input);
        // The lines that the settings of "4 詳細設定" and the additional input add or change.
        QStringList advanced, extra;
        for (const int line : outline.advanced)
            advanced << lines[line].trimmed();
        for (const int line : outline.extra)
            extra << lines[line].trimmed();
        advanced.sort();
        extra.sort();
        QCOMPARE(advanced, (QStringList{"grid fine nodisk", "scratch_dir scratch", "trust 0.2"}));
        QCOMPARE(extra, (QStringList{"# Additional input (not checked by MolForma)", "set cphf:maxiter 50"}));
        QHash<QString, NwchemInputBlock> blocks;
        for (const auto &b : outline.blocks)
            blocks.insert(b.keyword, b);
        QCOMPARE(blocks["geometry"].title, QString("geometry（3 原子）"));
        QCOMPARE(blocks["geometry"].tabs, QList<int>{2});
        QCOMPARE(blocks["geometry"].lines, 5);
        QCOMPARE(blocks["basis"].title, QString("basis \"sto-3g\""));
        QCOMPARE(blocks["dft"].tabs, (QList<int>{3, 4}));
        QCOMPARE(blocks["dft"].setting, QString("method"));
        QCOMPARE(blocks["dft"].group, QString("dft"));
        QCOMPARE(lines[blocks["dft"].line + blocks["dft"].lines - 1], QString("end"));
        QCOMPARE(blocks["driver"].group, QString("driver"));
        QCOMPARE(blocks["set"].tabs, QList<int>{3});
        QCOMPARE(blocks["set"].setting, QString("extra_input"));
        QCOMPARE(blocks["task"].tabs, QList<int>{3});
        QCOMPARE(lines[blocks["task"].line], QString("task dft optimize"));
        // A block that only "4 詳細設定" writes belongs to it alone.
        setNwchemAdvanced(d, "molden_output", true);
        const auto molden = nwchemInput(d);
        const auto withMolden = nwchemInputOutline(d, molden);
        const auto property = std::find_if(withMolden.blocks.begin(), withMolden.blocks.end(),
                                           [](const auto &b) { return b.keyword == "property"; });
        QVERIFY(property != withMolden.blocks.end());
        QCOMPARE(property->tabs, QList<int>{4});
        QCOMPARE(property->group, QString("visualization"));
        // Without such settings nothing is marked; an empty input has no outline.
        const auto plain = water();
        const auto none = nwchemInputOutline(plain, nwchemInput(plain));
        QVERIFY(none.advanced.isEmpty() && none.extra.isEmpty() && !none.blocks.isEmpty());
        QVERIFY(nwchemInputOutline(plain, {}).blocks.isEmpty());
    }
    void blockSourcesLeadToTheirTabs() {
        auto d = water();
        set(d, "method", "dft");
        setNwchemAdvanced(d, "dft_grid_nodisk", true);
        MainWindow w;
        w.loadDocument(d);
        openPreview(w);
        auto *tabs = widget<QTabWidget>(w, "workspaceTabs");
        auto *outline = widget<QListWidget>(w, "inputPreviewOutline");
        int dft = -1;
        for (int i = 0; i < outline->count(); ++i)
            if (outline->item(i)->toolTip() == "dft")
                dft = i;
        QVERIFY(dft >= 0);
        const int line = outline->item(dft)->data(Qt::UserRole).toInt() + 1;
        // The block at the cursor names the tabs that set it.
        outline->setCurrentRow(dft);
        QVERIFY(widget<QFrame>(w, "inputPreviewSource")->isVisibleTo(&w));
        QCOMPARE(widget<QLabel>(w, "inputPreviewSourceTitle")->text(), QString("dft ブロックの編集元"));
        QStringList links;
        for (auto *link : widget<QFrame>(w, "inputPreviewSource")->findChildren<QPushButton *>())
            links << link->text();
        QCOMPARE(links, (QStringList{"3 計算条件 › 基本設定", "4 詳細設定 › DFT"}));
        // Its numbers in the outline lead there: "4 詳細設定" shows the category of the block.
        auto *advanced = w.findChild<QToolButton *>(QString("inputPreviewSource%1_4").arg(line));
        QVERIFY(advanced);
        advanced->click();
        QCOMPARE(tabs->currentIndex(), 3);
        QCOMPARE(widget<QComboBox>(w, "advancedCategory")->currentData().toString(), QString("dft"));
        openPreview(w);
        w.findChild<QToolButton *>(QString("inputPreviewSource%1_3").arg(line))->click();
        QCOMPARE(tabs->currentIndex(), 2);
        // The geometry is set in "2 座標".
        openPreview(w);
        QVERIFY(w.findChild<QToolButton *>("inputPreviewSource7_2"));
        QVERIFY(!w.findChild<QToolButton *>("inputPreviewSource7_3"));
        QCOMPARE(widget<QLabel>(w, "inputPreviewStats")->text(),
                 QString("%1 行 · UTF-8 · %2 バイト")
                     .arg(nwchemInput(d).count('\n'))
                     .arg(QLocale().toString(nwchemInput(d).toUtf8().size())));
    }
    void focusedSettingAndSaveDialog() {
        MainWindow w;
        w.loadDocument(water());
        auto *tabs = widget<QTabWidget>(w, "workspaceTabs");
        tabs->setCurrentIndex(2);
        w.show();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        auto *title = widget<QLineEdit>(w, "nwchem_title");
        title->setFocus();
        title->selectAll();
        QTest::keyClicks(title, "preview export title");
        panel(w)->saveInput(files.filePath("focused.nw"));
        QVERIFY(readFile(files.filePath("focused.nw")).contains("title \"preview export title\""));
        openPreview(w);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        const auto path = files.filePath("dialog-input");
        QTimer::singleShot(0, &w, [&] {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->selectFile(path);
            QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
        });
        widget<QPushButton>(w, "inputPreviewSave")->click();
        QCOMPARE(readFile(path + ".nw"), text(w)->toPlainText().toUtf8());
        const auto artifacts = qEnvironmentVariable("MOLFORMA_PREVIEW_ARTIFACTS");
        if (!artifacts.isEmpty()) {
            QVERIFY(QDir().mkpath(artifacts));
            w.resize(1380, 900);
            QCoreApplication::processEvents();
            QVERIFY(w.grab().save(QDir(artifacts).filePath("input-preview.png")));
            panel(w)->saveInput(QDir(artifacts).filePath("water.nw"));
        }
    }
};
QTEST_MAIN(InputPreviewTest)
#include "test_input_preview.moc"
