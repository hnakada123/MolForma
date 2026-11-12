// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/operations.h"
#include "io/structure_io.h"
#include "model/i18n.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_geometry.h"
#include "nwchem/nwchem_import.h"
#include "ui/calculation_panel.h"
#include "ui/main_window.h"
#include <QListWidget>
#include <QtTest>
#include <QtWidgets>
#include <algorithm>
#include <cmath>
#include <limits>
using namespace mf;

class CalculationTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDir;
    static Document water() {
        return Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
    }
    static void set(Document &d, const QString &key, const QJsonValue &value) {
        auto s = d.data["nwchem"].toObject();
        s[key] = value;
        d.data["nwchem"] = s;
    }
    static bool errorContains(const Document &d, const QString &text) {
        return validateNwchem(d).errors.join('\n').contains(text);
    }
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
    void diagnosticIdentityDoesNotDependOnLanguage() {
        auto d = water();
        set(d, "method", "dft");
        set(d, "functional", "scan");
        set(d, "dispersion", "d3");
        const auto japanese = validateNwchem(d);
        struct ResetLanguage {
            ~ResetLanguage() {
                setEnglish(false);
            }
        } reset;
        setEnglish(true);
        const auto english = validateNwchem(d);
        QCOMPARE(japanese.diagnostics.size(), english.diagnostics.size());
        bool found = false;
        for (int i = 0; i < japanese.diagnostics.size(); ++i) {
            const auto &a = japanese.diagnostics[i], &b = english.diagnostics[i];
            QCOMPARE(a.identity(), b.identity());
            QCOMPARE(a.target, b.target);
            QCOMPARE(a.severity, b.severity);
            if (a.code == "dispersion.parameters") {
                found = true;
                QVERIFY(a.fix && b.fix);
                QCOMPARE(a.fix->setting, QString("dispersion"));
                QCOMPARE(a.fix->value, b.fix->value);
                QVERIFY(a.message != b.message);
            }
        }
        QVERIFY(found);
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
    void realSettingLimits_data() {
        QTest::addColumn<QString>("key");
        QTest::addColumn<double>("minimum");
        QTest::addColumn<double>("maximum");
        QTest::addColumn<QString>("task");
        QTest::newRow("raman") << QString("raman_wavelength_nm") << 100. << 5000. << QString("energy");
        QTest::newRow("grid") << QString("property_grid_padding") << .5 << 20. << QString("energy");
        QTest::newRow("ord-start") << QString("ord_start_nm") << 100. << 5000. << QString("energy");
        QTest::newRow("ord-end") << QString("ord_end_nm") << 100. << 5000. << QString("energy");
        QTest::newRow("dielectric") << QString("dielectric") << 1. << 1000. << QString("energy");
        QTest::newRow("irc") << QString("irc_stride") << .01 << 1. << QString("irc");
        QTest::newRow("neb-spring") << QString("neb_spring") << .001 << 100. << QString("neb");
        QTest::newRow("neb-step") << QString("neb_stepsize") << .001 << 10. << QString("neb");
        for (const auto *key : {"scan_start", "scan_end", "scan2_start", "scan2_end"})
            QTest::newRow(key) << QString(key) << -180. << 180. << QString("scan");
    }
    void realSettingLimits() {
        QFETCH(QString, key);
        QFETCH(double, minimum);
        QFETCH(double, maximum);
        QFETCH(QString, task);
        auto d = water();
        set(d, "task", task);
        if (key.startsWith("scan2_"))
            set(d, "scan2_atoms", idsToJson({d.atoms[0].id, d.atoms[1].id}));
        const auto rangeError = [&](const Document &document) {
            const auto check = validateNwchem(document);
            return std::any_of(check.diagnostics.begin(), check.diagnostics.end(), [&](const auto &item) {
                return item.setting == key && (item.code == "settings.real" || item.code == "settings.type" ||
                                               item.code == "dielectric.range" || item.code == "irc.stride" ||
                                               item.code == "neb.spring" || item.code == "neb.stepsize");
            });
        };
        for (const double valid : {minimum, maximum, (minimum + maximum) / 2}) {
            set(d, key, valid);
            QVERIFY2(!rangeError(d), qPrintable(key));
        }
        for (const QJsonValue &invalid :
             {QJsonValue(std::nextafter(minimum, -INFINITY)), QJsonValue(std::nextafter(maximum, INFINITY)),
              QJsonValue("1.0"), QJsonValue(), QJsonValue(std::numeric_limits<double>::quiet_NaN()),
              QJsonValue(INFINITY)}) {
            set(d, key, invalid);
            QVERIFY2(rangeError(d), qPrintable(key));
            // Loading a project preserves settings for correction, but generating input refuses them.
            const auto loaded = Document::fromProject(d.project());
            QVERIFY(rangeError(loaded));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(loaded));
        }
        if (task == "scan" || task == "neb") {
            set(d, "task", "energy");
            set(d, key, maximum + 1);
            QVERIFY(!rangeError(d)); // Inactive path parameters keep their former applicability.
        }
    }
    void propertyNumbersAreValidatedBeforeInputGeneration() {
        auto raman = water();
        set(raman, "method", "dft");
        set(raman, "task", "frequencies");
        set(raman, "prop_raman", true);
        QVERIFY(validateNwchem(raman).errors.isEmpty());
        QVERIFY(nwchemInput(raman).contains("response 1 "));
        set(raman, "raman_wavelength_nm", 0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(raman));
        auto grid = water();
        set(grid, "prop_esp_grid", true);
        QVERIFY(validateNwchem(grid).errors.isEmpty());
        QVERIFY(!nwchemInput(grid).isEmpty());
        set(grid, "property_grid_padding", -1);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(grid));
        auto ord = water();
        set(ord, "prop_ord", true);
        QVERIFY(validateNwchem(ord).errors.isEmpty());
        QVERIFY(!nwchemInput(ord).isEmpty());
        set(ord, "ord_start_nm", 0);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(ord));
    }
    void completeHfInput() {
        const auto d = water();
        const auto before = d.toJson();
        const auto report = validateNwchem(d);
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join('\n')));
        QCOMPARE(report.electrons, 10);
        QCOMPARE(report.alpha, 5);
        QCOMPARE(report.beta, 5);
        QCOMPARE(report.reference, "rhf");
        const auto input = nwchemInput(d);
        QVERIFY(input.startsWith("# Generated by MolForma for NWChem\nstart molforma\n"));
        QVERIFY(input.contains("charge 0\n"));
        QVERIFY(input.contains("basis spherical\n  * library \"sto-3g\"\nend"));
        QVERIFY(input.contains("scf\n  rhf\n  nopen 0\n  maxiter 100\n  thresh 1e-7\nend"));
        QVERIFY(input.endsWith("task scf energy\n"));
        QVERIFY(!input.contains("fragment"));
        QCOMPARE(d.toJson(), before);
        auto untitled = d;
        set(untitled, "title", "");
        QVERIFY(!nwchemInput(untitled).contains("\ntitle "));
    }
    void internalCoordinatesConvergenceAndNumbers() {
        // Optimizations without fixed or ghost atoms leave AUTOZ on; other tasks stay Cartesian.
        auto d = water();
        QVERIFY(nwchemInput(d).contains("nocenter noautosym noautoz "));
        set(d, "task", "optimize_frequencies");
        QVERIFY(nwchemAutoz(d));
        QVERIFY(nwchemInput(d).contains("geometry units angstrom nocenter noautosym angstrom_to_au "));
        set(d, "task", "optimize");
        auto fixed = d;
        fixed.atoms[0].fixed = true;
        QVERIFY(!nwchemAutoz(fixed) && nwchemInput(fixed).contains(" noautoz "));
        auto ghost = d;
        ghost.atoms[2].ghost = true;
        QVERIFY(!nwchemAutoz(ghost));
        auto cartesian = d;
        setNwchemAdvanced(cartesian, "driver_cartesian", true);
        QVERIFY(!nwchemAutoz(cartesian) && nwchemInput(cartesian).contains(" noautoz "));
        QVERIFY(!nwchemInput(cartesian).contains("  noautoz\n")); // Not a DRIVER directive.
        set(d, "task", "irc");
        QVERIFY(!nwchemAutoz(d));
        // The shortest coordinates that read back as the same doubles.
        QVERIFY(nwchemInput(water()).contains("\n  H 0.758 0 0.504\n  H -0.758 0 0.504\n"));
        // "NWChem既定" leaves the SCF/DFT thresholds out, except those set in the details.
        d = water();
        set(d, "scf_convergence", "default");
        auto text = nwchemInput(d);
        QVERIFY(text.contains("  maxiter 100\nend\n") && !text.contains("thresh"));
        set(d, "method", "dft");
        set(d, "functional", "b3lyp");
        text = nwchemInput(d);
        QVERIFY(!text.contains("convergence") && text.contains("  iterations 100\n  grid "));
        setNwchemAdvanced(d, "dft_energy", 1e-8);
        QVERIFY(nwchemInput(d).contains("  convergence energy 1e-08\n"));
        QVERIFY(!validateNwchem(d).warnings.join('\n').contains("NWChem既定"));
        set(d, "task", "frequencies");
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("SCF収束がNWChem既定"));
        set(d, "scf_convergence", "tight");
        QVERIFY(nwchemInput(d).contains("  convergence energy 1e-08 density 1e-7 gradient 1e-7\n"));
    }
    void normalRamanInput() {
        auto d = water();
        set(d, "prop_raman", true);
        QVERIFY(errorContains(d, "通常ラマン"));
        set(d, "method", "dft");
        set(d, "task", "frequencies");
        set(d, "raman_wavelength_nm", 532.0);
        QVERIFY2(validateNwchem(d).errors.isEmpty(), qPrintable(validateNwchem(d).errors.join('\n')));
        const auto text = nwchemInput(d);
        QVERIFY(text.contains("task dft frequencies\n"));
        QVERIFY(text.contains("unset prop:*\nproperty\n  response 1 "));
        QVERIFY(text.endsWith("raman\n  normal\n  lorentzian\nend\ntask dft raman\n"));
        const auto imported = importNwchemInput(text);
        QVERIFY(nwchemSettings(imported.document)["prop_raman"].toBool());
        QVERIFY(std::abs(nwchemSettings(imported.document)["raman_wavelength_nm"].toDouble() - 532.0) < 1e-8);
        set(d, "followup", true);
        QVERIFY(errorContains(d, "通常ラマン"));
    }
    void solutionEprInput() {
        auto d = water();
        set(d, "prop_epr", true);
        QVERIFY(errorContains(d, "ESR物性"));
        d = Document::fromXyz("2\nOH\nO 0 0 0\nH 0 0 .97\n");
        d.multiplicity = 2;
        set(d, "method", "dft");
        set(d, "relativistic", "zora");
        set(d, "prop_epr", true);
        QVERIFY2(validateNwchem(d).errors.isEmpty(), qPrintable(validateNwchem(d).errors.join('\n')));
        QVERIFY(nwchemInput(d).contains("  gshift\n  hyperfine\n"));
    }
    void opticalActivityAndFieldsInput() {
        auto d = water();
        set(d, "method", "dft");
        set(d, "tddft", true);
        set(d, "tddft_cdspectrum", true);
        QVERIFY(nwchemInput(d).contains("  cdspectrum\n"));
        QVERIFY(nwchemSettings(importNwchemInput(nwchemInput(d)).document)["tddft_cdspectrum"].toBool());
        set(d, "tddft", false);
        set(d, "prop_ord", true);
        set(d, "ord_points", 3);
        auto input = nwchemInput(d);
        QCOMPARE(input.count("  giao\n"), 3);
        QVERIFY(nwchemSettings(importNwchemInput(input).document)["prop_ord"].toBool());
        set(d, "ord_start_nm", 800.);
        QVERIFY(errorContains(d, "ORD"));
        set(d, "prop_ord", false);
        set(d, "prop_esp_grid", true);
        set(d, "prop_efield_grid", true);
        input = nwchemInput(d);
        QVERIFY(input.contains("  esp\n  grid pad 4 ngrid 50 output "));
        QVERIFY(input.contains("  efield\n  grid pad 4 ngrid 50 output "));
        QVERIFY(nwchemSettings(importNwchemInput(input).document)["prop_esp_grid"].toBool());
        set(d, "property_grid_points", 200);
        QVERIFY(!validateNwchem(d).errors.isEmpty());
    }
    void dftFunctionalsAndTasks() {
        auto d = water();
        set(d, "method", "dft");
        const QMap<QString, QString> expected{{"b3lyp", "b3lyp"},
                                              {"pbe", "xpbe96 cpbe96"},
                                              {"pbe0", "pbe0"},
                                              {"blyp", "becke88 lyp"},
                                              {"bp86", "becke88 perdew86"}};
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            set(d, "functional", it.key());
            set(d, "dispersion", "d3");
            const auto text = nwchemInput(d);
            QVERIFY(text.contains("  xc " + it.value() + '\n'));
            QVERIFY(text.contains("  disp vdw 3\n"));
            QVERIFY(!text.contains("\nscf\n"));
            set(d, "dispersion", "d3bj");
            QVERIFY(nwchemInput(d).contains("  disp vdw 4\n"));
        }
        set(d, "functional", "b3lyp");
        set(d, "task", "optimize_frequencies");
        set(d, "grid", "xfine");
        set(d, "opt_maxiter", 42);
        set(d, "opt_convergence", "tight");
        const auto text = nwchemInput(d);
        QVERIFY(text.contains("  grid xfine\n"));
        QVERIFY(text.contains("driver\n  maxiter 42\n  tight\nend"));
        QVERIFY(text.endsWith("task dft optimize\ntask dft frequencies\n"));
        set(d, "numerical_frequencies", true);
        QVERIFY(nwchemInput(d).endsWith("task dft frequencies numerical\n"));
    }
    void spinAndReferenceCompatibility() {
        auto d = water();
        d.charge = 1;
        QVERIFY(errorContains(d, "整合しません"));
        d.multiplicity = 2;
        QCOMPARE(validateNwchem(d).electrons, 9);
        QCOMPARE(validateNwchem(d).alpha, 5);
        QCOMPARE(validateNwchem(d).beta, 4);
        QVERIFY(nwchemInput(d).contains("  rohf\n  nopen 1\n"));
        set(d, "reference", "rhf");
        QVERIFY(errorContains(d, "RHF / RKS"));
        set(d, "reference", "auto");
        set(d, "method", "dft");
        QVERIFY(nwchemInput(d).contains("  mult 2\n  odft\n"));
        set(d, "method", "mp2");
        QVERIFY(nwchemInput(d).contains("  uhf\n  nopen 1\n"));
        set(d, "reference", "rohf");
        QVERIFY(errorContains(d, "ROHF参照"));
        set(d, "reference", "auto");
        set(d, "method", "ccsd");
        QVERIFY(errorContains(d, "閉殻RHF"));
        d.charge = 0;
        d.multiplicity = 1;
        for (const auto &method : {"ccsd", "ccsd(t)"}) {
            set(d, "method", method);
            const auto text = nwchemInput(d);
            QVERIFY(text.contains("\nccsd\n  freeze 0\n"));
            QVERIFY(text.endsWith(QString("task %1 energy\n").arg(method)));
        }
        set(d, "task", "gradient");
        QVERIFY(errorContains(d, "一点エネルギー"));
        set(d, "method", "mp2");
        set(d, "freeze_core", true);
        QVERIFY(nwchemInput(d).contains("mp2\n  freeze atomic\nend"));
        set(d, "task", "frequencies");
        QVERIFY(nwchemInput(d).endsWith("task mp2 frequencies numerical\n"));
        d.multiplicity = 12;
        QVERIFY(errorContains(d, "整合しません"));
    }
    void basisCoverageAndEcp() {
        auto d = Document::fromXyz("1\nrubidium ion\nRb 0 0 0\n");
        set(d, "basis", "def2-svp");
        d.charge = 1;
        QCOMPARE(validateNwchem(d).ecpElectrons, 28);
        QCOMPARE(validateNwchem(d).electrons, 8);
        QVERIFY(nwchemInput(d).contains("ecp\n  Rb library \"def2-ecp\"\nend"));
        set(d, "method", "mp2");
        set(d, "freeze_core", true);
        QVERIFY(validateNwchem(d).errors.isEmpty());
        QCOMPARE(validateNwchem(d).frozenOrbitals, 0);
        set(d, "freeze_core", false);
        QVERIFY(validateNwchem(d).errors.isEmpty());
        d.atoms[0].element = "Hf";
        d.charge = 0;
        QCOMPARE(validateNwchem(d).ecpElectrons, 60);
        QCOMPARE(validateNwchem(d).electrons, 12);
        d.atoms[0].element = "La";
        d.charge = 1;
        QCOMPARE(validateNwchem(d).ecpElectrons, 46);
        QCOMPARE(validateNwchem(d).electrons, 10);
        d.atoms[0].element = "Ce";
        QVERIFY(errorContains(d, "対応元素"));
        for (const auto &symbol : {"U", "Og"}) {
            d.atoms[0].element = symbol;
            d.charge = 0;
            QCOMPARE(validateNwchem(d).electrons, element(symbol).number);
            QVERIFY(nwchemGeometry(d, CoordinateUnit::Angstrom).contains(QString(symbol) + " "));
            QVERIFY(errorContains(d, "対応元素"));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
        }
        d.atoms[0].element = "Kr";
        d.charge = 0;
        set(d, "basis", "6-31g");
        QVERIFY(errorContains(d, "対応元素"));
        set(d, "basis", "6-31g*");
        QVERIFY(validateNwchem(d).errors.isEmpty());
        d.atoms[0].element = "K";
        set(d, "basis", "cc-pvdz");
        QVERIFY(errorContains(d, "対応元素"));
        d = Document::fromXyz("1\nlithium ion\nLi 0 0 0\n");
        d.charge = 1;
        set(d, "method", "mp2");
        set(d, "freeze_core", true);
        QVERIFY(errorContains(d, "相関電子が不足"));
        QCOMPARE(validateNwchem(d).frozenOrbitals, 1);
    }
    void geometryGhostsAndFixedIds() {
        auto d = water();
        d.atoms[0].fixed = true;
        d.atoms[1].isotope = 2;
        auto ghost = coordinateAtom("@C", {"0", "0", "3"}, "", CoordinateUnit::Angstrom);
        d.atoms << ghost;
        std::reverse(d.atoms.begin(), d.atoms.end());
        set(d, "geometry_units", "bohr");
        set(d, "task", "optimize");
        const auto text = nwchemInput(d);
        QCOMPARE(validateNwchem(d).electrons, 10);
        QVERIFY(text.contains("geometry units au"));
        QVERIFY(text.contains("  bqC library C \"sto-3g\"\n"));
        // Ghost centers carry no nucleus; optimization keeps them fixed with the user's fixed atoms.
        QVERIFY(text.contains("  fix atom 1 4\n"));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("ゴースト中心は核を持たない"));
        set(d, "task", "energy");
        QVERIFY(nwchemInput(d).contains("constraints\n  clear\n  fix atom 4\nend"));
        set(d, "task", "optimize");
        for (auto &a : d.atoms)
            a.fixed = !a.ghost;
        QVERIFY(errorContains(d, "全原子が固定"));
        for (auto &a : d.atoms)
            a.fixed = a.element == "O";
        QVERIFY(text.contains("mass 2.014"));
        QVERIFY(!text.contains("Ghost basis required"));
        set(d, "task", "frequencies");
        QVERIFY(errorContains(d, "固定原子・ゴースト"));
        for (auto &a : d.atoms)
            a.ghost = true;
        QVERIFY(errorContains(d, "実原子"));
        d = water();
        d.atoms[0].isotope = 999;
        QVERIFY(!validateNwchem(d).errors.isEmpty());
    }
    void solventAndExternalCharges() {
        auto d = water();
        addPointCharge(d, Vec(0, 0, 5), -0.5);
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("点電荷は入力に含めません"));
        QVERIFY(!nwchemInput(d).contains("\nbq units"));
        set(d, "include_point_charges", true);
        set(d, "geometry_units", "bohr");
        QVERIFY(nwchemInput(d).contains("bq units angstrom\n  0 0 5 -0.5\nend"));
        set(d, "solvent", "cosmo");
        set(d, "dielectric", 40.0);
        QVERIFY(nwchemInput(d).contains("cosmo\n  dielec 40\nend"));
        set(d, "task", "frequencies");
        QVERIFY(errorContains(d, "外部点電荷中"));
        set(d, "include_point_charges", false);
        QVERIFY(nwchemInput(d).endsWith("task scf frequencies numerical\n"));
        set(d, "method", "mp2");
        QVERIFY(errorContains(d, "COSMOはHF"));
    }
    void invalidInputCannotBeExported() {
        const QVector<QPair<QString, QJsonValue>> bad{{"method", "tce"},
                                                      {"task", "energy\ntask shell unsafe"},
                                                      {"basis", "custom"},
                                                      {"reference", "none"},
                                                      {"start", "../job"},
                                                      {"start", "job\ntask"},
                                                      {"title", "a\"\nend"},
                                                      {"title", "a;task"},
                                                      {"title", "a#b"},
                                                      {"memory_mb", 0},
                                                      {"memory_mb", 64.5},
                                                      {"scf_maxiter", -1},
                                                      {"basis_functions", "5d"},
                                                      {"basis_functions", true},
                                                      {"dielectric", 0.2},
                                                      {"geometry_units", "nm"},
                                                      {"functional", QJsonValue::Null}};
        for (const auto &entry : bad) {
            auto d = water();
            set(d, entry.first, entry.second);
            QVERIFY2(!validateNwchem(d).errors.isEmpty(), qPrintable(entry.first));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
        }
        auto d = water();
        d.data["nwchem"] = "invalid";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
        d = water();
        d.charge = 2000;
        QVERIFY(errorContains(d, "整数"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, setNwchemSetting(d, "charge", 0.5));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(Document()));
    }
    void tddftInput() {
        auto d = water();
        set(d, "method", "dft");
        set(d, "tddft", true);
        set(d, "tddft_roots", 5);
        auto text = nwchemInput(d);
        QVERIFY(text.contains("\ntddft\n  nroots 5\n  notriplet\nend\n"));
        QVERIFY(text.endsWith("task tddft energy\n"));
        QVERIFY(!text.contains("task dft energy"));
        set(d, "tddft_triplets", true);
        set(d, "tddft_tda", true);
        QVERIFY(nwchemInput(d).contains("\ntddft\n  nroots 5\n  cis\nend\n"));
        // Open shells: unrestricted excited states, no singlet/triplet keywords.
        auto cation = d;
        cation.charge = 1;
        cation.multiplicity = 2;
        QVERIFY(nwchemInput(cation).contains("  odft\n"));
        QVERIFY(nwchemInput(cation).contains("\ntddft\n  nroots 5\n  cis\nend\n"));
        QVERIFY(validateNwchem(cation).warnings.join('\n').contains("一重項・三重項を分けずに"));
        // Supported only for single-point DFT without meta-GGA kernels (NWChem 7.3.1).
        set(d, "functional", "m06-2x");
        QVERIFY(errorContains(d, "メタGGA"));
        set(d, "functional", "cam-b3lyp");
        QVERIFY(validateNwchem(d).errors.isEmpty());
        // An excited state's optimization: its CI vectors and the gradient of the chosen root.
        set(d, "tddft_triplets", false);
        set(d, "tddft_tda", false);
        set(d, "task", "optimize");
        set(d, "tddft_root", 2);
        QVERIFY2(validateNwchem(d).errors.isEmpty(), qPrintable(validateNwchem(d).errors.join('\n')));
        auto input = nwchemInput(d);
        QVERIFY(input.contains("  civecs\n  target 2\n  grad\n    root 2\n  end\nend\n"));
        QVERIFY(input.contains("\ndriver\n"));
        QVERIFY(input.contains("task tddft optimize\n") && !input.contains("task dft optimize"));
        QCOMPARE(nwchemTaskLines(input).size(), 1);
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("励起状態 2 の構造を最適化"));
        auto read = nwchemSettings(importNwchemInput(input).document);
        QCOMPARE(read["task"].toString(), QString("optimize"));
        QVERIFY(read["tddft"].toBool());
        QCOMPARE(read["tddft_root"].toInt(), 2);
        set(d, "tddft_root", 6);
        QVERIFY(errorContains(d, "励起状態の数（5）以下"));
        set(d, "tddft_root", 1);
        set(d, "tddft_triplets", true);
        QVERIFY(errorContains(d, "一重項の励起状態"));
        set(d, "tddft_triplets", false);
        set(d, "task", "saddle");
        QVERIFY(errorContains(d, "一点エネルギー"));
        set(d, "task", "energy");
        set(d, "method", "hf");
        QVERIFY(errorContains(d, "TDDFTはDFT"));
        set(d, "method", "dft");
        set(d, "tddft_roots", 0);
        QVERIFY(errorContains(d, "tddft_roots"));
    }
    void solventRelativityAndFitting() {
        auto d = water();
        set(d, "method", "dft");
        set(d, "solvent", "smd");
        QVERIFY(errorContains(d, "溶媒名"));
        set(d, "solvent_name", "acetntrl");
        QVERIFY(nwchemInput(d).contains("\ncosmo\n  do_cosmo_smd true\n  solvent acetntrl\nend\n"));
        QVERIFY(nwchemNumericalFrequencies(d));
        set(d, "method", "hf");
        QVERIFY(errorContains(d, "SMDはDFT専用"));
        set(d, "solvent", "cosmo");
        QVERIFY(nwchemInput(d).contains("\ncosmo\n  solvent acetntrl\nend\n"));
        set(d, "solvent_name", "dielectric");
        QVERIFY(nwchemInput(d).contains("\ncosmo\n  dielec 78.4\nend\n"));
        // Scalar-relativistic Hamiltonians need all-electron bases; NWChem's ZORA is DFT-only.
        auto r = water();
        set(r, "basis", "x2c-svpall");
        set(r, "relativistic", "zora");
        QVERIFY(errorContains(r, "ZORAはDFT専用"));
        set(r, "relativistic", "dkh");
        QVERIFY(nwchemInput(r).contains("\nrelativistic\n  douglas-kroll dkh\nend\n"));
        QVERIFY(nwchemInput(r).contains("\nbasis spherical\n  * library \"x2c-svpall\"\nend\n"));
        QVERIFY(nwchemNumericalFrequencies(r));
        auto heavy = Document::fromXyz("2\nhydrogen iodide\nH 0 0 0\nI 0 0 1.61\n");
        set(heavy, "basis", "def2-svp");
        set(heavy, "relativistic", "x2c");
        QVERIFY(errorContains(heavy, "ECPと併用"));
        set(heavy, "basis", "x2c-svpall");
        QVERIFY(validateNwchem(heavy).errors.isEmpty());
        QVERIFY(nwchemInput(heavy).contains("\nrelativistic\n  x2c on\nend\n"));
        set(heavy, "basis", "3-21g");
        QVERIFY(validateNwchem(heavy).warnings.join('\n').contains("相対論用の基底"));
        // RI-J: Weigend's Coulomb fitting set, also on ghost centers.
        auto fit = water();
        set(fit, "method", "dft");
        set(fit, "basis", "def2-svp");
        set(fit, "ri_j", true);
        fit.atoms.append(coordinateAtom("He", {"0", "0", "3"}, "", CoordinateUnit::Angstrom, true));
        const auto text = nwchemInput(fit);
        QVERIFY(text.contains("\nbasis \"cd basis\" spherical\n  * library \"weigend coulomb fitting\"\n"
                              "  bqHe library He \"weigend coulomb fitting\"\nend\n"));
        QVERIFY(nwchemNumericalFrequencies(fit));
        set(fit, "method", "hf");
        QVERIFY(errorContains(fit, "RI-J"));
        auto cerium = Document::fromXyz("1\ncerium\nCe 0 0 0\n");
        set(cerium, "method", "dft");
        set(cerium, "basis", "x2c-svpall");
        set(cerium, "ri_j", true);
        QVERIFY(errorContains(cerium, "フィッティング基底の対応元素に含まれません: Ce"));
        set(fit, "method", "dft");
        set(fit, "grid", "huge");
        QVERIFY(nwchemInput(fit).contains("  grid huge\n"));
    }
    void pastedBasesAndEcps() {
        // As the Basis Set Exchange writes them for NWChem: a header, comments, Fortran exponents.
        const QString hydrogen =
            "#BASIS SET: (3s) -> [1s]\nBASIS \"ao basis\" SPHERICAL PRINT\n"
            "#BASIS SET: (3s) -> [1s]\nh    S\n      0.3425250914D+01       0.1543289673D+00\n"
            "      0.6239137298D+00       0.5353281423D+00\n"
            "      0.1688554040D+00       0.4446345422D+00\nEND\n";
        auto read = nwchemPastedBasis(hydrogen, false);
        QVERIFY2(read.error.isEmpty(), qPrintable(read.error));
        QCOMPARE(read.elements, QStringList{"H"});
        QCOMPARE(read.lines["H"].first(), QString("  H S"));
        QCOMPARE(read.lines["H"].size(), 4);
        for (const auto &[text, ecp, problem] :
             {std::tuple{QString("0.1 0.2\nH S\n0.1 0.2\n"), false, QString("1行目")},
              std::tuple{QString("Xx S\n0.1 0.2\n"), false, QString("元素記号ではありません")},
              std::tuple{QString("H S\nH P\n0.1 0.2\n"), false, QString("数値の行がありません")},
              std::tuple{QString("H S\n0.1\n"), false, QString("2個以上")},
              std::tuple{QString("task shell ls\n"), false, QString("元素記号ではありません")},
              std::tuple{QString("O ul\n2 1.0 0.0\n"), true, QString("nelec")},
              std::tuple{QString("O nelec 8\nO ul\n2 1.0 0.0\n"), true, QString("原子番号より小さい")},
              std::tuple{QString("O nelec 2\nO ul\n2 1.0\n"), true, QString("3個の数値")}}) {
            const auto bad = nwchemPastedBasis(text, ecp);
            QVERIFY2(bad.error.contains(problem), qPrintable(text + " → " + bad.error));
            QVERIFY(bad.lines.isEmpty());
        }
        // The pasted basis replaces the library's for its element, also for its ghost.
        auto d = water();
        d.atoms << d.atoms[1];
        d.atoms.last().id = newId();
        d.atoms.last().ghost = true;
        d.atoms.last().position += Vec(0, 3, 0);
        set(d, "basis", "6-31g*");
        set(d, "basis_custom", hydrogen);
        QVERIFY2(validateNwchem(d).errors.isEmpty(), qPrintable(validateNwchem(d).errors.join('\n')));
        auto input = nwchemInput(d);
        QVERIFY(input.contains("\n  H S\n    0.3425250914D+01 0.1543289673D+00\n"));
        QVERIFY(input.contains("\n  O library \"6-31g*\"\n"));
        QVERIFY(input.contains("\n  bqH S\n    0.3425250914D+01"));
        QVERIFY(!input.contains("* library"));
        QVERIFY(nwchemBasisLabel(nwchemSettings(d)).contains("貼り付け: H"));
        // A pasted ECP: its core electrons, and numerical frequencies as for the library's ECPs.
        d = water();
        set(d, "ecp_custom", "O nelec 2\nO ul\n2 1.0 0.0\nO S\n2 1.0 0.0\n");
        QCOMPARE(validateNwchem(d).electrons, 8);
        QCOMPARE(validateNwchem(d).ecpElectrons, 2);
        QVERIFY(nwchemInput(d).contains(
            "\necp\n  O nelec 2\n  O ul\n    2 1.0 0.0\n  O S\n    2 1.0 0.0\nend\n"));
        set(d, "task", "frequencies");
        QVERIFY(nwchemNumericalFrequencies(d));
        // An element with a pasted basis takes no library ECP; pasted entries of absent elements are left
        // out.
        auto iodide = Document::fromXyz("2\nHI\nH 0 0 0\nI 0 0 1.6\n");
        set(iodide, "basis", "def2-svp");
        QCOMPARE(validateNwchem(iodide).ecpElectrons, 28);
        set(iodide, "basis_custom", "I S\n1.0 1.0\nPt S\n1.0 1.0\n");
        QCOMPARE(validateNwchem(iodide).ecpElectrons, 0);
        QCOMPARE(validateNwchem(iodide).electrons, 54);
        QVERIFY(!nwchemInput(iodide).contains("\necp\n"));
        QVERIFY(!nwchemInput(iodide).contains("Pt S"));
        QVERIFY(validateNwchem(iodide).warnings.join('\n').contains("分子にない元素（Pt）"));
        set(iodide, "basis_custom", "I S\n1.0\n");
        QVERIFY(errorContains(iodide, "貼り付けた基底の2行目"));
        // Read back from an input; the follow-up calculation is all library bases.
        d = water();
        set(d, "basis_custom", hydrogen);
        set(d, "ecp_custom", "O nelec 2\nO ul\n2 1.0 0.0\n");
        const auto imported = nwchemSettings(importNwchemInput(nwchemInput(d)).document);
        QCOMPARE(nwchemPastedBasis(imported["basis_custom"].toString(), false).lines,
                 nwchemPastedBasis(hydrogen, false).lines);
        QCOMPARE(nwchemPastedBasis(imported["ecp_custom"].toString(), true).cores["O"], 2);
        set(d, "ecp_custom", "");
        set(d, "task", "optimize");
        set(d, "followup", true);
        set(d, "followup_method", "hf");
        set(d, "followup_basis", "sto-3g");
        const auto followup = nwchemInput(d).section("# Follow-up", 1);
        QVERIFY(followup.contains("* library \"sto-3g\"") && !followup.contains("H S"));
    }
    void freeInternalCoordinates() {
        auto d = Document::fromXyz("4\nH2O2\nO 0 0.7 0\nO 0 -0.7 0\nH 0.9 0.9 0.3\nH -0.9 -0.9 0.3\n");
        const auto ids = d.ids();
        set(d, "task", "optimize");
        set(d, "constraints",
            QJsonArray{QJsonObject{{"atoms", idsToJson({ids[2], ids[0], ids[1], ids[3]})}, {"free", true}}});
        QVERIFY2(validateNwchem(d).errors.isEmpty(), qPrintable(validateNwchem(d).errors.join('\n')));
        auto input = nwchemInput(d);
        QVERIFY(input.contains("  zcoord\n    torsion 3 1 2 4\n  end\n"));
        QVERIFY(!input.contains("constant") && !input.contains("noautoz"));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("加えて最適化"));
        set(d, "task", "optimize_frequencies");
        QVERIFY(!validateNwchem(d).warnings.join('\n').contains("停留点ではない"));
        // With a constrained one: both, numbered as the table lists them.
        set(d, "constraints",
            QJsonArray{QJsonObject{{"atoms", idsToJson({ids[2], ids[0], ids[1], ids[3]})}, {"free", true}},
                       QJsonObject{{"atoms", idsToJson({ids[0], ids[1]})}, {"value", 1.45}}});
        input = nwchemInput(d);
        QVERIFY(input.contains("    torsion 3 1 2 4\n    bond 1 2 1.45 mfc2 constant\n"));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("停留点ではない"));
        set(d, "constraints",
            QJsonArray{QJsonObject{{"atoms", idsToJson({ids[2], ids[2], ids[1], ids[3]})}, {"free", true}}});
        QVERIFY(errorContains(d, "内部座標1: 同じ原子"));
    }
    void twoDimensionalScans() {
        auto d = Document::fromXyz("8\nethane\nC 0.7554 0 0\nC -0.7554 0 0\nH 1.1367 0 1.0279\n"
                                   "H 1.1367 0.8902 -0.5140\nH 1.1367 -0.8902 -0.5140\nH -1.1367 0 -1.0279\n"
                                   "H -1.1367 -0.8902 0.5140\nH -1.1367 0.8902 0.5140\n");
        const auto ids = d.ids();
        set(d, "task", "scan");
        set(d, "scan_atoms", idsToJson({ids[0], ids[1]}));
        set(d, "scan_start", 1.45);
        set(d, "scan_end", 1.65);
        set(d, "scan_steps", 3);
        set(d, "scan2_atoms", idsToJson({ids[2], ids[0], ids[1], ids[5]}));
        set(d, "scan2_start", 60.0);
        set(d, "scan2_end", 180.0);
        set(d, "scan2_steps", 3);
        QVERIFY2(validateNwchem(d).errors.isEmpty(), qPrintable(validateNwchem(d).errors.join('\n')));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("3 × 3 = 9 点"));
        const auto input = nwchemInput(d);
        QCOMPARE(nwchemTaskLines(input).size(), 9);
        QVERIFY(input.contains("title \"MolForma scan 4/9 bond 1 2 = 1.55 and torsion 3 1 2 6 = 60\""));
        QVERIFY(
            input.contains("    bond 1 2 1.55 mfscan constant\n    torsion 3 1 2 6 60 mfscan2 constant\n"));
        // Each point's starting structure holds both values.
        const auto points = input.split("\ngeometry scan");
        QCOMPARE(points.size(), 10);
        // Rigid points hold them exactly.
        set(d, "scan_relaxed", false);
        QVERIFY(validateNwchem(d).errors.isEmpty());
        // At most 400 points; two different coordinates that do not move each other.
        set(d, "scan_steps", 21);
        set(d, "scan2_steps", 20);
        QVERIFY(errorContains(d, "400点まで"));
        set(d, "scan_steps", 3);
        set(d, "scan2_steps", 3);
        set(d, "scan2_atoms", idsToJson({ids[1], ids[0]}));
        set(d, "scan2_start", 1.4);
        set(d, "scan2_end", 1.6);
        QVERIFY(errorContains(d, "別の座標"));
        // Both angles turn the same hydrogen: no structure holds both values.
        set(d, "scan_atoms", idsToJson({ids[1], ids[0], ids[2]}));
        set(d, "scan_start", 100.0);
        set(d, "scan_end", 120.0);
        set(d, "scan2_atoms", idsToJson({ids[3], ids[0], ids[2]}));
        set(d, "scan2_start", 100.0);
        set(d, "scan2_end", 120.0);
        QVERIFY(errorContains(d, "同じ原子を動かす"));
        // Deleting an atom of the second coordinate drops it; the scan becomes one-dimensional.
        auto smaller = d;
        smaller.remove({ids[3]});
        QVERIFY(nwchemSettings(smaller)["scan2_atoms"].toArray().isEmpty());
    }
    void fragmentInitialOrbitals() {
        const auto dimer = Document::fromXyz("6\ndimer\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n"
                                             "O 0 0 3\nH 0.758 0 3.504\nH -0.758 0 3.504\n");
        const auto ids = dimer.ids();
        const auto fragments = [&](const QVector<std::tuple<Ids, int, int>> &list) {
            QJsonArray array;
            for (const auto &[atoms, charge, multiplicity] : list)
                array << QJsonObject{
                    {"atoms", idsToJson(atoms)}, {"charge", charge}, {"multiplicity", multiplicity}};
            return array;
        };
        auto d = dimer;
        set(d, "method", "dft");
        set(d, "fragment_guess", fragments({{ids.mid(0, 3), 0, 1}, {ids.mid(3), 0, 1}}));
        QVERIFY2(validateNwchem(d).errors.isEmpty(), qPrintable(validateNwchem(d).errors.join('\n')));
        auto input = nwchemInput(d);
        QVERIFY(input.contains("\ngeometry frag1 units angstrom") &&
                input.contains("\ngeometry frag2 units"));
        QVERIFY(input.contains("set geometry frag1\ncharge 0\n"));
        QVERIFY(input.contains("dft\n  mult 1\n  vectors input atomic output molforma_frag2.movecs\nend\n"));
        QVERIFY(input.contains("\nset geometry geometry\ncharge 0\ndft\n  mult 1\n  vectors input fragment "
                               "molforma_frag1.movecs "
                               "molforma_frag2.movecs\nend\n"));
        QCOMPARE(nwchemTaskLines(input),
                 (QStringList{"task dft energy", "task dft energy", "task dft energy"}));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("フラグメント 2 個"));
        // HF: the reference of the molecule for each fragment.
        set(d, "method", "hf");
        QVERIFY(nwchemInput(d).contains(
            "scf\n  rhf\n  nopen 0\n  vectors input atomic output molforma_frag1.movecs\nend\n"));
        // High spin from two open-shell fragments: their unpaired electrons add up.
        set(d, "method", "dft");
        d.charge = 2;
        d.multiplicity = 3;
        set(d, "fragment_guess", fragments({{ids.mid(0, 3), 1, 2}, {ids.mid(3), 1, 2}}));
        QVERIFY2(validateNwchem(d).errors.isEmpty(), qPrintable(validateNwchem(d).errors.join('\n')));
        QVERIFY(nwchemInput(d).contains("dft\n  mult 2\n  odft\n  vectors input atomic"));
        // Broken symmetry (two doublets in a singlet) is what NWChem refuses.
        d.multiplicity = 1;
        QVERIFY(errorContains(d, "broken-symmetry"));
        d = dimer;
        set(d, "fragment_guess", fragments({{ids.mid(0, 3), 0, 1}, {ids.mid(3, 2), 0, 1}}));
        QVERIFY(errorContains(d, "含まれない原子が 1 個"));
        set(d, "fragment_guess", fragments({{ids.mid(0, 4), 0, 1}, {ids.mid(3), 0, 1}}));
        QVERIFY(errorContains(d, "2つのフラグメント"));
        set(d, "fragment_guess", fragments({{ids.mid(0, 3), 1, 2}, {ids.mid(3), 0, 1}}));
        QVERIFY(errorContains(d, "電荷の合計 1"));
        set(d, "fragment_guess", fragments({{ids.mid(0, 3), 0, 2}, {ids.mid(3), 0, 1}}));
        QVERIFY(errorContains(d, "フラグメント1の電子数 10 と多重度 2"));
        set(d, "fragment_guess", fragments({{ids, 0, 1}}));
        QVERIFY(errorContains(d, "2〜20個"));
        set(d, "fragment_guess", fragments({{ids.mid(0, 3), 0, 1}, {ids.mid(3), 0, 1}}));
        set(d, "task", "scan");
        QVERIFY(errorContains(d, "座標スキャン・IRC・NEB・BSSE補正と併用できません"));
        set(d, "task", "energy");
        setNwchemAdvanced(d, "vectors_guess", "hcore");
        QVERIFY(errorContains(d, "同時に指定できません"));
        // Deleting atoms takes them out of their fragment; a fragment without atoms goes.
        d = dimer;
        set(d, "fragment_guess", fragments({{ids.mid(0, 3), 0, 1}, {ids.mid(3), 0, 1}}));
        d.remove(ids.mid(3));
        QCOMPARE(nwchemSettings(d)["fragment_guess"].toArray().size(), 1);
        d.remove({ids[2]});
        QCOMPARE(idsFromJson(nwchemSettings(d)["fragment_guess"].toArray()[0].toObject()["atoms"]).size(), 2);
    }
    void spinSpinCouplings() {
        auto d =
            Document::fromXyz("5\nCH3F\nC 0 0 0\nF 0 0 1.383\nH 1.0267 0 -0.363\nH -0.5134 -0.8892 -0.363\n"
                              "H -0.5134 0.8892 -0.363\n");
        set(d, "method", "dft");
        set(d, "prop_spinspin", true);
        QVERIFY2(validateNwchem(d).errors.isEmpty(), qPrintable(validateNwchem(d).errors.join('\n')));
        QCOMPARE(nwchemSpinSpinPairs(d).size(), 6); // F and three H.
        auto input = nwchemInput(d);
        QVERIFY(input.contains(
            "\nunset prop:*\ndft\n  odft\nend\nproperty\n  spinspin 6 2 3 2 4 2 5 3 4 3 5 4 5\nend\n"
            "task dft property\n"));
        QVERIFY(input.endsWith("task dft property\n"));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("6 組"));
        set(d, "spinspin_pairs", "all");
        QCOMPARE(nwchemSpinSpinPairs(d).size(), 10);
        QVERIFY(nwchemInput(d).contains("property\n  spinspin\nend\n"));
        // An open shell is UKS already; HF is refused.
        auto cation = d;
        cation.charge = 1;
        cation.multiplicity = 2;
        QVERIFY(!nwchemInput(cation).section("unset prop:*", 1).contains("odft"));
        set(d, "method", "hf");
        QVERIFY(errorContains(d, "DFTのとき"));
        set(d, "method", "dft");
        set(d, "spinspin_pairs", "hfp");
        auto methane = Document::fromXyz("1\nC\nC 0 0 0\n");
        set(methane, "method", "dft");
        set(methane, "prop_spinspin", true);
        QVERIFY(errorContains(methane, "H・F・Pの原子が2個以上"));
        // Read back from the input.
        const auto read = nwchemSettings(importNwchemInput(nwchemInput(d)).document);
        QVERIFY(read["prop_spinspin"].toBool());
        QCOMPARE(read["spinspin_pairs"].toString(), QString("hfp"));
        QVERIFY(!importNwchemInput(nwchemInput(d)).notes.join('\n').contains("後続の計算"));
    }
    void pathsConstraintsAndScans() {
        auto d = water();
        const auto ids = d.ids();
        set(d, "task", "saddle_frequencies");
        auto text = nwchemInput(d);
        QVERIFY(text.contains("\ndriver\n"));
        QVERIFY(text.contains("\ntask scf saddle\ntask scf frequencies\n"));
        set(d, "task", "irc");
        set(d, "irc_points", 12);
        set(d, "irc_stride", 0.05);
        text = nwchemInput(d);
        QVERIFY(text.contains("\nmepgs\n  maxmep 12\n  maxiter 100\n  stride 0.05\nend\n"));
        QVERIFY(text.contains("\ntask scf mepgs\n"));
        QVERIFY(!text.contains("\ndriver\n"));
        // One direction only: NWChem judges a path converged only beyond 1 amu½·Bohr, so a short side
        // can stop the run before the other is traced.
        set(d, "irc_direction", "backward");
        text = nwchemInput(d);
        QVERIFY(text.contains("\nmepgs\n  maxmep 12\n  maxiter 100\n  stride 0.05\n  backward\nend\n"));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("指定した方向だけ"));
        QCOMPARE(importNwchemInput(text).document.data["nwchem"].toObject()["irc_direction"].toString(),
                 QString("backward"));
        set(d, "irc_direction", "sideways");
        QVERIFY(validateNwchem(d).errors.join('\n').contains("irc_direction"));
        set(d, "irc_direction", "both");
        // ZCOORD constraints switch AUTOZ on; distances follow the GEOMETRY units.
        set(d, "task", "optimize");
        set(d, "constraints",
            QJsonArray{QJsonObject{{"atoms", idsToJson({ids[0], ids[1]})}, {"value", 1.0}},
                       QJsonObject{{"atoms", idsToJson({ids[1], ids[0], ids[2]})}, {"value", 110.0}}});
        text = nwchemInput(d);
        QVERIFY(!text.contains("noautoz"));
        QVERIFY(text.contains(
            "  zcoord\n    bond 1 2 1 mfc1 constant\n    angle 2 1 3 110 mfc2 constant\n  end\nend\n"));
        set(d, "geometry_units", "bohr");
        QVERIFY(nwchemInput(d).contains(
            QString("    bond 1 2 %1 mfc1 constant\n").arg(QString::number(1 / BohrToAngstrom, 'g', 17))));
        set(d, "geometry_units", "angstrom");
        auto fixed = d;
        fixed.atoms[2].fixed = true;
        QVERIFY(errorContains(fixed, "固定原子・ゴーストと併用できません"));
        set(d, "task", "energy");
        QVERIFY(errorContains(d, "構造最適化・遷移状態探索・緩和スキャン"));
        set(d, "task", "optimize");
        set(d, "constraints",
            QJsonArray{QJsonObject{{"atoms", idsToJson({ids[1], ids[0], ids[2]})}, {"value", 190.0}}});
        QVERIFY(errorContains(d, "内部座標1: 角度は"));
        // Deleting an atom drops the constraints and the scan coordinate that refer to it.
        set(d, "constraints",
            QJsonArray{QJsonObject{{"atoms", idsToJson({ids[0], ids[1]})}, {"value", 1.0}},
                       QJsonObject{{"atoms", idsToJson({ids[0], ids[2]})}, {"value", 1.0}}});
        set(d, "scan_atoms", idsToJson({ids[0], ids[1]}));
        auto removed = d;
        removed.remove({ids[1]});
        QCOMPARE(nwchemSettings(removed)["constraints"].toArray().size(), 1);
        QVERIFY(nwchemSettings(removed)["scan_atoms"].toArray().isEmpty());
        // Scans: a named geometry and a title marker per point; the later atom's side moves.
        auto scan = water();
        const auto sids = scan.ids();
        set(scan, "task", "scan");
        QVERIFY(errorContains(scan, "スキャンする座標"));
        set(scan, "scan_atoms", idsToJson({sids[0], sids[1]}));
        set(scan, "scan_start", 0.9);
        set(scan, "scan_end", 1.2);
        set(scan, "scan_steps", 4);
        text = nwchemInput(scan);
        QCOMPARE(text.count("\nset geometry scan"), 4);
        QCOMPARE(text.count("\ntask scf optimize\n"), 4);
        QVERIFY(text.contains("\ntitle \"MolForma scan 4/4 bond 1 2 = 1.2\"\ngeometry scan4 units angstrom"));
        QVERIFY(text.contains("    bond 1 2 1.2 mfscan constant\n"));
        QVERIFY(!text.contains("\ngeometry units"));
        set(scan, "scan_relaxed", false);
        text = nwchemInput(scan);
        QCOMPARE(text.count("\ntask scf energy\n"), 4);
        QVERIFY(!text.contains("zcoord") && !text.contains("\ndriver\n"));
        const auto first = text.mid(text.indexOf("geometry scan1"), 200),
                   last = text.mid(text.indexOf("geometry scan4"), 200);
        QVERIFY(first.section('\n', 1, 1) == last.section('\n', 1, 1)); // Oxygen stays.
        QVERIFY(first.section('\n', 2, 2) != last.section('\n', 2, 2)); // The scanned hydrogen moves.
        set(scan, "scan_end", 60.0);
        QVERIFY(errorContains(scan, "スキャン終了値: 距離は"));
        set(scan, "scan_end", 1.2);
        scan.atoms[1].fixed = true;
        QVERIFY(errorContains(scan, "スキャン: 固定原子"));
    }
    void nebPathInput() {
        auto d = Document::fromXyz("3\nHCN\nC 0 0 0\nN 0 0 1.156\nH -0.9 0 -0.7\n");
        const auto ids = d.ids();
        set(d, "task", "neb");
        QVERIFY(errorContains(d, "NEBの終点の構造を"));
        // The end is kept by atom ID, fitted onto the start: a turned and shifted copy gives the same end.
        const auto product = Document::fromXyz("3\nHNC\nC 0 0 0\nN 0 0 1.17\nH 0.8 0 1.85\n");
        auto turned = product;
        for (auto &a : turned.atoms)
            a.position = Eigen::AngleAxisd(0.7, Vec::UnitY()).toRotationMatrix() * a.position + Vec(1, 2, 3);
        const auto end = nwchemNebEndSetting(d, product, true);
        const auto other = nwchemNebEndSetting(d, turned, true);
        QCOMPARE(end.size(), 3);
        for (const auto &id : ids)
            QVERIFY((vectorFromJson(end[id]) - vectorFromJson(other[id])).norm() < 1e-9);
        // Without fitting, the positions are taken as they are.
        QVERIFY((vectorFromJson(nwchemNebEndSetting(d, product, false)[ids[2]]) - Vec(0.8, 0, 1.85)).norm() <
                1e-12);
        set(d, "neb_end", nwchemNebEndSetting(d, product, false));
        QVERIFY(validateNwchem(d).errors.isEmpty());
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("NEBは始点"));
        auto text = nwchemInput(d);
        QVERIFY(text.contains("\ngeometry units angstrom nocenter noautosym noautoz"));
        QVERIFY(text.contains(
            "\ngeometry endgeom units angstrom nocenter noautosym noautoz"
            " angstrom_to_au 1.8897261246257702\n  C 0 0 0\n  N 0 0 1.17\n  H 0.8 0 1.85\nend\n"));
        QCOMPARE(text.count("constraints"), 1); // Only for the start, which NWChem keeps as "geometry".
        QVERIFY(text.contains(
            "\nneb\n  nbeads 10\n  kbeads 1\n  maxiter 50\n  stepsize 0.1\n  print_shift 1\nend\n"));
        QVERIFY(text.contains("\ntask scf neb\n"));
        QVERIFY(!text.contains("\ndriver\n"));
        QCOMPARE(nwchemSectionUnused(d, "path"), QString());
        // An NWChem NEB input reads back with its end.
        const auto imported = importNwchemInput(text).document;
        QCOMPARE(nwchemSettings(imported)["task"].toString(), QString("neb"));
        QVERIFY(nwchemNebEnd(imported).document);
        QVERIFY((nwchemNebEnd(imported).document->atoms[2].position - Vec(0.8, 0, 1.85)).norm() < 1e-12);
        // NEB's first path is the straight line between the ends.
        const auto path = nwchemNebInitialPath(d, *nwchemNebEnd(d).document, 3);
        QCOMPARE(path.size(), 3);
        QVERIFY((path[1].atoms[2].position - Vec(-0.05, 0, 0.575)).norm() < 1e-12);
        // Atoms added later have no end; deleted ones leave it; renamed IDs follow.
        auto added = d;
        added.atoms << Atom{};
        added.atoms.last().element = "H";
        added.atoms.last().position = Vec(3, 0, 0);
        QVERIFY(errorContains(added, "位置のない原子が 1 個"));
        auto removed = d;
        removed.remove({ids[2]});
        QVERIFY(!nwchemSettings(removed)["neb_end"].toObject().contains(ids[2]));
        auto renamed = d;
        renamed.renameIds({{ids[0], "carbon"}});
        QVERIFY(nwchemSettings(renamed)["neb_end"].toObject().contains("carbon"));
        QVERIFY(nwchemNebEnd(renamed).document.has_value());
        // The same structure at both ends, atoms meeting on the straight line, fixed atoms.
        auto same = d;
        set(same, "neb_end", nwchemNebEndSetting(d, d, false));
        QVERIFY(errorContains(same, "終点が始点"));
        auto through = Document::fromXyz("3\nHCN\nC 0 0 0\nN 0 0 1.156\nH 0 0 -1.07\n");
        set(through, "task", "neb");
        auto across = through;
        across.atoms[2].position = Vec(0, 0, 2.2);
        set(through, "neb_end", nwchemNebEndSetting(through, across, false));
        QVERIFY(validateNwchem(through).warnings.join('\n').contains("近づきます"));
        auto fixed = d;
        fixed.atoms[0].fixed = true;
        QVERIFY(errorContains(fixed, "NEBは固定原子"));
        auto properties = d;
        set(properties, "prop_mulliken", true);
        QVERIFY(errorContains(properties, "物性計算はスキャン・IRC・NEB"));
        QVERIFY_THROWS_EXCEPTION(std::exception, nwchemNebEndSetting(d, water(), true));
        auto swapped = product;
        std::swap(swapped.atoms[0], swapped.atoms[1]);
        QVERIFY_THROWS_EXCEPTION(std::exception, nwchemNebEndSetting(d, swapped, true));
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
    void bsseFollowupPropertiesAndExtraInput() {
        auto dimer = Document::fromXyz("6\nwater dimer\nO -1.551007 -0.114520 0\nH -1.934259 0.762503 0\n"
                                       "H -0.599677 0.040712 0\nO 1.350625 0.111469 0\n"
                                       "H 1.680398 -0.373741 -0.758561\nH 1.680398 -0.373741 0.758561\n");
        QCOMPARE(nwchemBsseMonomers(dimer).size(), 2);
        set(dimer, "bsse", true);
        auto text = nwchemInput(dimer);
        QVERIFY(text.contains(
            "\nbsse\n  mon m1 1 2 3\n  charge 0\n  mult 1\n  mon m2 4 5 6\n  charge 0\n  mult 1\nend\n"));
        // NWChem's BSSE module needs a basis entry for the ghosts it creates.
        QVERIFY(text.contains(
            "  * library \"sto-3g\"\n  bqH library H \"sto-3g\"\n  bqO library O \"sto-3g\"\n"));
        set(dimer, "task", "frequencies");
        QVERIFY(errorContains(dimer, "BSSE補正は一点エネルギー"));
        set(dimer, "task", "energy");
        dimer.multiplicity = 3;
        QVERIFY(errorContains(dimer, "閉殻の単量体"));
        auto single = water();
        set(single, "bsse", true);
        QVERIFY(errorContains(single, "2個以上の分子"));
        // Follow-up single point: method blocks are cleared so first-level options do not leak.
        auto f = water();
        set(f, "method", "dft");
        set(f, "dispersion", "d3bj");
        set(f, "task", "optimize");
        set(f, "followup", true);
        set(f, "followup_functional", "pbe0");
        set(f, "followup_basis", "def2-tzvp");
        text = nwchemInput(f);
        const int follow = text.indexOf("\nunset scf:*\nunset dft:*\n");
        QVERIFY(follow > text.indexOf("\ntask dft optimize\n"));
        QVERIFY(text.mid(follow).contains("\nbasis spherical\n  * library \"def2-tzvp\"\nend\n"));
        QVERIFY(text.mid(follow).contains("  xc pbe0\n"));
        QVERIFY(!text.mid(follow).contains("disp vdw"));
        QVERIFY(text.endsWith("\ntask dft energy\n"));
        set(f, "followup_method", "ccsd");
        set(f, "followup_basis", "cc-pvdz");
        text = nwchemInput(f);
        QVERIFY(text.contains("\nunset ccsd:*\n") && text.endsWith("\ntask ccsd energy\n"));
        set(f, "task", "energy");
        QVERIFY(errorContains(f, "後続の一点計算は構造最適化"));
        auto xenon = Document::fromXyz("1\nxenon\nXe 0 0 0\n");
        set(xenon, "basis", "def2-svp");
        set(xenon, "task", "optimize");
        set(xenon, "followup", true);
        set(xenon, "followup_method", "hf");
        set(xenon, "followup_basis", "x2c-svpall");
        QVERIFY(errorContains(xenon, "ECPを使う基底"));
        // Properties after the last calculation; MOLDEN output clears the requests first.
        auto p = water();
        for (const auto *key : {"prop_mulliken", "prop_nmr", "prop_polarizability", "prop_esp"})
            set(p, key, true);
        text = nwchemInput(p);
        QVERIFY(text.contains(
            "\nproperty\n  mulliken\n  shielding\n  aoresponse 1 0.0\nend\ntask scf property\n"));
        QVERIFY(text.contains("\nesp\n  restrain\nend\ntask esp\n"));
        setNwchemAdvanced(p, "molden_output", true);
        QVERIFY(nwchemInput(p).contains("\nunset prop:*\nproperty\n  vectors"));
        auto later = p; // Properties at a follow-up level are cleared before MOLDEN output too.
        set(later, "task", "optimize");
        set(later, "followup", true);
        set(later, "followup_method", "hf");
        set(later, "followup_basis", "6-31g*");
        text = nwchemInput(later);
        QVERIFY(text.indexOf("task scf property") > text.indexOf("unset dft:*"));
        QVERIFY(text.contains("\nunset prop:*\nproperty\n  vectors"));
        set(p, "method", "mp2");
        QVERIFY(errorContains(p, "物性計算は最後の計算レベル"));
        // Additional input is copied unchecked, except for program starts.
        auto e = water();
        set(e, "extra_input", "set cphf:maxiter 50");
        set(e, "extra_tasks", "task scf property");
        text = nwchemInput(e);
        QVERIFY(text.indexOf("set cphf:maxiter 50") < text.indexOf("\ntask scf energy\n"));
        QVERIFY(text.endsWith("task scf property\n"));
        QVERIFY(validateNwchem(e).warnings.join('\n').contains("検査せずに"));
        set(e, "extra_tasks", "TASK  Shell \"ls\"");
        QVERIFY(errorContains(e, "task shell"));
        set(e, "extra_tasks", "");
        set(e, "extra_input", "python\n  print('x')\nend");
        QVERIFY(errorContains(e, "pythonブロック"));
        // NWChem statements, not physical lines: ";" separators and "\\" continuations.
        // NWChem also reads quoted words without their quotes.
        for (const auto *text : {"print none; task shell echo hi", "task \\\nshell echo hi", "task python",
                                 "task \"shell\" \"echo hi\"", "\"task\" sh\"ell\" echo", "\"task shell\" x",
                                 "task\tshell echo", "\"python\"\n  print('x')\nend"}) {
            set(e, "extra_input", text);
            QVERIFY2(errorContains(e, "task shell"), text);
        }
        set(e, "extra_input", "");
        // Presets keep the method, not atom references or additional input.
        set(e, "constraints",
            QJsonArray{QJsonObject{{"atoms", idsToJson({e.ids()[0], e.ids()[1]})}, {"value", 1.0}}});
        const auto preset = nwchemPresetSettings(nwchemSettings(e));
        QVERIFY(!preset.contains("extra_input") && !preset.contains("constraints") &&
                !preset.contains("scan_atoms"));
        QVERIFY(preset.contains("relativistic") && preset.contains("followup_basis"));
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
        QCOMPARE(nwchemSectionUnused(d, "path"), QString("使わない"));
        QCOMPARE(nwchemSectionUnused(d, "constraints"), QString("最適化のみ"));
        QCOMPARE(nwchemSectionUnused(d, "optimization"), QString("使わない"));
        QCOMPARE(nwchemSectionUnused(d, "bsse"), QString("単量体なし"));
        QCOMPARE(nwchemSectionUnused(d, "excited"), QString("DFTのみ"));
        QVERIFY(nwchemSectionUnused(d, "basic").isEmpty());
        set(d, "method", "dft");
        QVERIFY(nwchemSectionUnused(d, "excited").isEmpty());
        set(d, "task", "optimize_frequencies");
        QCOMPARE(nwchemSectionUnused(d, "excited"), QString("一点・最適化"));
        QVERIFY(nwchemSectionUnused(d, "constraints").isEmpty() &&
                nwchemSectionUnused(d, "optimization").isEmpty());
        set(d, "task", "scan");
        set(d, "scan_relaxed", false);
        QVERIFY(nwchemSectionUnused(d, "path").isEmpty());
        QCOMPARE(nwchemSectionUnused(d, "constraints"), QString("最適化のみ"));

        MainWindow w;
        w.loadDocument(water());
        auto *sections = w.findChild<QListWidget *>("calculationSections");
        const auto entry = [&](const QString &key) -> QListWidgetItem * {
            for (int i = 0; i < sections->count(); ++i)
                if (sections->item(i)->data(Qt::UserRole) == key)
                    return sections->item(i);
            return nullptr;
        };
        QCOMPARE(entry("path")->data(Qt::UserRole + 12).toString(), QString("使わない"));
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
    void nwchemImportRoundTripEverySetting() {
        // Every value of every setting reads back from the input MolForma writes for it, in each
        // calculation that uses the setting (IRC writes the DRIVER maxiter in MEPGS). Not read from inputs:
        // the settings of later tasks (follow-up calculations), scans (one geometry per point), point charges
        // (bq) and the additional input.
        const QStringList notImported{"followup",
                                      "followup_method",
                                      "followup_functional",
                                      "followup_dispersion",
                                      "followup_basis",
                                      "task=scan",
                                      "scan_start",
                                      "scan_end",
                                      "scan_steps",
                                      "scan_relaxed",
                                      "scan2_start",
                                      "scan2_end",
                                      "scan2_steps",
                                      "include_point_charges",
                                      "extra_input",
                                      "extra_tasks"};
        using Context = QVector<std::pair<const char *, QJsonValue>>;
        const QVector<Context> contexts{
            {},
            {{"method", "dft"}},
            {{"method", "dft"},
             {"functional", "custom"},
             {"xc_custom", "xpbe96 0.75 cpbe96 1.0 hfexch 0.25"}},
            {{"method", "dft"}, {"tddft", true}},
            {{"method", "dft"}, {"task", "optimize"}, {"tddft", true}, {"tddft_roots", 3}},
            {{"method", "dft"}, {"prop_spinspin", true}},
            {{"method", "dft"}, {"task", "frequencies"}, {"prop_raman", true}},
            {{"method", "dft"}, {"prop_ord", true}},
            {{"prop_esp_grid", true}},
            {{"method", "mp2"}},
            {{"method", "ccsd"}},
            {{"task", "optimize"}},
            {{"task", "frequencies"}},
            {{"task", "irc"}},
            {{"task", "scan"}, {"scan_atoms", "<atoms>"}},
            {{"task", "neb"}, {"neb_end", "<end>"}},
            {{"task", "optimize"}, {"followup", true}},
            {{"solvent", "cosmo"}},
            {{"basis", "x2c-svpall"}}};
        QStringList failures;
        for (const auto &spec : nwchemSettingSpecs()) {
            if (spec.structural || notImported.contains(spec.key))
                continue;
            QVector<QJsonValue> values;
            switch (spec.kind) {
            case NwchemSettingKind::Choice:
                for (const auto &c : spec.choices)
                    if (c.value != spec.initial.toString())
                        values << c.value;
                break;
            case NwchemSettingKind::Flag:
                values << !spec.initial.toBool();
                break;
            case NwchemSettingKind::Integer:
                values << int(spec.minimum) + 1;
                break;
            case NwchemSettingKind::Real:
                values << spec.minimum + (spec.maximum - spec.minimum) / 4;
                break;
            case NwchemSettingKind::String:
                values << QJsonValue(spec.key == "start"          ? "roundtrip"
                                     : spec.key == "title"        ? "Round trip"
                                     : spec.key == "basis_custom" ? "  H S\n    3.42525091 0.15432897\n"
                                                                    "    0.62391373 0.53532814\n"
                                     : spec.key == "ecp_custom"
                                         ? "  O nelec 2\n  O ul\n    2 1.0 0.0\n  O S\n    2 1.0 0.0\n"
                                         : "xpbe96 0.75 cpbe96 1.0 hfexch 0.25");
                break;
            case NwchemSettingKind::Object:
            case NwchemSettingKind::Array:
                continue;
            }
            for (const auto &value : values) {
                const auto key = spec.key + "=" + nwchemAdvancedText(value);
                if (notImported.contains(key))
                    continue;
                // Bases of heavy elements, and counterpoise monomers.
                const auto base =
                    value.toString().startsWith("sarc-")
                        ? Document::fromXyz("2\ngold\nAu 0 0 0\nAu 0 0 2.5\n")
                    : spec.key == "bsse"
                        ? Document::fromXyz("6\ndimer\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 "
                                            "0.504\nO 0 0 3\nH 0.758 0 3.504\nH -0.758 0 3.504\n")
                        : water();
                // Every one of these calculations that uses the setting, except those not read back.
                int tested = 0;
                for (const auto &context : contexts) {
                    auto d = base;
                    QStringList named;
                    bool skipped = false;
                    for (const auto &[setting, contextValue] : context) {
                        if (contextValue == "<end>") { // The end of an NEB: the hydrogens moved.
                            auto end = d;
                            for (auto &a : end.atoms)
                                if (a.element == "H")
                                    a.position.z() -= 1;
                            set(d, setting, nwchemNebEndSetting(d, end, false));
                        } else
                            set(d, setting,
                                contextValue == "<atoms>"
                                    ? QJsonValue(QJsonArray{d.atoms[0].id, d.atoms[1].id})
                                    : contextValue);
                        named << QString(setting) + "=" + nwchemAdvancedText(contextValue);
                        skipped |= notImported.contains(setting) || notImported.contains(named.last());
                    }
                    if (skipped || !nwchemSettingApplies(spec, nwchemSettings(d)))
                        continue;
                    const auto where = named.isEmpty() ? key : key + " with " + named.join(", ");
                    set(d, spec.key, value);
                    // Values that need another setting with them.
                    if (spec.key == "prop_epr") {
                        d.charge = 1;
                        d.multiplicity = 2;
                        set(d, "method", "dft");
                        set(d, "relativistic", "zora");
                    }
                    if (value == "custom")
                        set(d, "xc_custom", "xpbe96 0.75 cpbe96 1.0 hfexch 0.25");
                    if (value == "zora" || value == "smd" || value == "sarc-zora")
                        set(d, "method", "dft");
                    if (value == "smd")
                        set(d, "solvent_name", "h2o");
                    if (value.toString().startsWith("sarc-"))
                        set(d, "relativistic", value == "sarc-zora" ? "zora" : "dkh");
                    if (!validateNwchem(d).errors.isEmpty())
                        continue; // A combination MolForma refuses.
                    ++tested;
                    // Nothing tells the value from the default when the input is the same (RHF in DFT).
                    auto initial = d;
                    set(initial, spec.key, spec.initial);
                    const auto input = nwchemInput(d);
                    if (validateNwchem(initial).errors.isEmpty() && input == nwchemInput(initial))
                        continue;
                    const auto imported = nwchemSettings(importNwchemInput(input).document);
                    const bool realMatches = spec.kind == NwchemSettingKind::Real &&
                                             imported[spec.key].isDouble() &&
                                             std::abs(imported[spec.key].toDouble() - value.toDouble()) <=
                                                 1e-8 * std::max(1., std::abs(value.toDouble()));
                    if (imported[spec.key] != value && !realMatches)
                        failures << where + " (read " + nwchemAdvancedText(imported[spec.key]) + ")";
                }
                if (!tested)
                    failures << key + " (no valid calculation uses it)";
            }
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join('\n')));
    }
    void nwchemImportRoundTrip() {
        // MolForma's own inputs read back to the same structure and settings.
        auto d = water();
        d.charge = 1;
        d.multiplicity = 2;
        for (const auto &[key, value] : {std::pair<const char *, QJsonValue>{"method", "dft"},
                                         {"functional", "pbe0"},
                                         {"dispersion", "d3bj"},
                                         {"basis", "def2-tzvp"},
                                         {"task", "optimize_frequencies"},
                                         {"solvent", "smd"},
                                         {"solvent_name", "acetntrl"},
                                         {"grid", "xfine"},
                                         {"relativistic", "dkh"},
                                         {"opt_maxiter", 40},
                                         {"start", "roundtrip"},
                                         {"title", "Round trip"},
                                         {"memory_mb", 2048}})
            set(d, key, value);
        set(d, "relativistic", "none"); // Relativity needs all-electron bases; tested separately.
        const auto imported = importNwchemInput(nwchemInput(d));
        const auto &r = imported.document;
        QCOMPARE(r.atoms.size(), d.atoms.size());
        for (int i = 0; i < d.atoms.size(); ++i)
            QVERIFY((r.atoms[i].position - d.atoms[i].position).norm() < 1e-9);
        QCOMPARE(r.charge, 1);
        QCOMPARE(r.multiplicity, 2);
        const auto a = nwchemSettings(d), b = nwchemSettings(r);
        for (const auto *key :
             {"method", "functional", "dispersion", "basis", "task", "solvent", "solvent_name", "grid",
              "opt_maxiter", "start", "title", "memory_mb", "reference"})
            QVERIFY2(a[key] == b[key] || (QString(key) == "reference" && b[key] == "uhf"), key);
        QVERIFY2(imported.notes.isEmpty(), qPrintable(imported.notes.join('\n')));
        // A hand-written input: comments, ";", continuations, units, ghosts, masses and other blocks.
        const auto other = importNwchemInput(
            "start h2o # comment\n"
            "geometry units bohr noautoz\n  symmetry c1\n  O 0 0 0\n  H1 1.43 0 \\\n 1.1 mass 2.014\n"
            "  H2 -1.43 0 1.1\n  bqHe 0 0 6\nend\n"
            "basis cartesian; * library 6-31g*; O library cc-pvdz; bqHe library He 6-31g*; end\n"
            "scf; uhf; triplet; end\nnwpw\n  cutoff 30\nend\nconstraints\n  clear\n  fix atom 1 3:4\nend\n"
            "task scf optimize\ntask scf freq\ntask esp\n");
        const auto &o = other.document;
        QCOMPARE(o.atoms.size(), 4);
        QVERIFY(std::abs(o.atoms[1].position.z() - 1.1 * BohrToAngstrom) < 1e-9);
        QCOMPARE(o.atoms[1].mass, 2.014);
        QVERIFY(o.atoms[0].fixed && !o.atoms[1].fixed && o.atoms[2].fixed && o.atoms[3].fixed);
        QVERIFY(o.atoms[3].ghost && o.atoms[3].element == "He");
        const auto os = nwchemSettings(o);
        QCOMPARE(os["basis"].toString(), QString("6-31g*"));
        QCOMPARE(os["basis_overrides"].toObject()["O"].toString(), QString("cc-pvdz"));
        QCOMPARE(os["basis_functions"].toString(), QString("cartesian"));
        QCOMPARE(os["reference"].toString(), QString("uhf"));
        QCOMPARE(o.multiplicity, 3);
        QCOMPARE(os["task"].toString(), QString("optimize_frequencies"));
        QVERIFY(os["prop_esp"].toBool());
        QVERIFY(other.notes.join('\n').contains("nwpw"));
        // NWChem numbers point charges too; frozen core only from "freeze atomic"; memory parts add up.
        const auto charges = importNwchemInput("memory heap 100 mb stack 1000 mb global 2400 mb\n"
                                               "geometry\n bq 0 0 5 charge 0.5\n O 0 0 0\n H 0 0.76 0.5\n"
                                               " H 0 -0.76 0.5\nend\nconstraints\n fix atom 2\nend\n"
                                               "mp2\n freeze 0\nend\ntask mp2 energy\n");
        QCOMPARE(charges.document.atoms.size(), 3);
        QVERIFY(charges.document.atoms[0].fixed && !charges.document.atoms[1].fixed);
        QVERIFY(!nwchemSettings(charges.document)["freeze_core"].toBool());
        QCOMPARE(nwchemSettings(charges.document)["memory_mb"].toInt(), 3500);
        QVERIFY(nwchemSettings(importNwchemInput("geometry\n Ne 0 0 0\nend\nccsd\n freeze atomic\nend\n")
                                   .document)["freeze_core"]
                    .toBool());
        // Custom XC lines and Z-matrix geometries.
        const auto custom =
            importNwchemInput("geometry\n He 0 0 0\nend\ndft\n xc xpbe96 0.75 cpbe96 1.0 hfexch 0.25\nend\n"
                              "task dft energy\n");
        QCOMPARE(nwchemSettings(custom.document)["functional"].toString(), QString("custom"));
        QCOMPARE(nwchemSettings(custom.document)["xc_custom"].toString(),
                 QString("xpbe96 0.75 cpbe96 1.0 hfexch 0.25"));
        QVERIFY_THROWS_EXCEPTION(std::exception,
                                 importNwchemInput("geometry\n zmatrix\n O\n H 1 0.96\n end\nend\n"));
        // A follow-up level after the first task does not replace the main one.
        auto f = water();
        set(f, "method", "dft");
        set(f, "task", "optimize");
        set(f, "followup", true);
        set(f, "followup_functional", "pbe0");
        const auto first = importNwchemInput(nwchemInput(f));
        QCOMPARE(nwchemSettings(first.document)["functional"].toString(), QString("b3lyp"));
        QVERIFY(first.notes.join('\n').contains("後続の計算"));
    }
    void gaussianAndXyzFrames() {
        const auto g =
            gaussianStructure("%chk=water.chk\n%mem=1GB\n#p B3LYP/6-31G(d) opt ! route\n\nwater\n\n"
                              "-1 2\nO 0 0 0\nH(Fragment=1) 0.758 0 0.504\n1 0 -0.758 0 0.504\n\n");
        QCOMPARE(g.atoms.size(), 3);
        QCOMPARE(g.atoms[2].element, QString("H"));
        QCOMPARE(g.atoms[2].position.x(), -0.758);
        QCOMPARE(g.charge, -1);
        QCOMPARE(g.multiplicity, 2);
        QVERIFY_THROWS_EXCEPTION(std::exception, gaussianStructure("#p hf\n\nzmat\n\n0 1\nO\nH 1 0.96\n\n"));
        // ONIOM rows: freeze code, coordinates, then the layer.
        const auto oniom = gaussianStructure("#p oniom(b3lyp/6-31g*:uff)\n\nlayers\n\n0 1\n"
                                             "O 0 0.0 0.0 0.0 H\nH -1 0.758 0.0 0.504 L\nH 0 0 0 1 H\n\n");
        QCOMPARE(oniom.atoms[1].position, Vec(0.758, 0, 0.504));
        QCOMPARE(oniom.atoms[2].position, Vec(0, 0, 1));
        QTemporaryDir dir;
        const auto path = dir.filePath("trajectory.xyz");
        writeFile(path, "1\nfirst\nHe 0 0 0\n\n1\nsecond\nHe 0 0 1\n");
        QCOMPARE(xyzFrames(QString::fromUtf8(readFile(path))).size(), 2);
        QVERIFY_THROWS_EXCEPTION(std::exception, importStructure(path));
        QCOMPARE(importStructure(path, {}, 1).atoms[0].position.z(), 1.0);
        const auto nw = dir.filePath("input.nw");
        writeFile(nw, "geometry\n Ne 0 0 0\nend\ncharge 0\ntask scf energy\n");
        QCOMPARE(importStructure(nw).atoms[0].element, QString("Ne"));
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
        auto water = this->water();
        set(water, "basis_overrides", QJsonObject{{"Fe", "def2-tzvp"}});
        QVERIFY(nwchemInput(water).contains("  * library \"sto-3g\"\n"));
        // The table in "2 計算条件" edits the same setting and is undoable.
        MainWindow w;
        w.loadDocument(this->water());
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
    void functionalsAndCustomXc() {
        auto d = water();
        set(d, "method", "dft");
        const QMap<QString, QString> xc{{"tpss", "xtpss03 ctpss03"},
                                        {"m06-2x", "m06-2x"},
                                        {"r2scan", "r2scan"},
                                        {"wb97x-d3", "wb97x-d3"},
                                        {"cam-b3lyp", "xcamb88 1.00 lyp 0.81 vwn_5 0.19 hfexch 1.00"}};
        for (auto it = xc.begin(); it != xc.end(); ++it) {
            set(d, "functional", it.key());
            QVERIFY2(nwchemInput(d).contains("\n  xc " + it.value() + '\n'), qPrintable(it.key()));
        }
        set(d, "functional", "cam-b3lyp");
        QVERIFY(nwchemInput(d).contains("  cam 0.33 cam_alpha 0.19 cam_beta 0.46\n"));
        // Only dispersion corrections NWChem has Grimme's parameters for.
        set(d, "functional", "m06-2x");
        set(d, "dispersion", "d3bj");
        QVERIFY(errorContains(d, "M06-2X のD3(BJ)"));
        set(d, "dispersion", "d3");
        QVERIFY(nwchemInput(d).contains("  disp vdw 3\n"));
        set(d, "functional", "scan");
        QVERIFY(errorContains(d, "D3(BJ)または補正なし"));
        set(d, "functional", "wb97x-d3");
        QVERIFY(errorContains(d, "D3を含む"));
        set(d, "dispersion", "none");
        QVERIFY(validateNwchem(d).errors.isEmpty());
        // Meta-GGA and range-separated functionals have no analytic Hessian in NWChem.
        set(d, "task", "frequencies");
        QVERIFY(nwchemInput(d).contains("task dft frequencies numerical\n"));
        set(d, "functional", "b3pw91");
        QVERIFY(nwchemInput(d).contains("task dft frequencies\n"));
        set(d, "task", "energy");
        // A custom XC line stays one line of names and numbers.
        set(d, "functional", "custom");
        QVERIFY(errorContains(d, "任意のxc"));
        set(d, "xc_custom", "  xpbe96 0.75   cpbe96 1.0 hfexch 0.25 ");
        QVERIFY(nwchemInput(d).contains("\n  xc xpbe96 0.75 cpbe96 1.0 hfexch 0.25\n  mult 1\n"));
        QCOMPARE(nwchemFunctionalLabel(nwchemSettings(d)), QString("xc xpbe96 0.75 cpbe96 1.0 hfexch 0.25"));
        for (const auto *bad : {"b3lyp\nend\ntask shell", "b3lyp; task", "b3lyp \"x\"", "b3lyp # x"}) {
            set(d, "xc_custom", bad);
            QVERIFY2(errorContains(d, "任意のxc"), bad);
        }
        set(d, "xc_custom", "b3lyp");
        set(d, "dispersion", "d3");
        QVERIFY(errorContains(d, "任意のxcでは分散補正"));
    }
    void basisConventionsCosmoAndSymmetry() {
        // Pople's 6-31G family uses Cartesian d shells by default; the other bases spherical harmonics.
        auto d = water();
        QVERIFY(nwchemInput(d).contains("basis spherical\n  * library \"sto-3g\""));
        set(d, "basis", "6-31g*");
        QVERIFY(nwchemInput(d).contains("basis cartesian\n  * library \"6-31g*\""));
        QVERIFY(!validateNwchem(d).warnings.join('\n').contains("通常"));
        set(d, "basis", "cc-pvdz");
        QVERIFY(nwchemInput(d).contains("basis spherical\n"));
        set(d, "basis_functions", "cartesian");
        QVERIFY(nwchemInput(d).contains("basis cartesian\n"));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("通常球面調和関数"));
        // Earlier projects and presets stored a boolean; it stays an explicit choice.
        auto legacy = water();
        set(legacy, "basis", "6-31g*");
        set(legacy, "spherical", true);
        QCOMPARE(nwchemSettings(legacy)["basis_functions"].toString(), QString("spherical"));
        QVERIFY(nwchemInput(legacy).contains("basis spherical\n"));
        QVERIFY(validateNwchem(legacy).warnings.join('\n').contains("通常Cartesian"));
        QCOMPARE(nwchemPresetSettings(QJsonObject{{"spherical", false}})["basis_functions"].toString(),
                 QString("cartesian"));
        // NWChem's COSMO has no UHF energy; ROHF (the automatic HF choice) and UKS work.
        auto open = water();
        open.charge = 1;
        open.multiplicity = 2;
        set(open, "solvent", "cosmo");
        QVERIFY(validateNwchem(open).errors.isEmpty());
        set(open, "reference", "uhf");
        QVERIFY(errorContains(open, "COSMOはUHF"));
        set(open, "method", "dft");
        QVERIFY(validateNwchem(open).errors.isEmpty());
        // Thermochemistry uses σ = 1 under noautosym; symmetric molecules get the correction.
        auto frequencies = water();
        set(frequencies, "task", "frequencies");
        auto hint = validateNwchem(frequencies).warnings.join('\n');
        QVERIFY2(hint.contains("回転対称数σ=2と") && hint.contains("1.377"), qPrintable(hint));
        frequencies.atoms[1].isotope = 2; // HDO is Cs
        QVERIFY(!validateNwchem(frequencies).warnings.join('\n').contains("回転対称数"));
        auto methane = Document::fromXyz("5\nmethane\nC 0 0 0\nH .629 .629 .629\nH -.629 -.629 .629\n"
                                         "H -.629 .629 -.629\nH .629 -.629 -.629\n");
        set(methane, "task", "optimize_frequencies");
        QVERIFY(validateNwchem(methane).warnings.join('\n').contains("回転対称数σ=12と"));
        set(methane, "task", "optimize");
        QVERIFY(!validateNwchem(methane).warnings.join('\n').contains("回転対称数"));
    }
    void projectAndPresetPreservation() {
        auto d = water();
        d.envelope["result_reference"] = QJsonObject{{"path", "existing.out"}};
        d.charge = 2; // Presets leave the structure's charge and multiplicity.
        d.multiplicity = 1;
        set(d, "future", QJsonObject{{"engine", 99}});
        set(d, "start", "saved_name");
        set(d, "title", "saved title");
        set(d, "memory_mb", 2048);
        set(d, "geometry_units", "bohr");
        const auto atoms = d.toJson().value("atoms");
        applyNwchemPreset(d, nwchemPresets()[1].settings);
        QCOMPARE(d.charge, 2);
        QCOMPARE(d.multiplicity, 1);
        QCOMPARE(d.toJson()["atoms"], atoms);
        QCOMPARE(nwchemSettings(d)["start"].toString(), "saved_name");
        QCOMPARE(nwchemSettings(d)["title"].toString(), "saved title");
        QCOMPARE(nwchemSettings(d)["memory_mb"].toInt(), 2048);
        QCOMPARE(nwchemSettings(d)["geometry_units"].toString(), "bohr");
        QCOMPARE(nwchemSettings(d)["future"].toObject()["engine"].toInt(), 99);
        const auto restored = Document::fromProject(d.project());
        QCOMPARE(restored.data["nwchem"], d.data["nwchem"]);
        QCOMPARE(restored.envelope["result_reference"], d.envelope["result_reference"]);
        QCOMPARE(restored.charge, 2);
        QCOMPARE(nwchemInput(restored), nwchemInput(d));
        QVERIFY(nwchemInput(d).contains("charge 2\n"));
        QVERIFY(nwchemInput(d).contains("task dft optimize"));
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
        water().save(path);
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
