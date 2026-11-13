// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/operations.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "io/document_storage.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_import.h"
#include "nwchem/nwchem_plan.h"
#include "nwchem_support.h"
#include "ui/calculation_panel.h"
#include "ui/main_window.h"
#include <QListWidget>
#include <QtTest>
#include <QtWidgets>
#include <algorithm>
#include <cmath>
using namespace mf;

class CalculationTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDir;
    static QComboBox *combo(MainWindow &w, const char *key) {
        return w.findChild<QComboBox *>(QString("nwchem_") + key);
    }
    static void choose(MainWindow &w, const char *key, const char *value) {
        auto *c = combo(w, key);
        QVERIFY(c);
        QVERIFY(c->findData(value) >= 0);
        c->setCurrentIndex(c->findData(value));
    }
    static QPushButton *button(MainWindow &w, const char *name) {
        return w.findChild<QPushButton *>(name);
    }
    static QAction *action(MainWindow &w, const QString &name) {
        return w.findChild<QAction *>(name);
    }
    static int userPresets(MainWindow &w) {
        int count = 0;
        for (const auto *a : w.findChildren<QAction *>())
            count += a->objectName().startsWith("calculationUserPreset");
        return count;
    }
    // The rows of "既定から変更した条件" as "label: value".
    static QStringList changes(MainWindow &w) {
        QStringList rows;
        auto *tree = w.findChild<QTreeWidget *>("calculationChanges");
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            rows << tree->topLevelItem(i)->text(0) + ": " + tree->topLevelItem(i)->text(1);
        return rows;
    }

  private slots:
    void initTestCase() {
        QVERIFY(settingsDir.isValid());
        qputenv("XDG_CONFIG_HOME", settingsDir.path().toUtf8());
        qputenv("XDG_DATA_HOME", settingsDir.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settingsDir.path()); // MolForma uses NativeFormat.
    }
    void sharedSettingRulesAndInvalidToggleRecovery() {
        auto d = water();
        d.envelope["future_envelope"] = QJsonObject{{"keep", 42}};
        set(d, "future_setting", QJsonObject{{"keep", true}});
        const auto envelope = d.envelope;
        setNwchemSetting(d, "scf_maxiter", 10001);
        const auto check = validateNwchem(d);
        QVERIFY(std::any_of(check.diagnostics.begin(), check.diagnostics.end(), [](const auto &item) {
            return item.code == "settings.integer" && item.setting == "scf_maxiter";
        }));
        setNwchemSetting(d, "scf_maxiter", 100);
        setNwchemSetting(d, "ri_j", true); // HF does not use it; the user must still be able to clear it.
        const auto stored = nwchemSettings(d);
        const auto fitting = nwchemSettingSpec("ri_j");
        QVERIFY(!nwchemSettingApplies(fitting, stored));
        QVERIFY(nwchemSettingEditable(fitting, stored));
        MainWindow w;
        w.loadDocument(d);
        auto *toggle = w.findChild<QCheckBox *>("nwchem_ri_j");
        QVERIFY(toggle && toggle->isEnabled() && toggle->isChecked());
        toggle->click();
        QVERIFY(!nwchemSettings(w.document())["ri_j"].toBool());
        QVERIFY(!toggle->isEnabled());
        auto *iterations = w.findChild<QSpinBox *>("nwchem_scf_maxiter");
        QVERIFY(iterations);
        QCOMPARE(iterations->maximum(), 10000);
        QCOMPARE(w.document().envelope, envelope);
        QCOMPARE(nwchemSettings(w.document())["future_setting"], stored["future_setting"]);
        w.history()->undo();
        QVERIFY(toggle->isEnabled() && toggle->isChecked());
        // A choice with source-language labels must not freeze the language at first use.
        const auto japanese = nwchemChoices("task");
        setEnglish(true);
        const auto english = nwchemChoices("task");
        setEnglish(false);
        QCOMPARE(japanese[0].value, english[0].value);
        QVERIFY(japanese[0].label != english[0].label);
    }
    void panelNebEnd() {
        MainWindow w;
        w.loadDocument(Document::fromXyz("3\nHCN\nC 0 0 0\nN 0 0 1.156\nH -0.9 0 -0.7\n"));
        auto *panel = w.findChild<CalculationPanel *>();
        auto *state = w.findChild<QLabel *>("calculationNebEnd");
        QVERIFY(panel && state);
        QCOMPARE(state->text(), QString("未設定"));
        // The product drawn first and kept as the end, then the molecule edited back into the start.
        button(w, "calculationNebEndCurrent")->click();
        QCOMPARE(nwchemSettings(w.document())["task"].toString(), QString("neb"));
        QVERIFY(errorContains(w.document(), "終点が始点"));
        w.edit("move", [](Document &d) { d.atoms[2].position = Vec(0.8, 0, 1.85); });
        QVERIFY(validateNwchem(w.document()).errors.isEmpty());
        QVERIFY(state->text().contains("設定済み"));
        // Swapping shows the end in "1 分子"; swapping again and undo come back.
        button(w, "calculationNebSwap")->click();
        QVERIFY((w.document().atoms[2].position - Vec(-0.9, 0, -0.7)).norm() < 1e-12);
        QVERIFY((nwchemNebEnd(w.document()).document->atoms[2].position - Vec(0.8, 0, 1.85)).norm() < 1e-12);
        w.history()->undo();
        QVERIFY((w.document().atoms[2].position - Vec(0.8, 0, 1.85)).norm() < 1e-12);
        // From a file: the last frame of a multi-frame XYZ, fitted onto the molecule.
        QTemporaryDir dir;
        const auto file = dir.filePath("path.xyz");
        writeFile(file,
                  "3\none\nC 0 0 0\nN 0 0 1.156\nH 0 5 5\n3\ntwo\nC 1 0 0\nN 1 0 1.156\nH 0.1 0 -0.7\n");
        panel->setNebEndFromFile(file);
        const auto end = nwchemNebEnd(w.document()).document;
        QVERIFY(end);
        QVERIFY((end->atoms[1].position - end->atoms[0].position).norm() - 1.156 < 1e-9);
        QVERIFY(end->atoms[2].position.norm() < 2); // Not the first frame.
        auto other = dir.filePath("water.xyz");
        writeFile(other, "3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        QVERIFY_THROWS_EXCEPTION(std::exception, panel->setNebEndFromFile(other));
        // A file beyond 128 MiB is refused before anything is read, as over MCP.
        const auto set = w.document();
        for (const auto *name : {"large.xyz", "large.out"}) {
            QFile large(dir.filePath(name));
            QVERIFY(large.open(QIODevice::WriteOnly));
            QVERIFY(large.resize(qint64(128) * 1024 * 1024 + 1)); // Sparse: no data is written.
            large.close();
            try {
                panel->setNebEndFromFile(large.fileName());
                QFAIL("A file beyond the limit was read");
            } catch (const std::exception &e) {
                QVERIFY2(QString::fromUtf8(e.what()).contains("128 MiB"), e.what());
            }
            QVERIFY(!panel->loadingNebEnd());
            QVERIFY(large.remove());
        }
        QCOMPARE(w.document(), set);
    }
    void openingNwchemInputShowsItsNotes() {
        // Both suffixes of NWChem inputs list what was not imported when the file is opened.
        QTemporaryDir dir;
        const QByteArray input = "geometry\n Ne 0 0 0\nend\nset geometry:actlist 1\ntask scf energy\n";
        QVERIFY(!importNwchemInput(QString::fromUtf8(input)).notes.isEmpty());
        for (const auto *name : {"neon.nw", "neon.nwinp"}) {
            writeFile(dir.filePath(name), input);
            MainWindow w;
            QString shown;
            QTimer::singleShot(0, &w, [&shown] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    shown = box->text();
                    box->accept();
                }
            });
            w.loadPath(dir.filePath(name));
            QVERIFY2(shown.contains("取り込まなかった項目"), name);
            QCOMPARE(w.document().atoms.size(), 1);
            QCOMPARE(w.document().atoms[0].element, QString("Ne"));
        }
    }
    void panelNebLogLoading() {
        QTemporaryDir dir;
        const auto file = dir.filePath("end.out");
        writeFile(file, "NWChem\n Output coordinates in angstroms (scale by 1.8897 to convert to a.u.)\n\n"
                        " No. Tag Charge X Y Z\n ---- ---- ------ ----- ----- -----\n"
                        " 1 O 8.0 0.0 0.0 0.0\n 2 H 1.0 0.9 0.0 0.6\n"
                        " 3 H 1.0 -0.9 0.0 0.6\n\n");
        MainWindow w;
        w.loadDocument(water());
        auto *panel = w.findChild<CalculationPanel *>();
        QSignalSpy loaded(panel, &CalculationPanel::nebEndLoaded);
        QSignalSpy failed(panel, &CalculationPanel::nebEndLoadFailed);
        const auto before = w.document();
        panel->setNebEndFromFile(file);
        QVERIFY(panel->loadingNebEnd());
        QCOMPARE(w.document(), before);
        panel->cancelNebEndLoad();
        QVERIFY(!panel->loadingNebEnd());
        QCOMPARE(w.document(), before);
        QCOMPARE(loaded.count(), 0);
        // A result for an earlier document must not overwrite edits made while it was loading.
        panel->setNebEndFromFile(file);
        w.edit("change conditions", [](Document &d) { setNwchemSetting(d, "title", "changed"); });
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY2(failed[0][0].toString().contains("読込中"), qPrintable(failed[0][0].toString()));
        QVERIFY(!nwchemNebEnd(w.document()).document);
        const auto changed = w.document();
        // Replacing a request cancels its worker; only the latest one can apply an undoable edit.
        const auto latest = dir.filePath("latest.out");
        writeFile(latest, QByteArray(readFile(file)).replace("0.9", "1.2"));
        panel->setNebEndFromFile(file);
        panel->setNebEndFromFile(latest);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QCOMPARE(failed.count(), 1);
        const auto endpoint = nwchemNebEnd(w.document()).document;
        QVERIFY(endpoint);
        QVERIFY(std::abs((endpoint->atoms[1].position - endpoint->atoms[0].position).norm() -
                         std::sqrt(1.2 * 1.2 + 0.6 * 0.6)) < 1e-9);
        QVERIFY(!panel->loadingNebEnd());
        w.history()->undo();
        QCOMPARE(w.document(), changed);
        // Closing the window cancels its reader, too.
        panel->setNebEndFromFile(file);
        panel->stopEditing();
        QVERIFY(!panel->loadingNebEnd());
    }
    void panelConstraintsScanAndExtraInput() {
        MainWindow w;
        w.loadDocument(water());
        const auto ids = w.document().ids();
        choose(w, "task", "optimize");
        w.setSelection({ids[1], ids[0], ids[2]});
        button(w, "calculationAddConstraint")->click();
        auto constraints = nwchemSettings(w.document())["constraints"].toArray();
        QCOMPARE(constraints.size(), 1);
        QCOMPARE(idsFromJson(constraints[0].toObject()["atoms"]), (Ids{ids[1], ids[0], ids[2]}));
        auto *table = w.findChild<QTableWidget *>("nwchem_constraints");
        QCOMPARE(table->item(0, 0)->text(), QString("角度 2–1–3"));
        table->item(0, 1)->setText("110");
        QCOMPARE(nwchemSettings(w.document())["constraints"][0].toObject()["value"].toDouble(), 110.0);
        QVERIFY(nwchemInput(w.document()).contains("angle 2 1 3 110 mfc1 constant"));
        w.setSelection({ids[0], ids[1]});
        button(w, "calculationScanAtoms")->click();
        const auto s = nwchemSettings(w.document());
        QCOMPARE(s["task"].toString(), QString("scan"));
        QCOMPARE(idsFromJson(s["scan_atoms"]), (Ids{ids[0], ids[1]}));
        QVERIFY(std::abs(s["scan_start"].toDouble() - 0.9103) < 1e-3);
        QVERIFY(std::abs(s["scan_end"].toDouble() - s["scan_start"].toDouble() - 0.5) < 1e-9);
        QCOMPARE(w.findChild<QLabel *>("calculationScanCoordinate")->text(), QString("距離 1–2"));
        // Additional input: applied explicitly, and protected from MCP like the executable.
        auto *extra = w.findChild<QPlainTextEdit *>("nwchem_extra_input");
        QVERIFY(extra->property("mcpExecutionSetting").toBool());
        QVERIFY(button(w, "calculationApplyExtra")->property("mcpExecutionSetting").toBool());
        extra->setPlainText("set cphf:maxiter 50");
        QVERIFY(nwchemSettings(w.document())["extra_input"].toString().isEmpty());
        button(w, "calculationApplyExtra")->click();
        QCOMPARE(nwchemSettings(w.document())["extra_input"].toString(), QString("set cphf:maxiter 50"));
        w.history()->undo();
        QVERIFY(extra->toPlainText().isEmpty()); // Undo shows the stored text again.
    }
    void panelC3Settings() {
        MainWindow w;
        w.loadDocument(Document::fromXyz("6\ndimer\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n"
                                         "O 0 0 3\nH 0.758 0 3.504\nH -0.758 0 3.504\n"));
        const auto ids = w.document().ids();
        // An added internal coordinate: "固定" off.
        choose(w, "task", "optimize");
        w.setSelection({ids[1], ids[0], ids[2]});
        button(w, "calculationAddConstraint")->click();
        auto *table = w.findChild<QTableWidget *>("nwchem_constraints");
        QCOMPARE(table->item(0, 2)->checkState(), Qt::Checked);
        table->item(0, 2)->setCheckState(Qt::Unchecked);
        QVERIFY(nwchemSettings(w.document())["constraints"][0].toObject()["free"].toBool());
        QCOMPARE(table->item(0, 1)->text(), QString("（固定しない）"));
        QVERIFY(nwchemInput(w.document()).contains("    angle 2 1 3\n"));
        w.history()->undo();
        w.history()->undo();
        // Fragments: the selection, then the rest, each with the charge of its atoms.
        w.setSelection(ids.mid(0, 3));
        button(w, "calculationAddFragment")->click();
        button(w, "calculationAddFragmentRest")->click();
        auto fragments = nwchemSettings(w.document())["fragment_guess"].toArray();
        QCOMPARE(fragments.size(), 2);
        QCOMPARE(idsFromJson(fragments[1].toObject()["atoms"]), ids.mid(3));
        auto *fragmentTable = w.findChild<QTableWidget *>("nwchem_fragment_guess");
        QCOMPARE(fragmentTable->item(0, 0)->text(), QString("H2O（3 原子）"));
        fragmentTable->item(1, 2)->setText("3");
        QCOMPARE(nwchemSettings(w.document())["fragment_guess"][1].toObject()["multiplicity"].toInt(), 3);
        QVERIFY(!validateNwchem(w.document()).errors.isEmpty()); // 10 electrons cannot be a triplet's.
        fragmentTable->setCurrentCell(1, 0);
        button(w, "calculationRemoveFragment")->click();
        QCOMPARE(nwchemSettings(w.document())["fragment_guess"].toArray().size(), 1);
        // The second scan coordinate.
        w.setSelection({ids[0], ids[1]});
        button(w, "calculationScanAtoms")->click();
        w.setSelection({ids[2], ids[0], ids[1]});
        button(w, "calculationScanAtoms2")->click();
        QCOMPARE(idsFromJson(nwchemSettings(w.document())["scan2_atoms"]), (Ids{ids[2], ids[0], ids[1]}));
        QCOMPARE(w.findChild<QLabel *>("calculationScanCoordinate2")->text(), QString("角度 3–1–2"));
        button(w, "calculationScanClear2")->click();
        QVERIFY(nwchemSettings(w.document())["scan2_atoms"].toArray().isEmpty());
        // Pasting a basis through its dialog.
        QTimer::singleShot(0, &w, [&] {
            if (auto *dialog = w.findChild<QDialog *>("pasteBasisDialog")) {
                dialog->findChild<QPlainTextEdit *>("pasteBasisText")->setPlainText("H S\n1.0 1.0\n");
                QVERIFY(dialog->findChild<QLabel *>("pasteBasisState")->text().contains("基底: H"));
                dialog->accept();
            }
        });
        button(w, "calculationPasteBasis")->click();
        QCOMPARE(nwchemSettings(w.document())["basis_custom"].toString(), QString("H S\n1.0 1.0\n"));
        QCOMPARE(w.findChild<QLabel *>("calculationPastedBasis")->text(), QString("基底: H"));
    }
    void diagnosticsChangesAndSearch() {
        MainWindow w;
        w.loadDocument(water());
        choose(w, "method", "dft");
        choose(w, "dispersion", "d3");
        choose(w, "functional", "pbe0");
        // Changes from the defaults, in the form's words.
        const auto rows = changes(w);
        QVERIFY2(rows.contains("電子状態の手法: DFT") && rows.contains("DFT汎関数: PBE0"),
                 qPrintable(rows.join('\n')));
        // A diagnostic with a quick fix (PBE0-D3 is fine; SCAN-D3 has no parameters).
        choose(w, "functional", "scan");
        auto *list = w.findChild<QListWidget *>("calculationValidation");
        QListWidgetItem *fixable = nullptr;
        for (int i = 0; i < list->count(); ++i)
            if (!list->item(i)->data(Qt::UserRole + 2).toString().isEmpty())
                fixable = list->item(i);
        QVERIFY(fixable);
        QVERIFY(fixable->text().contains("分散補正を「なし」にする"));
        emit list->itemDoubleClicked(fixable);
        QCOMPARE(nwchemSettings(w.document())["dispersion"].toString(), QString("none"));
        w.findChild<QSpinBox *>("nwchem_charge")->setValue(1); // Odd electron count: the fix gives a doublet.
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text().contains("多重度"))
                emit list->itemDoubleClicked(list->item(i));
        QCOMPARE(w.document().multiplicity, 2);
        // Searching a detailed option shows it in the details of its section.
        auto *search = w.findChild<QLineEdit *>("calculationSearch");
        search->setText("DIIS部分空間");
        QMetaObject::invokeMethod(search, "returnPressed");
        QCOMPARE(w.findChild<QTabWidget *>("workspaceTabs")->currentWidget()->objectName(),
                 QString("calculationTab"));
        QVERIFY(w.findChild<QWidget *>("calculationDetailBody_convergence")->isVisibleTo(&w));
        QVERIFY(w.findChild<QWidget *>("calculationDetailRow_scf_diisbas")->isVisibleTo(&w));
        QCOMPARE(w.findChild<QLabel *>("calculationDetailTitle")->text(), QString("DIIS部分空間の大きさ"));
    }
    void sectionsChangesAndResets() {
        // Sections without a part in the calculation, and why.
        auto d = water();
        using Section = NwchemSection;
        const auto unused = [](const QString &reason = {}) { return NwchemSectionUse{false, reason}; };
        QCOMPARE(nwchemSectionUse(d, Section::Path), unused());
        QCOMPARE(nwchemSectionUse(d, Section::Constraints), unused("最適化のみ"));
        QCOMPARE(nwchemSectionUse(d, Section::Optimization), unused());
        QCOMPARE(nwchemSectionUse(d, Section::Followup), unused("最適化のみ"));
        QCOMPARE(nwchemSectionUse(d, Section::Bsse), unused("単量体なし"));
        QCOMPARE(nwchemSectionUse(d, Section::Excited), unused("DFTのみ"));
        set(d, "method", "dft");
        QCOMPARE(nwchemSectionUse(d, Section::Excited), NwchemSectionUse());
        set(d, "task", "optimize_frequencies");
        QCOMPARE(nwchemSectionUse(d, Section::Excited), unused("一点・最適化"));
        QVERIFY(nwchemSectionUse(d, Section::Constraints).used &&
                nwchemSectionUse(d, Section::Optimization).used &&
                nwchemSectionUse(d, Section::Followup).used);
        // A section in use has no reason.
        QVERIFY(nwchemSectionUse(d, Section::Followup).reason.isEmpty());
        set(d, "task", "scan");
        set(d, "scan_relaxed", false);
        QVERIFY(nwchemSectionUse(d, Section::Path).used);
        QCOMPARE(nwchemSectionUse(d, Section::Constraints), unused("最適化のみ"));
        // The panel's cards of these sections have the keys of the core; the others have none.
        QStringList keys;
        for (const auto section : {Section::Path, Section::Constraints, Section::Optimization,
                                   Section::Followup, Section::Bsse, Section::Excited}) {
            keys << nwchemSectionKey(section);
            QVERIFY(nwchemSectionFromKey(keys.last()) == section);
        }
        QCOMPARE(keys, (QStringList{"path", "constraints", "optimization", "followup", "bsse", "excited"}));
        QVERIFY(!nwchemSectionFromKey("basic") && !nwchemSectionFromKey(""));

        MainWindow w;
        w.loadDocument(water());
        auto *sections = w.findChild<QListWidget *>("calculationSections");
        const auto entry = [&](const QString &key) -> QListWidgetItem * {
            for (int i = 0; i < sections->count(); ++i)
                if (sections->item(i)->data(Qt::UserRole) == key)
                    return sections->item(i);
            return nullptr;
        };
        // What the list and the card say of a section without a part: with its reason, if it has one.
        QCOMPARE(entry("path")->data(Qt::UserRole + 12).toString(), QString("使わない"));
        QCOMPARE(entry("path")->toolTip(), QString("この計算では使いません"));
        QCOMPARE(entry("constraints")->data(Qt::UserRole + 12).toString(), QString("最適化のみ"));
        QCOMPARE(entry("constraints")->toolTip(), QString("この計算では使いません（最適化のみ）"));
        QVERIFY(entry("path")->data(Qt::UserRole + 13).toBool());
        QVERIFY(entry("basic")->data(Qt::UserRole + 12).toString().isEmpty() &&
                entry("basic")->toolTip().isEmpty() && !entry("basic")->data(Qt::UserRole + 13).toBool());
        for (const auto &key : keys)
            QVERIFY2(w.findChild<QWidget *>("calculationCard_" + key), qPrintable(key));
        QVERIFY(entry("basic")->data(Qt::UserRole + 11).toString().isEmpty());
        // Chips and segments set the hidden combo boxes that hold the values.
        w.findChild<QToolButton *>("nwchem_task_optimize")->click();
        w.findChild<QToolButton *>("nwchem_method_dft")->click();
        QCOMPARE(nwchemSettings(w.document())["task"].toString(), "optimize");
        QCOMPARE(nwchemSettings(w.document())["method"].toString(), "dft");
        QVERIFY(w.findChild<QToolButton *>("nwchem_method_dft")->isChecked());
        QVERIFY(w.findChild<QToolButton *>("nwchem_grid_fine")->isEnabled());
        QCOMPARE(entry("basic")->data(Qt::UserRole + 11).toString(), QString("変更 2"));
        QVERIFY(entry("optimization")->data(Qt::UserRole + 12).toString().isEmpty());
        QCOMPARE(changes(w), (QStringList{"計算目的: 構造最適化", "電子状態の手法: DFT"}));
        // Only changed rows: the other cards and rows give way.
        // All rows, or only the changed ones (the details too).
        auto *only = w.findChild<QToolButton *>("calculationChangeFilter_changed");
        QCOMPARE(only->text(), QString("変更済み 2"));
        only->click();
        auto *tab = w.findChild<QWidget *>("calculationTab");
        QVERIFY(!w.findChild<QWidget *>("calculationCard_path")->isVisibleTo(tab));
        QVERIFY(w.findChild<QWidget *>("calculationCard_basic")->isVisibleTo(tab));
        QVERIFY(!combo(w, "functional")->isVisibleTo(tab));
        QVERIFY(w.findChild<QWidget *>("calculationChoice_method")->isVisibleTo(tab));
        w.findChild<QToolButton *>("calculationChangeFilter_all")->click();
        QVERIFY(combo(w, "functional")->isVisibleTo(tab));
        // Each change resets on its own, a section at once; both undo.
        w.findChild<QToolButton *>("calculationReset_method")->click();
        QTRY_COMPARE(nwchemSettings(w.document())["method"].toString(), "hf");
        QCOMPARE(nwchemSettings(w.document())["task"].toString(), "optimize");
        choose(w, "basis", "6-31g*");
        button(w, "calculationResetSection_basic")->click();
        QCOMPARE(nwchemSettings(w.document())["task"].toString(), "energy");
        QCOMPARE(nwchemSettings(w.document())["basis"].toString(), "sto-3g");
        QVERIFY(changes(w).isEmpty());
        QVERIFY(w.findChild<QLabel *>("calculationNoChanges")->isVisibleTo(tab));
        w.history()->undo();
        QCOMPARE(nwchemSettings(w.document())["basis"].toString(), "6-31g*");
        // The details changed are counted with the fields, and lead to their rows.
        const int fieldsChanged = changes(w).size();
        w.edit("detail", [](Document &next) { setNwchemAdvanced(next, "scf_thresh", 1e-8); });
        QCOMPARE(changes(w).size(), fieldsChanged + 1);
        QCOMPARE(changes(w).last(), QString("波動関数の収束閾値: 1e-08"));
        auto *list = w.findChild<QTreeWidget *>("calculationChanges");
        // The card is as tall as its rows, not the rail.
        QCOMPARE(list->height(),
                 list->topLevelItemCount() * std::max(list->sizeHintForRow(0), 26) + 2 * list->frameWidth());
        // The checks say they passed, as in "4 入力プレビュー".
        QVERIFY(!w.findChild<QLabel *>("calculationChecksPassed")->isHidden());
        emit list->itemClicked(list->topLevelItem(list->topLevelItemCount() - 1), 0);
        QVERIFY(w.findChild<QWidget *>("calculationDetailRow_scf_thresh")->isVisibleTo(tab));
        QCOMPARE(w.findChild<QLabel *>("calculationDetailCurrent")->text(), QString("1e-08"));
        w.history()->undo();
        // "すべて既定へ" resets the conditions but keeps the name, title, unit and charge.
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        tabs->setCurrentIndex(1);
        w.edit("names", [](Document &next) {
            auto s = next.data["nwchem"].toObject();
            s["start"] = "kept";
            s["geometry_units"] = "bohr";
            s["method"] = "dft";
            next.data["nwchem"] = s;
            next.charge = 1;
            next.multiplicity = 2;
        });
        button(w, "calculationResetAll")->click();
        const auto reset = nwchemSettings(w.document());
        QCOMPARE(reset["method"].toString(), QString("hf"));
        QCOMPARE(reset["basis"].toString(), QString("sto-3g"));
        QCOMPARE(reset["start"].toString(), QString("kept"));
        QCOMPARE(reset["geometry_units"].toString(), QString("bohr"));
        QCOMPARE(w.document().charge, 1);
        QCOMPARE(w.history()->undoText(), QString("計算条件をすべて既定に戻す"));
        w.history()->undo();
        QCOMPARE(nwchemSettings(w.document())["basis"].toString(), QString("6-31g*"));
        // Settings of other tabs are shown with the way to where they are set.
        QVERIFY(!w.findChild<QSpinBox *>("nwchem_memory_mb"));
        QCOMPARE(w.findChild<QLabel *>("calculationMemoryState")->text(), QString("1024 MB / MPIプロセス"));
        button(w, "calculationMemoryLink")->click();
        QCOMPARE(tabs->currentWidget()->objectName(), QString("executionTab"));
        tabs->setCurrentIndex(1);
        // The MOLDEN and CUBE files are set in the details of their own section, open from the start.
        auto *molden = w.findChild<QCheckBox *>("advanced_molden_output");
        QVERIFY(molden->isVisibleTo(tab) && !molden->isChecked());
        w.edit("molden", [](Document &next) { setNwchemAdvanced(next, "molden_output", true); });
        QVERIFY(molden->isChecked());
        QVERIFY(w.findChild<QLabel *>("calculationOutputFiles")->text().contains(".molden"));
    }
    void basisLibraryAndElementBases() {
        // LANL2DZ: all-electron for light elements, ECP from sodium on ("nelec" of NWChem's library).
        auto hi = Document::fromXyz("2\nhydrogen iodide\nH 0 0 0\nI 0 0 1.61\n");
        set(hi, "basis", "lanl2dz ecp");
        auto check = validateNwchem(hi);
        QVERIFY2(check.errors.isEmpty(), qPrintable(check.errors.join('\n')));
        QCOMPARE(check.ecpElectrons, 46);
        QCOMPARE(check.electrons, 8);
        QVERIFY(check.warnings.join('\n').contains("ECP（lanl2dz ecp）"));
        QVERIFY(nwchemInput(hi).contains(
            "  * library \"lanl2dz ecp\"\nend\n\necp\n  I library \"lanl2dz ecp\"\nend"));
        // Per-element bases: each element on its own line, ECP only where that basis needs one.
        set(hi, "basis", "6-31g*");
        QVERIFY(errorContains(hi, "対応元素に含まれません: I"));
        set(hi, "basis_overrides", QJsonObject{{"I", "def2-tzvp"}});
        check = validateNwchem(hi);
        QVERIFY2(check.errors.isEmpty(), qPrintable(check.errors.join('\n')));
        QCOMPARE(check.ecpElectrons, 28);
        auto text = nwchemInput(hi);
        QVERIFY(text.contains("basis cartesian\n  H library \"6-31g*\"\n  I library \"def2-tzvp\"\nend"));
        QVERIFY(text.contains("ecp\n  I library \"def2-ecp\"\nend"));
        QVERIFY(!text.contains("* library"));
        QVERIFY(check.warnings.join('\n').contains("d関数の慣例"));
        QCOMPARE(nwchemBasisLabel(nwchemSettings(hi)), QString("6-31G*、I: def2-TZVP")); // Usual spellings.
        set(hi, "basis_overrides", QJsonObject{{"I", "6-31+g*"}});
        QVERIFY(errorContains(hi, "I（6-31+g*）"));
        for (const auto &bad : {QJsonObject{{"Xx", "def2-svp"}}, QJsonObject{{"I", "custom"}}}) {
            set(hi, "basis_overrides", bad);
            QVERIFY(errorContains(hi, "元素別の基底"));
        }
        // An override for an element that is not in the molecule changes nothing.
        auto water = ::water();
        set(water, "basis_overrides", QJsonObject{{"Fe", "def2-tzvp"}});
        QVERIFY(nwchemInput(water).contains("  * library \"sto-3g\"\n"));
        // The table in "2 計算条件" edits the same setting and is undoable.
        MainWindow w;
        w.loadDocument(::water());
        w.show();
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(1);
        auto *oxygen = w.findChild<QComboBox *>("nwchem_basis_O");
        QVERIFY(oxygen);
        oxygen->setCurrentIndex(oxygen->findData("cc-pvdz"));
        QCOMPARE(nwchemSettings(w.document())["basis_overrides"].toObject(), QJsonObject({{"O", "cc-pvdz"}}));
        w.history()->undo();
        QVERIFY(nwchemSettings(w.document())["basis_overrides"].toObject().isEmpty());
        QCOMPARE(w.findChild<QComboBox *>("nwchem_basis_O")->currentIndex(), 0);
    }
    void nativeTabUndoValidationAndSync() {
        MainWindow w;
        const auto d = water();
        w.loadDocument(d);
        auto *tabs = w.findChild<QTabWidget *>("workspaceTabs");
        QCOMPARE(tabs->tabText(1), "2 計算条件");
        tabs->setCurrentIndex(1);
        QCOMPARE(w.document().toJson(), d.toJson());
        QCOMPARE(w.history()->count(), 0);
        auto *status = w.findChild<QLabel *>("calculationStatus");
        QVERIFY(status->text().contains("入力を生成できます"));
        choose(w, "method", "dft");
        QVERIFY(combo(w, "functional")->isEnabled());
        QCOMPARE(w.history()->count(), 1);
        button(w, "editorUndo")->click(); // Undo and Redo are the header's.
        QCOMPARE(w.document().toJson(), d.toJson());
        QVERIFY(button(w, "editorRedo")->isEnabled());
        button(w, "editorRedo")->click();
        QCOMPARE(nwchemSettings(w.document())["method"].toString(), "dft");
        choose(w, "geometry_units", "bohr");
        QVERIFY(
            w.findChild<QAction *>("copyNwchemGeometry")->text().endsWith("（Bohr）")); // The table's export.
        auto *charge = w.findChild<QSpinBox *>("nwchem_charge");
        auto *mult = w.findChild<QSpinBox *>("nwchem_multiplicity");
        charge->setValue(1);
        QCOMPARE(w.document().charge, 1);
        QVERIFY(status->text().contains("生成できません"));
        QVERIFY(!button(w, "inputPreviewCopy")->isEnabled());
        mult->setValue(2);
        QVERIFY(status->text().contains("入力を生成できます"));
        button(w, "inputPreviewCopy")->click();
        QCOMPARE(QApplication::clipboard()->text(), nwchemInput(w.document()));
        QVERIFY(QApplication::clipboard()->text().contains("  mult 2\n  odft\n"));
        const int count = w.history()->count();
        tabs->setCurrentIndex(0);
        tabs->setCurrentIndex(1);
        QCOMPARE(w.history()->count(), count);
        auto modified = w.document();
        set(modified, "method", "future_method");
        w.loadDocument(modified);
        QVERIFY(combo(w, "method")->currentText().contains("未対応"));
        // The segments name a value they have no button for.
        auto *methods = w.findChild<QWidget *>("calculationChoice_method");
        QVERIFY(methods->findChild<QLabel *>()->text().contains("未対応"));
        QVERIFY(!button(w, "inputPreviewSave")->isEnabled());
        QCOMPARE(nwchemSettings(w.document())["method"].toString(), "future_method");
    }
    void presetUndoAndCandidates() {
        MainWindow w;
        auto d = water();
        set(d, "geometry_units", "bohr");
        w.loadDocument(d);
        // The menu names what a preset changes before it is applied.
        auto *preset = action(w, "calculationPreset1");
        emit w.findChild<QMenu *>("calculationPresetMenu")->aboutToShow();
        QVERIFY(preset->toolTip().contains("電子状態の手法: Hartree–Fock → DFT"));
        preset->trigger();
        QCOMPARE(w.history()->count(), 1);
        QCOMPARE(nwchemSettings(w.document())["method"].toString(), "dft");
        QCOMPARE(nwchemSettings(w.document())["geometry_units"].toString(), "bohr");
        w.history()->undo();
        QCOMPARE(w.document().toJson(), d.toJson());
        w.edit("候補", [](Document &next) { next.atoms[1].position.x() += 0.1; }, true);
        QVERIFY(!combo(w, "method")->isEnabled());
        QVERIFY(!button(w, "inputPreviewCopy")->isEnabled());
        QVERIFY(!button(w, "calculationPresets")->isEnabled());
        QVERIFY(w.findChild<QLabel *>("calculationMessage")->text().contains("候補"));
        preset->trigger(); // Ignored while the candidate is pending.
        QCOMPARE(nwchemSettings(w.displayDocument())["method"].toString(), "hf");
        w.cancelCandidate();
        QVERIFY(button(w, "inputPreviewCopy")->isEnabled());
        QCOMPARE(w.document().toJson(), d.toJson());
        w.edit("別タブの電荷変更", [](Document &next) {
            next.charge = 1;
            next.multiplicity = 2;
        });
        QCOMPARE(w.findChild<QSpinBox *>("nwchem_charge")->value(), 1);
        QCOMPARE(w.findChild<QSpinBox *>("nwchem_multiplicity")->value(), 2);
    }
    void savedPresetAndTitleEditing() {
        MainWindow w;
        w.loadDocument(water());
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(1);
        w.show();
        choose(w, "method", "dft");
        choose(w, "functional", "pbe");
        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->setTextValue("test PBE");
            dialog->accept();
        });
        button(w, "calculationStorePreset")->click();
        QCOMPARE(userPresets(w), 1);
        QCOMPARE(action(w, "calculationUserPreset0")->text(), QString("test PBE"));
        choose(w, "functional", "blyp");
        action(w, "calculationUserPreset0")->trigger();
        QCOMPARE(nwchemSettings(w.document())["functional"].toString(), "pbe");
        auto *title = w.findChild<QLineEdit *>("nwchem_title");
        title->setText("invalid#title");
        QMetaObject::invokeMethod(title, "editingFinished");
        QVERIFY(w.findChild<QLabel *>("calculationStatus")->text().contains("生成できません"));
        title->setText("Valid title");
        QMetaObject::invokeMethod(title, "editingFinished");
        QVERIFY(w.findChild<QLabel *>("calculationStatus")->text().contains("入力を生成できます"));
        QVERIFY(nwchemInput(w.document()).contains("title \"Valid title\""));
        MainWindow second;
        QCOMPARE(userPresets(second), 1);
        action(w, "calculationRemovePreset0")->trigger();
        QTRY_COMPARE(userPresets(w), 0);
    }
    void saveCommitsFocusedField() {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const auto path = files.filePath("test.molforma.json");
        saveProject(water(), path);
        MainWindow w;
        w.loadPath(path);
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(1);
        w.show();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        auto *title = w.findChild<QLineEdit *>("nwchem_title");
        auto *scroll = title->parentWidget();
        while (scroll && !qobject_cast<QScrollArea *>(scroll))
            scroll = scroll->parentWidget();
        QVERIFY(scroll);
        qobject_cast<QScrollArea *>(scroll)->ensureWidgetVisible(title);
        title->setFocus();
        QTRY_VERIFY(title->hasFocus());
        title->selectAll();
        QTest::keyClicks(title, "Saved with shortcut");
        QTest::keyClick(title, Qt::Key_S, Qt::ControlModifier);
        QTRY_COMPARE(nwchemSettings(w.document())["title"].toString(), "Saved with shortcut");
        const auto restored = Document::fromProject(QJsonDocument::fromJson(readFile(path)).object());
        QCOMPARE(nwchemSettings(restored)["title"].toString(), "Saved with shortcut");
        QVERIFY(w.history()->isClean());
    }
    void focusedFieldTeardown() {
        MainWindow w;
        w.loadDocument(water());
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(1);
        w.show();
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        auto *title = w.findChild<QLineEdit *>("nwchem_title");
        title->setFocus();
        QTRY_VERIFY(title->hasFocus());
        QTest::keyClicks(title, "pending");
        // Destruction must discard focus-out edits before the owner's members die.
        QCOMPARE(w.history()->count(), 0);
    }
    void engineArtifacts() {
        const auto path = qEnvironmentVariable("MOLFORMA_CALCULATION_ARTIFACTS");
        if (path.isEmpty())
            return;
        QVERIFY(QDir().mkpath(path));
        auto h2 = Document::fromXyz("2\nhydrogen\nH 0 0 0\nH 0 0 0.78\n");
        auto emitInput = [&](const QString &name, Document d) {
            set(d, "start", name);
            set(d, "memory_mb", 256);
            writeFile(path + '/' + name + ".nw", nwchemInput(d).toUtf8());
        };
        emitInput("hf", h2);
        for (const auto &functional : nwchemChoices("functional")) {
            auto d = h2;
            set(d, "method", "dft");
            set(d, "functional", functional.value);
            set(d, "xc_custom", "xpbe96 0.75 cpbe96 1.0 hfexch 0.25");
            for (const auto *dispersion : {"d3bj", "d3", "none"}) { // The first one NWChem supports.
                set(d, "dispersion", dispersion);
                if (validateNwchem(d).errors.isEmpty())
                    break;
            }
            emitInput(functional.value, d);
        }
        for (const auto &method : {"mp2", "ccsd", "ccsd(t)"}) {
            auto d = method == QString("ccsd(t)") ? water() : h2;
            if (method == QString("ccsd(t)"))
                set(d, "basis", "6-31g");
            set(d, "method", method);
            emitInput(method == QString("ccsd(t)") ? "ccsdt" : method, d);
        }
        auto opt = h2;
        set(opt, "task", "optimize_frequencies");
        emitInput("optfreq", opt);
        auto mp2 = h2;
        set(mp2, "method", "mp2");
        set(mp2, "task", "frequencies");
        emitInput("mp2freq", mp2);
        auto cosmo = water();
        set(cosmo, "method", "dft");
        set(cosmo, "task", "gradient");
        set(cosmo, "solvent", "cosmo");
        emitInput("cosmo", cosmo);
        auto open = Document::fromXyz("1\nhydrogen atom\nH 0 0 0\n");
        open.multiplicity = 2;
        emitInput("rohf", open);
        set(open, "method", "dft");
        emitInput("odft", open);
        auto ecp = Document::fromXyz("1\nrubidium ion\nRb 0 0 0\n");
        ecp.charge = 1;
        set(ecp, "basis", "def2-svp");
        emitInput("ecp", ecp);
        set(ecp, "method", "mp2");
        set(ecp, "freeze_core", true);
        emitInput("ecp_mp2", ecp);
        auto ghost = water();
        ghost.atoms[0].fixed = true;
        ghost.atoms[1].isotope = 2;
        ghost.atoms << coordinateAtom("@C", {"0", "0", "3"}, "", CoordinateUnit::Angstrom);
        emitInput("ghost", ghost);
        set(ghost, "geometry_units", "bohr");
        emitInput("ghost_bohr", ghost);
        set(ghost, "method", "dft");
        set(ghost, "dispersion", "d3bj");
        emitInput("ghost_dft", ghost);
        auto points = water();
        addPointCharge(points, Vec(0, 0, 5), -0.5);
        set(points, "include_point_charges", true);
        emitInput("points", points);
        auto frozen = water();
        set(frozen, "method", "mp2");
        set(frozen, "freeze_core", true);
        emitInput("mp2freeze", frozen);
        auto cosmoFreq = h2;
        set(cosmoFreq, "solvent", "cosmo");
        set(cosmoFreq, "task", "frequencies");
        emitInput("cosmofreq", cosmoFreq);
        MainWindow w;
        auto d = water();
        applyNwchemPreset(d, nwchemPresets()[1].settings);
        emitInput("preset_water", d);
        w.loadDocument(d);
        w.resize(1380, 920);
        w.findChild<QTabWidget *>("workspaceTabs")->setCurrentIndex(1);
        w.show();
        QCoreApplication::processEvents();
        QVERIFY(w.grab().save(path + "/calculation.png"));
    }
};
int main(int argc, char **argv) {
    if (const int code = resultWorkerMain(argc, argv); code >= 0)
        return code;
    QApplication app(argc, argv);
    CalculationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_calculation.moc"
