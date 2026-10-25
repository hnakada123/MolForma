#include "ui/advanced_panel.h"
#include "ui/main_window.h"
#include <QtTest>
#include <QtWidgets>
using namespace mf;

class AdvancedTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    static Document water() {
        return Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
    }
    static void base(Document &d, const QString &key, const QJsonValue &value) {
        auto s = d.data["nwchem"].toObject();
        s[key] = value;
        d.data["nwchem"] = s;
    }
    static const AdvancedOption &option(const QString &key) {
        for (const auto &o : nwchemAdvancedOptions())
            if (o.key == key)
                return o;
        qFatal("Unknown option");
    }
    static void values(Document &d, const QJsonObject &a) {
        for (auto it = a.begin(); it != a.end(); ++it)
            setNwchemAdvanced(d, it.key(), it.value());
    }
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
    // The table row of an option (or nullptr), and whether filtering hides it.
    static QTreeWidgetItem *row(MainWindow &w, const QString &key) {
        auto *table = w.findChild<QTreeWidget *>("advancedTable");
        for (auto *item : table->findItems("*", Qt::MatchWildcard | Qt::MatchRecursive))
            if (item->data(0, Qt::UserRole).toString() == key)
                return item;
        return nullptr;
    }
    static bool hidden(MainWindow &w, const QString &key) {
        auto *item = row(w, key);
        return item->isHidden() || item->parent()->isHidden();
    }
    static bool errors(const Document &d, const QString &text) {
        return validateNwchem(d).errors.join('\n').contains(text);
    }
  private slots:
    void initTestCase() {
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
    }
    void defaultsAndParsing() {
        auto d = water();
        const auto original = d.toJson();
        const auto text = nwchemInput(d);
        resetNwchemAdvanced(d);
        QCOMPARE(d.toJson(), original);
        QSet<QString> keys;
        for (const auto &o : nwchemAdvancedOptions()) {
            QVERIFY(!keys.contains(o.key));
            keys.insert(o.key);
            QVERIFY(!o.help.isEmpty());
            QVERIFY(!o.keyword.isEmpty());
        }
        QCOMPARE(parseNwchemAdvanced(option("scf_thresh"), " 1D-8 ").toDouble(), 1e-8);
        QCOMPARE(parseNwchemAdvanced(option("corr_freeze"), "0").toInt(), 0);
        QVERIFY(parseNwchemAdvanced(option("scf_thresh"), " ").isUndefined());
        for (const auto &bad : {"nan", "inf", "1e999", "1,2", "1e-8 task scf"}) {
            auto v = parseNwchemAdvanced(option("scf_thresh"), bad);
            QVERIFY(!nwchemAdvancedError(option("scf_thresh"), v).isEmpty());
        }
        const double precise = 0.12345678901234567;
        QCOMPARE(parseNwchemAdvanced(option("driver_trust"), nwchemAdvancedText(precise)).toDouble(),
                 precise);
        QCOMPARE(parseNwchemAdvanced(option("freq_temperatures"), "100 298.15 350").toArray(),
                 QJsonArray({100, 298.15, 350}));
        QVERIFY(!nwchemAdvancedError(option("freq_temperatures"), QJsonArray{0, 298.15}).isEmpty());
        base(d, "advanced", QJsonObject{});
        QCOMPARE(nwchemInput(d), text);
    }
    void hfAndDftHaveDistinctControls() {
        auto d = water();
        values(d, {{"scf_thresh", 1e-8},
                   {"scf_tol2e", 1e-12},
                   {"scf_direct", true},
                   {"scf_solver", "diis"},
                   {"scf_diisbas", 7}});
        auto text = nwchemInput(d);
        QCOMPARE(text.count("  thresh "), 1);
        QVERIFY(text.contains("  thresh 1e-08\n"));
        QVERIFY(text.contains("  direct\n  diis\n  diisbas 7\n"));
        d.setJobValue("charge", 1);
        d.setJobValue("multiplicity", 2);
        QVERIFY(errors(d, "閉殻RHF"));
        base(d, "method", "dft");
        values(d, {{"dft_energy", 1e-8},
                   {"dft_density", 1e-6},
                   {"dft_gradient", 1e-7},
                   {"dft_damp", 30},
                   {"dft_ncydp", 3},
                   {"dft_lshift", 0.25},
                   {"dft_diis", 7},
                   {"dft_direct", true},
                   {"dft_grid_nodisk", true}});
        text = nwchemInput(d);
        QVERIFY(text.contains("  convergence damp 30\n"));
        QVERIFY(text.contains("  convergence ncydp 3\n"));
        QVERIFY(text.contains("  convergence lshift 0.25\n"));
        QVERIFY(text.contains("  convergence diis 7\n"));
        QVERIFY(text.contains("grid fine nodisk\n"));
        QVERIFY(!text.contains("  diis\n"));
        QVERIFY(!text.contains("  tol2e "));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("保留"));
        QCOMPARE(nwchemAdvanced(d)["scf_diisbas"].toInt(), 7);
    }
    void correlatedOrbitalsAndThresholds() {
        auto d = water();
        base(d, "method", "mp2");
        base(d, "freeze_core", true);
        values(d, {{"corr_freeze", 0},
                   {"mp2_tight", true},
                   {"mp2_scratchdisk", 128},
                   {"cc_thresh", 1e-8},
                   {"cc_diisbas", 6},
                   {"cc_nodisk", true}});
        auto text = nwchemInput(d);
        QVERIFY(text.contains("mp2\n  freeze 0\n  tight\n  scratchdisk 128\nend"));
        QCOMPARE(validateNwchem(d).frozenOrbitals, 0);
        QVERIFY(!text.contains("  diisbas 6"));
        setNwchemAdvanced(d, "mp2_scratchdisk", 1000000);
        QVERIFY(nwchemInput(d).contains("  scratchdisk 1000000\n"));
        setNwchemAdvanced(d, "corr_freeze", 5);
        QVERIFY(errors(d, "相関電子が不足"));
        setNwchemAdvanced(d, "corr_freeze", 1.5);
        QVERIFY(errors(d, "整数"));
        setNwchemAdvanced(d, "corr_freeze", 1);
        base(d, "method", "ccsd");
        text = nwchemInput(d);
        QVERIFY(text.contains("ccsd\n  freeze 1\n  maxiter 50\n  thresh 1e-08\n  diisbas 6\n  nodisk\nend"));
        QVERIFY(!text.contains("scratchdisk"));
        QCOMPARE(validateNwchem(d).frozenOrbitals, 1);
        setNwchemAdvanced(d, "corr_freeze", QJsonValue::Undefined);
        QVERIFY(nwchemInput(d).contains("freeze atomic"));
    }
    void optimizationAndVibrations() {
        auto d = water();
        base(d, "task", "optimize_frequencies");
        base(d, "opt_convergence", "tight");
        values(d, {{"driver_gmax", 0.0001},
                   {"driver_grms", 0.00005},
                   {"driver_xmax", 0.001},
                   {"driver_xrms", 0.0005},
                   {"driver_trust", 0.2},
                   {"driver_eprec", 1e-8},
                   {"driver_xyz", true},
                   {"freq_temperatures", QJsonArray{100, 298.15, 350}},
                   {"freq_animate", true},
                   {"hessian_thresh", 1e-8},
                   {"freq_fd_delta", 0.005}});
        const auto before = d.toJson().value("atoms");
        auto text = nwchemInput(d);
        const auto gmax = QRegularExpression("  tight\\n  gmax ([^\\n]+)\\n").match(text);
        QVERIFY(gmax.hasMatch());
        QCOMPARE(gmax.captured(1).toDouble(), 0.0001);
        QVERIFY(text.contains("  xyz molforma_opt\n"));
        QVERIFY(text.contains("  temp 3 100 298.15 350\n"));
        QVERIFY(text.contains("  animate\n  filename molforma_vib\n"));
        QVERIFY(text.contains("hessian\n  thresh 1e-08\nend"));
        QVERIFY(!text.contains("fd_delta"));
        base(d, "numerical_frequencies", true);
        text = nwchemInput(d);
        const auto delta = QRegularExpression("  fd_delta ([^\\n]+)\\n").match(text);
        QVERIFY(delta.hasMatch());
        QCOMPARE(delta.captured(1).toDouble(), 0.005);
        QVERIFY(!text.contains("\nhessian\n"));
        QVERIFY(text.endsWith("task scf optimize\ntask scf frequencies numerical\n"));
        base(d, "task", "energy");
        text = nwchemInput(d);
        QVERIFY(!text.contains("\nfreq\n"));
        QVERIFY(!text.contains("\ndriver\n"));
        QCOMPARE(d.toJson().value("atoms"), before);
    }
    void solventAndFiles() {
        auto d = water();
        base(d, "solvent", "cosmo");
        values(d, {{"cosmo_minbem", 3},
                   {"cosmo_ificos", "1"},
                   {"cosmo_screen", "ideal"},
                   {"cosmo_zeta", 1.0},
                   {"vectors_guess", "atomic"},
                   {"vectors_output", "path with space/output.movecs"},
                   {"scratch_dir", "/remote/scratch_job"},
                   {"permanent_dir", "/remote/permanent"},
                   {"echo", true}});
        auto text = nwchemInput(d);
        QVERIFY(text.contains("  vectors input atomic output \"path with space/output.movecs\"\n"));
        QVERIFY(text.contains("  scratch_dir /remote/scratch_job\n"));
        QVERIFY(text.contains("  permanent_dir /remote/permanent\n"));
        QVERIFY(text.contains("  minbem 3\n  ificos 1\n  screen ideal\n  zeta 1\n"));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("比誘電率"));
        setNwchemAdvanced(d, "vectors_guess", "file");
        QVERIFY(errors(d, "movecsファイルを指定"));
        setNwchemAdvanced(d, "vectors_input", "seed.movecs");
        QVERIFY(nwchemInput(d).contains("vectors input \"seed.movecs\" output"));
        setNwchemAdvanced(d, "vectors_output", QJsonValue::Undefined);
        QVERIFY(errors(d, "別の出力movecs"));
        setNwchemAdvanced(d, "vectors_output", "seed.movecs");
        QVERIFY(errors(d, "別のファイル名"));
        for (const auto &bad : {"a\"; task shell invalid", "a\nb", "a#b", "a\\b"}) {
            setNwchemAdvanced(d, "vectors_output", bad);
            QVERIFY_EXCEPTION_THROWN(nwchemInput(d), std::runtime_error);
        }
        for (const auto &bad : {"/remote/scratch job", "host:/scratch", "scratch'job"})
            QVERIFY(!nwchemAdvancedError(option("scratch_dir"), bad).isEmpty());
    }
    void inactiveAndUnknownDataSurvive() {
        auto d = water();
        d.envelope["future_result"] = QJsonObject{{"id", "result-1"}};
        d.setJobValue("sections", QJsonObject{{"scf", QJsonObject{{"future", 99}}}});
        const auto job = d.job();
        values(d,
               {{"future", QJsonObject{{"option", "keep"}}}, {"dft_damp", "invalid"}, {"scf_thresh", 1e-8}});
        QVERIFY(validateNwchem(d).errors.isEmpty());
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("未対応"));
        QVERIFY(!nwchemInput(d).contains("invalid"));
        base(d, "method", "dft");
        QVERIFY(errors(d, "ダンピング"));
        auto restored = Document::fromProject(d.project());
        QCOMPARE(nwchemAdvanced(restored), nwchemAdvanced(d));
        QCOMPARE(restored.envelope["future_result"], d.envelope["future_result"]);
        resetNwchemAdvanced(d, "scf");
        QVERIFY(nwchemAdvanced(d).contains("dft_damp"));
        resetNwchemAdvanced(d);
        QCOMPARE(nwchemAdvanced(d).size(), 1);
        QCOMPARE(nwchemAdvanced(d)["future"].toObject()["option"].toString(), "keep");
        QCOMPARE(d.job(), job);
        base(d, "advanced", "malformed");
        QVERIFY(errors(d, "オブジェクト"));
    }
    void presetsControlNumericalOverrides() {
        auto d = water();
        values(d, {{"scf_thresh", 1e-9},
                   {"driver_trust", 0.12},
                   {"vectors_output", "keep.movecs"},
                   {"future", QJsonObject{{"keep", true}}}});
        const auto saved = nwchemPresetSettings(nwchemSettings(d));
        QCOMPARE(saved["advanced"].toObject().size(), 2);
        applyNwchemPreset(d, nwchemPresets()[1].settings);
        QVERIFY(!nwchemAdvanced(d).contains("scf_thresh"));
        QCOMPARE(nwchemAdvanced(d)["vectors_output"].toString(), "keep.movecs");
        QVERIFY(nwchemAdvanced(d).contains("future"));
        applyNwchemPreset(d, saved);
        QCOMPARE(nwchemAdvanced(d)["driver_trust"].toDouble(), 0.12);
        QVERIFY(nwchemInput(d).contains("thresh 1e-09"));
        auto old = saved;
        old.remove("advanced");
        applyNwchemPreset(d, old);
        QVERIFY(!nwchemAdvanced(d).contains("scf_thresh"));
    }
    void everyKnownOptionRejectsWrongTypes() {
        auto d = water();
        for (const auto &o : nwchemAdvancedOptions()) {
            const QJsonValue invalid = QJsonObject{{"unexpected", true}};
            QVERIFY2(!nwchemAdvancedError(o, invalid).isEmpty(), qPrintable(o.key));
            if (o.kind == AdvancedKind::Real || o.kind == AdvancedKind::Integer) {
                QVERIFY(!nwchemAdvancedError(o, o.maximum + 1).isEmpty());
                QVERIFY(!nwchemAdvancedError(o, o.minimum - 1).isEmpty());
            }
        }
        values(d, {{"scf_solver", "diis\nend\ntask shell invalid"}});
        QVERIFY_EXCEPTION_THROWN(nwchemInput(d), std::runtime_error);
    }
    void nativeEditingSearchUndoAndCandidates() {
        MainWindow w;
        auto d = water();
        w.loadDocument(d);
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        QCOMPARE(tabs->count(), 8);
        QCOMPARE(tabs->tabText(3), "4 詳細設定");
        tabs->setCurrentIndex(3);
        QCOMPARE(w.document().toJson(), d.toJson());
        // A blank value shows what NWChem uses instead, and where it comes from.
        QCOMPARE(w.findChild<QLineEdit *>("advanced_scf_thresh")->placeholderText(),
                 QString("1e-7（3 計算条件のSCF収束: 厳密）"));
        input(w, "scf_thresh", "2d-8");
        QCOMPARE(nwchemAdvanced(w.document())["scf_thresh"].toDouble(), 2e-8);
        QCOMPARE(w.history()->count(), 1);
        button(w, "inputPreviewCopy")->click();
        QCOMPARE(QApplication::clipboard()->text(), nwchemInput(w.document()));
        w.history()->undo();
        QCOMPARE(w.document().toJson(), d.toJson());
        w.history()->redo();
        input(w, "scf_thresh", "invalid");
        QVERIFY(!button(w, "inputPreviewCopy")->isEnabled());
        QVERIFY(w.findChild<QLabel *>("advancedStatus")->text().contains("エラー"));
        QVERIFY(row(w, "scf_thresh")->text(2).contains("エラー"));
        QVERIFY(row(w, "scf_thresh")->data(0, Qt::UserRole + 3).toBool()); // Marked as changed.
        QVERIFY(row(w, "scf_thresh")->parent()->data(0, Qt::UserRole + 2).toString().contains("変更 1"));
        QVERIFY(w.findChild<QPushButton *>("advancedResetField_scf_thresh")->isVisibleTo(&w));
        QVERIFY(!w.findChild<QPushButton *>("advancedResetField_scf_tol2e")->isVisibleTo(&w));
        button(w, "advancedResetField_scf_thresh")->click();
        QTRY_VERIFY(button(w, "inputPreviewCopy")->isEnabled());
        QVERIFY(w.findChild<QLabel *>("advancedStatus")->text().contains("問題はありません"));
        auto *filter = w.findChild<QComboBox *>("advancedFilter");
        QCOMPARE(w.findChild<QToolButton *>("advancedFilter_applicable")->text(),
                 QString("この計算で使う 11"));
        w.findChild<QToolButton *>("advancedFilter_all")->click();
        QCOMPARE(filter->currentData().toString(), "all");
        auto *search = w.findChild<QLineEdit *>("advancedSearch");
        search->setText("damp");
        QVERIFY(!hidden(w, "dft_damp"));
        QVERIFY(hidden(w, "scf_thresh"));
        QVERIFY(row(w, "dft_damp")->parent()->text(0).contains("DFT"));
        QVERIFY(!w.findChild<QLineEdit *>("advanced_dft_damp")->isEnabled());
        search->clear();
        // The categories follow the filter: the DFT options do not take part in an HF calculation.
        auto *categories = w.findChild<QListWidget *>("advancedCategories");
        filter->setCurrentIndex(filter->findData("applicable"));
        for (int i = 0; i < categories->count(); ++i)
            if (categories->item(i)->data(Qt::UserRole) == "dft")
                QCOMPARE(categories->item(i)->data(Qt::UserRole + 12).toString(), QString("使わない"));
        input(w, "scf_thresh", "1e-8");
        input(w, "scf_tol2e", "1e-12");
        const auto beforeReset = w.document().toJson();
        button(w, "advancedReset_scf")->click();
        QVERIFY(nwchemAdvanced(w.document()).isEmpty());
        w.history()->undo();
        QCOMPARE(w.document().toJson(), beforeReset);
        filter->setCurrentIndex(filter->findData("changed"));
        QVERIFY(hidden(w, "dft_damp"));
        QVERIFY(!hidden(w, "scf_thresh"));
        // The selected row at the right: its values, scope and line in the input.
        w.findChild<QTreeWidget *>("advancedTable")->setCurrentItem(row(w, "scf_thresh"));
        QCOMPARE(w.findChild<QLabel *>("advancedCurrent")->text(), QString("1e-08"));
        QCOMPARE(w.findChild<QLabel *>("advancedScope")->text(), QString("HF・MP2・CCSD（参照SCF）"));
        const auto snippet = w.findChild<QPlainTextEdit *>("advancedSnippet")->toPlainText();
        QVERIFY2(snippet.startsWith("scf\n") && snippet.contains("  thresh 1e-08") && snippet.endsWith("end"),
                 qPrintable(snippet));
        button(w, "advancedShowInInput")->click();
        QCOMPARE(tabs->currentIndex(), 5);
        QCOMPARE(w.findChild<QPlainTextEdit *>("inputPreviewText")->textCursor().block().text(),
                 QString("  thresh 1e-08"));
        tabs->setCurrentIndex(3);
        // A diagnostic naming an option selects its row (showing every option if it is pending)
        // and the description at the right follows the selected row.
        filter->setCurrentIndex(filter->findData("applicable"));
        auto *diagnostics = w.findChild<QListWidget *>("advancedValidation");
        QMetaObject::invokeMethod(diagnostics, "messageClicked", Q_ARG(QString, "「密度のダンピング / %」"),
                                  Q_ARG(bool, true));
        QCOMPARE(w.findChild<QTreeWidget *>("advancedTable")->currentItem(), row(w, "dft_damp"));
        QCOMPARE(filter->currentData().toString(), "all");
        QCOMPARE(w.findChild<QLabel *>("advancedKeyword")->text(), QString("dft › convergence damp"));
        QVERIFY(w.findChild<QLabel *>("advancedScope")->text().contains("今の計算では使いません"));
        w.edit("candidate", [](Document &next) { next.atoms[1].position.x() += 0.1; }, true);
        QVERIFY(!w.findChild<QLineEdit *>("advanced_scf_thresh")->isEnabled());
        QVERIFY(!button(w, "advancedResetAll")->isEnabled());
        QVERIFY(!button(w, "inputPreviewSave")->isEnabled());
        QVERIFY(w.findChild<QLabel *>("advancedMessage")->text().contains("候補"));
        w.cancelCandidate();
        QVERIFY(button(w, "inputPreviewSave")->isEnabled());
    }
    void effectiveDefaultsAndScopes() {
        auto d = water();
        const auto text = [&](const QString &key) { return nwchemAdvancedDefault(option(key), d); };
        QCOMPARE(text("dft_energy"), QString("1e-7（3 計算条件のSCF収束: 厳密）"));
        QCOMPARE(text("driver_gmax"), QString("0.00045（3 計算条件の最適化収束: 標準）"));
        QCOMPARE(text("driver_trust"), QString("0.3（NWChem既定）"));
        QCOMPARE(text("cube_divisions"), QString("40")); // MolForma's own default.
        QCOMPARE(text("corr_freeze"), QString("0（凍結なし）"));
        QCOMPARE(text("freq_fd_delta"), QString("0.001"));
        base(d, "scf_convergence", "default");
        base(d, "opt_convergence", "tight");
        base(d, "freeze_core", true);
        base(d, "method", "dft");
        QCOMPARE(text("scf_thresh"), QString("NWChem既定（3 計算条件のSCF収束: NWChem既定）"));
        QCOMPARE(text("driver_xrms"), QString("0.00004（3 計算条件の最適化収束: 厳密）"));
        QCOMPARE(text("corr_freeze"), QString("atomic（3 計算条件の内殻凍結）"));
        QCOMPARE(text("freq_fd_delta"), QString("0.010（DFT）"));
        base(d, "opt_convergence", "loose");
        QCOMPARE(text("driver_xmax"), QString("0.018（3 計算条件の最適化収束: 緩い）"));
        // Where an option goes, and when it takes part.
        QCOMPARE(nwchemAdvancedBlock(option("dft_damp"), d), QString("dft"));
        QCOMPARE(nwchemAdvancedBlock(option("vectors_guess"), d), QString("dft"));
        QCOMPARE(nwchemAdvancedBlock(option("scratch_dir"), d), QString("scratch_dir"));
        QCOMPARE(nwchemAdvancedBlock(option("hessian_thresh"), d), QString("hessian"));
        QCOMPARE(nwchemAdvancedBlock(option("molden_output"), d), QString("property"));
        QCOMPARE(nwchemAdvancedBlock(option("driver_cartesian"), d), QString("geometry"));
        QCOMPARE(nwchemAdvancedScope(option("cc_thresh")), QString("CCSD・CCSD(T)"));
        QCOMPARE(nwchemAdvancedScope(option("freq_fd_delta")), QString("数値Hessianを使う振動数"));
        for (const auto &o : nwchemAdvancedOptions()) {
            QVERIFY(!nwchemAdvancedScope(o).isEmpty());
            QVERIFY(!nwchemAdvancedDefault(o, d).isEmpty());
        }
    }
    void saveShortcutAndFocusedTeardown() {
        QTemporaryDir files;
        const auto path = files.filePath("advanced.kgui.json");
        water().save(path);
        MainWindow w;
        w.loadPath(path);
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(3);
        w.show();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
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
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(3);
        w.show();
        QCoreApplication::processEvents();
        QVERIFY(w.grab().save(path + "/advanced.png"));
    }
};
QTEST_MAIN(AdvancedTest)
#include "test_advanced.moc"
