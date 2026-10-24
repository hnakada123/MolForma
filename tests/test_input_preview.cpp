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
        d.envelope["execution"] = QJsonObject{{"kagami_exe", "preserved"}};
        d.envelope["result_reference"] = QJsonObject{{"job_dir", "old-result"}};
        d.data["future_field"] = QJsonObject{{"value", 42}};
        MainWindow w;
        w.loadDocument(d);
        const auto before = w.document().project();
        auto *tabs = widget<QTabWidget>(w, "workspaceTabs");
        QCOMPARE(tabs->count(), 8);
        QCOMPARE(tabs->tabText(5), "6 入力プレビュー");
        widget<QPushButton>(w, "executionPreview")->click();
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
        widget<QPushButton>(w, "inputPreviewGo5")->click();
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
        auto *sections = widget<QComboBox>(w, "inputPreviewSections");
        int geometry = -1;
        for (int i = 0; i < sections->count(); ++i)
            if (sections->itemText(i).contains("geometry"))
                geometry = i;
        QVERIFY(geometry >= 0);
        sections->setCurrentIndex(geometry);
        QVERIFY(QMetaObject::invokeMethod(sections, "activated", Q_ARG(int, geometry)));
        QCOMPARE(text(w)->textCursor().blockNumber(), sections->currentData().toInt());
        QVERIFY(text(w)->textCursor().block().text().startsWith("geometry"));
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
