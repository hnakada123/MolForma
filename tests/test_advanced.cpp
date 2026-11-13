// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/files.h"
#include "io/document_storage.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem_support.h"
#include "ui/calculation_panel.h"
#include "ui/diagnostics_list.h"
#include "ui/main_window.h"
#include "ui/settings_widgets.h"
#include <QtTest>
#include <QtWidgets>
using namespace mf;

class AdvancedTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    static QPushButton *button(MainWindow &w, const char *name) {
        return w.findChild<QPushButton *>(name);
    }
    static void input(MainWindow &w, const char *key, const QString &text) {
        auto *line = w.findChild<QLineEdit *>(QString("advanced_") + key);
        QVERIFY(line);
        line->setText(text);
        line->setModified(true);
        QMetaObject::invokeMethod(line, "editingFinished");
    }
  private slots:
    void initTestCase() {
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
    }
    void detailsInTheSections() {
        MainWindow w;
        auto d = water();
        w.loadDocument(d);
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        auto *calculation = w.findChild<CalculationPanel *>("calculationTab");
        for (int i = 0; i < tabs->count(); ++i)
            QVERIFY(!tabs->tabText(i).contains("詳細設定")); // No tab of their own.
        tabs->setCurrentWidget(calculation);
        QCOMPARE(w.document().toJson(), d.toJson());
        // The details of a section behind a toggle with their count; those of the files open.
        auto *toggle = w.findChild<QToolButton *>("calculationDetails_convergence");
        auto *body = w.findChild<QWidget *>("calculationDetailBody_convergence");
        // The count after the name, in the disclosure's muted text.
        const auto count = [toggle] { return toggle->parentWidget()->findChild<QLabel *>()->text(); };
        QCOMPARE(toggle->text(), QString("詳細設定"));
        QCOMPARE(count(), QString("21 項目"));
        QVERIFY(!body->isVisibleTo(&w));
        QVERIFY(w.findChild<QWidget *>("calculationDetailBody_orbitals")->isVisibleTo(&w));
        toggle->click();
        QVERIFY(body->isVisibleTo(&w));
        // A blank value shows what NWChem uses instead, and where it comes from.
        QVERIFY(w.findChild<QLineEdit *>("advanced_scf_thresh")->placeholderText().startsWith("1e-7（"));
        input(w, "scf_thresh", "2d-8");
        QCOMPARE(nwchemAdvanced(w.document())["scf_thresh"].toDouble(), 2e-8);
        QCOMPARE(w.history()->count(), 1);
        QCOMPARE(count(), QString("21 項目 · 変更 1"));
        QCOMPARE(w.findChild<QLabel *>("calculationDetailState_scf_thresh")->text(), QString("適用"));
        auto *sections = static_cast<NavList *>(w.findChild<QListWidget *>("calculationSections"));
        QCOMPARE(sections->entry("convergence")->data(NavList::BadgeRole).toString(), QString("変更 1"));
        button(w, "inputPreviewCopy")->click();
        QCOMPARE(QApplication::clipboard()->text(), nwchemInput(w.document()));
        w.history()->undo();
        QCOMPARE(w.document().toJson(), d.toJson());
        w.history()->redo();
        input(w, "scf_thresh", "invalid");
        QVERIFY(!button(w, "inputPreviewCopy")->isEnabled());
        QVERIFY(w.findChild<QLabel *>("calculationStatus")->text().contains("エラー"));
        QCOMPARE(w.findChild<QLabel *>("calculationDetailState_scf_thresh")->text(), QString("エラー"));
        QVERIFY(button(w, "advancedResetField_scf_thresh")->isVisibleTo(&w));
        QVERIFY(!button(w, "advancedResetField_scf_tol2e")->isVisibleTo(&w));
        button(w, "advancedResetField_scf_thresh")->click();
        QTRY_VERIFY(button(w, "inputPreviewCopy")->isEnabled());
        // What the calculation does not use is shown on request: HF takes no DFT options.
        auto *damp = w.findChild<QWidget *>("calculationDetailRow_dft_damp");
        auto *unused = button(w, "calculationDetailUnused_convergence");
        QVERIFY(!damp->isVisibleTo(&w));
        QCOMPARE(unused->text(), QString("この計算で使わない項目も表示（17）"));
        unused->click();
        QVERIFY(damp->isVisibleTo(&w));
        QVERIFY(!w.findChild<QLineEdit *>("advanced_dft_damp")->isEnabled());
        unused->click();
        QVERIFY(!damp->isVisibleTo(&w));
        // Search finds an option by its NWChem keyword, and shows it although it is not used.
        auto *search = w.findChild<QLineEdit *>("calculationSearch");
        search->setText("damp");
        QMetaObject::invokeMethod(search, "returnPressed");
        QVERIFY(damp->isVisibleTo(&w));
        QVERIFY(w.findChild<QWidget *>("calculationDetailCard")->isVisibleTo(&w));
        QCOMPARE(w.findChild<QLabel *>("calculationDetailTitle")->text(), option("dft_damp").label);
        QCOMPARE(w.findChild<QLabel *>("calculationDetailKeyword")->text(),
                 QString("dft › convergence damp"));
        QVERIFY(w.findChild<QLabel *>("calculationDetailScope")->text().contains("今の計算では使いません"));
        search->clear();
        // The reset of a section covers its details.
        input(w, "scf_thresh", "1e-8");
        input(w, "scf_tol2e", "1e-12");
        const auto beforeReset = w.document().toJson();
        button(w, "calculationResetSection_convergence")->click();
        QVERIFY(nwchemAdvanced(w.document()).isEmpty());
        QCOMPARE(w.history()->undoText(), QString("「SCF・電子相関」を既定に戻す"));
        w.history()->undo();
        QCOMPARE(w.document().toJson(), beforeReset);
        // Changed only: the changed details, also in the list of changes with their own resets.
        auto *filter = w.findChild<QComboBox *>("calculationChangeFilter");
        filter->setCurrentIndex(filter->findData("changed"));
        QVERIFY(!damp->isVisibleTo(&w));
        QVERIFY(w.findChild<QWidget *>("calculationDetailRow_scf_thresh")->isVisibleTo(&w));
        QVERIFY(!w.findChild<QWidget *>("calculationDetailRow_scf_direct")->isVisibleTo(&w));
        QCOMPARE(w.findChild<QToolButton *>("calculationChangeFilter_changed")->text(),
                 QString("変更済み 2"));
        auto *changes = w.findChild<QTreeWidget *>("calculationChanges");
        QCOMPARE(changes->topLevelItemCount(), 2);
        QCOMPARE(changes->topLevelItem(0)->data(0, Qt::UserRole).toString(), QString("advanced:scf_thresh"));
        QVERIFY(w.findChild<QToolButton *>("calculationReset_advanced_scf_tol2e"));
        filter->setCurrentIndex(filter->findData("all"));
        // The chosen detail at the right: its values, scope and line in the input.
        calculation->showField("advanced:scf_thresh");
        QCOMPARE(w.findChild<QLabel *>("calculationDetailCurrent")->text(), QString("1e-08"));
        QCOMPARE(w.findChild<QLabel *>("calculationDetailScope")->text(),
                 QString("HF・MP2・CCSD（参照SCF）"));
        const auto snippet = w.findChild<QPlainTextEdit *>("calculationSnippet")->toPlainText();
        QVERIFY2(snippet.startsWith("scf\n") && snippet.contains("  thresh 1e-08") && snippet.endsWith("end"),
                 qPrintable(snippet));
        button(w, "calculationShowInInput")->click();
        QCOMPARE(tabs->currentWidget()->objectName(), QString("inputPreviewTab"));
        QCOMPARE(w.findChild<QPlainTextEdit *>("inputPreviewText")->textCursor().block().text(),
                 QString("  thresh 1e-08"));
        tabs->setCurrentWidget(calculation);
        // Routing uses the diagnostic's target, independently of its display text.
        auto invalid = w.document();
        auto settings = nwchemSettings(invalid);
        settings["method"] = "dft";
        invalid.data["nwchem"] = settings;
        setNwchemAdvanced(invalid, "dft_damp", 200);
        const auto report = validateNwchem(invalid);
        auto *diagnostics = w.findChild<DiagnosticsList *>("calculationValidation");
        diagnostics->setDiagnostics(report.diagnostics);
        QListWidgetItem *damping = nullptr;
        for (int i = 0; i < diagnostics->count(); ++i)
            if (diagnostics->item(i)->data(DiagnosticsList::SettingRole) == "advanced:dft_damp")
                damping = diagnostics->item(i);
        QVERIFY(damping);
        damping->setText("Display wording is irrelevant");
        emit diagnostics->itemClicked(damping);
        QCOMPARE(w.findChild<QLabel *>("calculationDetailTitle")->text(), option("dft_damp").label);
        QVERIFY(damp->isVisibleTo(&w));
        w.edit("candidate", [](Document &next) { next.atoms[1].position.x() += 0.1; }, true);
        QVERIFY(!w.findChild<QLineEdit *>("advanced_scf_thresh")->isEnabled());
        QVERIFY(!button(w, "calculationResetAll")->isEnabled());
        QVERIFY(!button(w, "inputPreviewSave")->isEnabled());
        QVERIFY(w.findChild<QLabel *>("calculationMessage")->text().contains("候補"));
        w.cancelCandidate();
        QVERIFY(button(w, "inputPreviewSave")->isEnabled());
        // "すべて既定へ" resets the details too, in one step.
        button(w, "calculationResetAll")->click();
        QVERIFY(nwchemAdvanced(w.document()).isEmpty());
        w.history()->undo();
        QCOMPARE(w.document().toJson(), beforeReset);
    }
    void saveShortcutAndFocusedTeardown() {
        QTemporaryDir files;
        const auto path = files.filePath("advanced.molforma.json");
        saveProject(water(), path);
        MainWindow w;
        w.loadPath(path);
        w.findChild<QTabWidget *>("workspaceTabs")
            ->setCurrentWidget(w.findChild<QWidget *>("calculationTab"));
        w.show();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        w.findChild<QToolButton *>("calculationDetails_convergence")->click();
        auto *line = w.findChild<QLineEdit *>("advanced_scf_thresh");
        line->setFocus();
        QTRY_VERIFY(line->hasFocus());
        QTest::keyClicks(line, "1e-8");
        QTest::keyClick(line, Qt::Key_S, Qt::ControlModifier);
        QTRY_COMPARE(nwchemAdvanced(w.document())["scf_thresh"].toDouble(), 1e-8);
        const auto restored = Document::fromProject(QJsonDocument::fromJson(readFile(path)).object());
        QCOMPARE(nwchemAdvanced(restored)["scf_thresh"].toDouble(), 1e-8);
        QVERIFY(w.history()->isClean());
        // A pending edit must not access the document during QWidget teardown.
        line->setFocus();
        QTest::keyClicks(line, "pending");
    }
    void engineArtifacts() {
        const auto path = qEnvironmentVariable("MOLFORMA_ADVANCED_ARTIFACTS");
        if (path.isEmpty())
            return;
        QVERIFY(QDir().mkpath(path));
        auto emitInput = [&](const QString &name, Document d) {
            base(d, "start", name);
            base(d, "memory_mb", 256);
            writeFile(path + '/' + name + ".nw", nwchemInput(d).toUtf8());
        };
        auto hf = water();
        values(hf, {{"scf_thresh", 1e-8},
                    {"scf_tol2e", 1e-12},
                    {"scf_direct", true},
                    {"scf_solver", "diis"},
                    {"scf_diisbas", 7}});
        emitInput("hf_details", hf);
        auto dft = water();
        base(dft, "method", "dft");
        values(dft, {{"dft_energy", 1e-8},
                     {"dft_density", 1e-7},
                     {"dft_gradient", 1e-6},
                     {"dft_damp", 25},
                     {"dft_ncydp", 3},
                     {"dft_lshift", 0.2},
                     {"dft_diis", 7},
                     {"dft_direct", true},
                     {"dft_grid_nodisk", true},
                     {"vectors_guess", "atomic"}});
        emitInput("dft_details", dft);
        auto mp2 = water();
        base(mp2, "method", "mp2");
        values(mp2, {{"corr_freeze", 1}, {"mp2_tight", true}, {"mp2_scratchdisk", 128}});
        emitInput("mp2_details", mp2);
        auto cc = water();
        base(cc, "method", "ccsd");
        base(cc, "basis", "6-31g");
        values(cc, {{"corr_freeze", 1},
                    {"cc_thresh", 1e-8},
                    {"cc_tol2e", 1e-13},
                    {"cc_diisbas", 6},
                    {"cc_nodisk", true}});
        emitInput("cc_details", cc);
        auto opt = Document::fromXyz("2\nhydrogen\nH 0 0 0\nH 0 0 0.78\n");
        base(opt, "task", "optimize_frequencies");
        values(opt, {{"driver_gmax", 0.0001},
                     {"driver_grms", 0.00005},
                     {"driver_xmax", 0.001},
                     {"driver_xrms", 0.0005},
                     {"driver_trust", 0.2},
                     {"driver_eprec", 1e-8},
                     {"driver_xyz", true},
                     {"hessian_thresh", 1e-8},
                     {"freq_animate", true},
                     {"freq_temperatures", QJsonArray{100, 298.15, 350}}});
        emitInput("opt_details", opt);
        auto numeric = opt;
        base(numeric, "task", "frequencies");
        base(numeric, "method", "mp2");
        setNwchemAdvanced(numeric, "freq_animate", QJsonValue::Undefined);
        setNwchemAdvanced(numeric, "freq_fd_delta", 0.003);
        emitInput("numeric_details", numeric);
        auto cosmo = water();
        base(cosmo, "method", "dft");
        base(cosmo, "solvent", "cosmo");
        values(cosmo,
               {{"cosmo_minbem", 2}, {"cosmo_ificos", "1"}, {"cosmo_screen", "ks"}, {"cosmo_zeta", 1.0}});
        emitInput("cosmo_details", cosmo);
        QVERIFY(QDir().mkpath(path + "/scratch_files"));
        QVERIFY(QDir().mkpath(path + "/persistent_files"));
        auto files = water();
        values(files, {{"vectors_guess", "hcore"},
                       {"vectors_output", path + "/seed orbitals.movecs"},
                       {"scratch_dir", path + "/scratch_files"},
                       {"permanent_dir", path + "/persistent_files"},
                       {"echo", true}});
        emitInput("files_details", files);
        auto vectors = water();
        values(vectors, {{"vectors_guess", "file"},
                         {"vectors_input", path + "/seed orbitals.movecs"},
                         {"vectors_output", path + "/new orbitals.movecs"}});
        emitInput("vectors_details", vectors);
        MainWindow w;
        w.loadDocument(dft);
        w.resize(1380, 950);
        w.findChild<CalculationPanel *>("calculationTab")->showDetailGroup("dft");
        w.show();
        QCoreApplication::processEvents();
        QVERIFY(w.grab().save(path + "/advanced.png"));
    }
};
QTEST_MAIN(AdvancedTest)
#include "test_advanced.moc"
