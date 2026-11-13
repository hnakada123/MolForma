// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/coordinates.h"
#include "editing/operations.h"
#include "foundation/i18n.h"
#include "model/units.h"
#include "nwchem/input_builder.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include "nwchem/nwchem_geometry.h"
#include "nwchem/nwchem_import.h"
#include "nwchem/nwchem_output.h"
#include "nwchem/nwchem_plan.h"
#include "nwchem_support.h"
#include "results/results.h"
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <limits>
using namespace mf;

// NWChem input without a window: the settings and their checks, the task plan, the generated input with
// its outline, the advanced options, the CUBE/MOLDEN tasks and input read back as settings.
class NwchemTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings, files;
    // errorContains, as the tests of the advanced options name it.
    static bool errors(const Document &d, const QString &text) {
        return errorContains(d, text);
    }
  private slots:
    void initTestCase() {
        QVERIFY(settings.isValid());
        QVERIFY(files.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
        qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
        qputenv("XDG_DATA_HOME", settings.filePath("data").toUtf8());
    }
    void taskPlanMatchesWrittenInput() {
        const QVector<QPair<QJsonObject, QStringList>> cases{
            {{{"task", "energy"}}, {"task scf energy"}},
            {{{"task", "optimize_frequencies"}}, {"task scf optimize", "task scf frequencies"}},
            {{{"task", "optimize_frequencies"}, {"method", "mp2"}},
             {"task mp2 optimize", "task mp2 frequencies numerical"}},
            {{{"task", "optimize_frequencies"},
              {"followup", true},
              {"followup_method", "hf"},
              {"followup_basis", "sto-3g"},
              {"prop_nmr", true},
              {"advanced", QJsonObject{{"molden_output", true}, {"cube_output", true}}}},
             {"task scf optimize", "task scf frequencies", "task scf energy", "task scf property",
              "task scf property", "task dplot"}},
            {{{"task", "frequencies"}, {"method", "dft"}, {"prop_raman", true}},
             {"task dft frequencies", "task dft raman"}},
            {{{"task", "energy"}, {"prop_ord", true}, {"ord_points", 2}},
             {"task scf energy", "task scf property", "task scf property"}},
            {{{"task", "gradient"}, {"extra_input", "task scf energy"}, {"extra_tasks", "task scf energy"}},
             {"task scf energy", "task scf gradient", "task scf energy"}}};
        for (const auto &[settings, expected] : cases) {
            auto d = water();
            d.data["nwchem"] = settings;
            const auto report = validateNwchem(d);
            QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join('\n')));
            QCOMPARE(nwchemTaskPlan(d).commands(), expected);
            QCOMPARE(nwchemTaskLines(generateNwchemInput(d).text), expected);
        }
    }
    // A continued restart leaves out the generated tasks the other job finished. The tasks of the additional
    // input are among those it finished, but they are the user's text: they stay, and count for no
    // generated task. The plan's steps say so, and the input has the tasks of the steps it does not skip.
    void continuedRestartKeepsUnfinishedTasks() {
        using Phase = NwchemTaskPhase;
        using Steps = QList<NwchemTaskStep>;
        const auto continued = [](Document d, const QStringList &done) {
            set(d, "restart",
                QJsonObject{{"mode", "continue"},
                            {"job", "/job"},
                            {"done", QJsonArray::fromStringList(done)},
                            {"geometry", nwchemRestartDigest(d, true)}});
            return d;
        };
        const auto tasks = [](const Document &d) {
            const auto report = validateNwchem(d);
            return report.errors.isEmpty() ? nwchemTaskLines(generateNwchemInput(d).text) : report.errors;
        };
        auto d = water();
        set(d, "task", "optimize_frequencies");
        set(d, "prop_mulliken", true);
        auto plan = nwchemTaskPlan(d);
        QCOMPARE(plan.steps, Steps({{Phase::Main, "task scf optimize"},
                                    {Phase::Main, "task scf frequencies"},
                                    {Phase::Properties, "task scf property"}}));
        QCOMPARE(plan.finished, 0);
        QCOMPARE(plan.frequency, 1);
        auto next = continued(d, {"task scf optimize"});
        plan = nwchemTaskPlan(next);
        QCOMPARE(plan.steps, Steps({{Phase::Main, "task scf optimize", true},
                                    {Phase::Main, "task scf frequencies"},
                                    {Phase::Properties, "task scf property"}}));
        QCOMPARE(plan.finished, 1);
        QCOMPARE(plan.frequency, 1);
        QCOMPARE(plan.commands(), nwchemTaskPlan(d).commands()); // Skipped or not.
        QCOMPARE(tasks(next), QStringList({"task scf frequencies", "task scf property"}));
        // What stands around a task that is left out is written as before.
        const auto input = generateNwchemInput(next);
        QVERIFY(input.text.contains("\ndriver\n"));
        QVERIFY(input.text.contains("end\n\ntask scf frequencies\n\nproperty\n"));
        QCOMPARE(int(std::count_if(input.outline.blocks.cbegin(), input.outline.blocks.cend(),
                                   [](const auto &block) { return block.keyword == "task"; })),
                 2);
        // A task before the generated ones, indented and in upper case as NWChem also reads it.
        set(d, "extra_input", "set scf:maxiter 60\n  TASK scf energy");
        QCOMPARE(nwchemTaskPlan(d).commands(Phase::Before), QStringList{"TASK scf energy"});
        next = continued(d, {"TASK scf energy"}); // The optimization was under way.
        plan = nwchemTaskPlan(next);
        QCOMPARE(plan.steps, Steps({{Phase::Before, "TASK scf energy"},
                                    {Phase::Main, "task scf optimize"},
                                    {Phase::Main, "task scf frequencies"},
                                    {Phase::Properties, "task scf property"}}));
        QVERIFY(!plan.steps[0].generated());
        QVERIFY(plan.steps[1].generated());
        QCOMPARE(plan.finished, 0);
        QCOMPARE(plan.frequency, 1); // Among the tasks of its phase.
        QCOMPARE(tasks(next), QStringList({"TASK scf energy", "task scf optimize", "task scf frequencies",
                                           "task scf property"}));
        next = continued(d, {"TASK scf energy", "task scf optimize"}); // The frequencies were under way.
        plan = nwchemTaskPlan(next);
        QCOMPARE(plan.steps, Steps({{Phase::Before, "TASK scf energy"},
                                    {Phase::Main, "task scf optimize", true},
                                    {Phase::Main, "task scf frequencies"},
                                    {Phase::Properties, "task scf property"}}));
        QCOMPARE(plan.finished, 1);
        QCOMPARE(tasks(next), QStringList({"TASK scf energy", "task scf frequencies", "task scf property"}));
        // Tasks after the generated ones: those of them that ended run again with the others.
        set(d, "extra_tasks", "task scf gradient\ntask scf energy");
        const Steps user({{Phase::Before, "TASK scf energy"},
                          {Phase::Main, "task scf optimize", true},
                          {Phase::Main, "task scf frequencies", true},
                          {Phase::Properties, "task scf property", true},
                          {Phase::After, "task scf gradient"},
                          {Phase::After, "task scf energy"}});
        next = continued(d, {"TASK scf energy", "task scf optimize", "task scf frequencies",
                             "task scf property", "task scf gradient"});
        plan = nwchemTaskPlan(next);
        QCOMPARE(plan.steps, user);
        QVERIFY(!plan.steps[4].generated());
        QCOMPARE(plan.finished, 3);
        QCOMPARE(tasks(next), QStringList({"TASK scf energy", "task scf gradient", "task scf energy"}));
        // The settings of a line that is left out stay known to the outline, without a line.
        QVERIFY(generateNwchemInput(next).outline.settings.value("numerical_frequencies").isEmpty());
        // However many tasks the other job finished, the user's are never among the skipped ones.
        next = continued(d, nwchemTaskPlan(d).commands() + QStringList{"task scf energy"});
        plan = nwchemTaskPlan(next);
        QCOMPARE(plan.steps, user);
        QCOMPARE(plan.finished, 3);
    }
    // The input writer takes the generated tasks of a plan in their order, each in the phase it expects:
    // it cannot write a task the plan does not have, nor leave one of the plan's out.
    void taskCursorFollowsPlan() {
        using Phase = NwchemTaskPhase;
        auto d = water();
        set(d, "task", "optimize");
        set(d, "prop_esp", true);
        set(d, "extra_input", "task scf energy");
        set(d, "extra_tasks", "task scf gradient");
        NwchemTaskCursor tasks(nwchemTaskPlan(d));
        QCOMPARE(tasks.plan.commands(),
                 QStringList({"task scf energy", "task scf optimize", "task esp", "task scf gradient"}));
        QVERIFY(tasks.at(Phase::Main)); // The user's task before it is not the writer's.
        QVERIFY(!tasks.at(Phase::Before));
        QVERIFY(!tasks.at(Phase::Esp));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, tasks.take(Phase::Esp));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, tasks.finish());
        QCOMPARE(tasks.take(Phase::Main).command, QString("task scf optimize"));
        QVERIFY(!tasks.at(Phase::Main));
        QCOMPARE(tasks.take(Phase::Esp).command, QString("task esp"));
        QVERIFY(!tasks.at(Phase::After));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, tasks.take(Phase::After));
        tasks.finish();
        // A skipped step is taken like the others: the writer leaves its task out, not its place.
        set(d, "restart",
            QJsonObject{{"mode", "continue"},
                        {"job", "/job"},
                        {"done", QJsonArray{"task scf energy", "task scf optimize"}},
                        {"geometry", nwchemRestartDigest(d, true)}});
        NwchemTaskCursor left(nwchemTaskPlan(d));
        QVERIFY(left.take(Phase::Main).skipped);
        QVERIFY(!left.take(Phase::Esp).skipped);
        left.finish();
    }
    // The lines around a task that is left out read as if its line had been taken out of the complete
    // input: of two empty lines around it one goes, and the lines after it are counted without it.
    void skippedTaskLeavesNoGap() {
        using Phase = NwchemTaskPhase;
        const auto written = [](const QList<NwchemTaskStep> &steps, const QString &after) {
            NwchemTaskPlan plan;
            plan.steps = steps;
            NwchemTaskCursor tasks(plan);
            NwchemInputBuilder out;
            out.append("charge 0\n\n");
            for (const auto &step : steps)
                out.task(tasks, step.phase, "task", {"numerical_frequencies"});
            out.block("property", after, "prop_mulliken");
            tasks.finish();
            return out.result;
        };
        const NwchemTaskStep optimize{Phase::Main, "task scf optimize", true},
            frequencies{Phase::Main, "task scf frequencies", true},
            energy{Phase::Followup, "task scf energy"};
        auto input = written({optimize}, "\nproperty\nend\n");
        QCOMPARE(input.text, QString("charge 0\n\nproperty\nend\n"));
        QCOMPARE(input.outline.blocks.size(), 1);
        QCOMPARE(input.outline.blocks[0].line, 2);
        QVERIFY(input.outline.settings.contains("numerical_frequencies"));
        QVERIFY(input.outline.settings["numerical_frequencies"].isEmpty());
        // A line that follows at once keeps the empty line before the task.
        QCOMPARE(written({optimize}, "property\nend\n").text, QString("charge 0\n\nproperty\nend\n"));
        input = written({optimize, frequencies, energy}, "\nproperty\nend\n");
        QCOMPARE(input.text, QString("charge 0\n\ntask scf energy\n\nproperty\nend\n"));
        QCOMPARE(input.outline.blocks[0].line, 2);
        QCOMPARE(input.outline.settings["numerical_frequencies"], QSet<int>{2});
        QCOMPARE(input.outline.blocks[1].line, 4);
        QCOMPARE(written({optimize, frequencies}, "\nproperty\nend\n").text,
                 QString("charge 0\n\nproperty\nend\n"));
        // The end of the input counts as an empty line.
        QCOMPARE(written({optimize, frequencies}, {}).text, QString("charge 0\n"));
        // Written tasks take no line with them.
        auto run = optimize;
        run.skipped = false;
        QCOMPARE(written({run, frequencies}, "\nproperty\nend\n").text,
                 QString("charge 0\n\ntask scf optimize\n\nproperty\nend\n"));
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
        QCOMPARE(nwchemTaskPlan(d).commands(), nwchemTaskLines(input));
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
        QCOMPARE(nwchemTaskPlan(d).commands(), nwchemTaskLines(input));
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
        QVERIFY(nwchemSectionUse(d, NwchemSection::Path).used);
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
        d.charge = 1;
        d.multiplicity = 2;
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
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
        }
        for (const auto &bad : {"/remote/scratch job", "host:/scratch", "scratch'job"})
            QVERIFY(!nwchemAdvancedError(option("scratch_dir"), bad).isEmpty());
    }
    void inactiveAndUnknownDataSurvive() {
        auto d = water();
        d.envelope["future_result"] = QJsonObject{{"id", "result-1"}};
        d.data["future_document"] = QJsonObject{{"scf", QJsonObject{{"future", 99}}}};
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
        QCOMPARE(restored.data["future_document"], d.data["future_document"]);
        resetNwchemAdvanced(d, "scf");
        QVERIFY(nwchemAdvanced(d).contains("dft_damp"));
        resetNwchemAdvanced(d);
        QCOMPARE(nwchemAdvanced(d).size(), 1);
        QCOMPARE(nwchemAdvanced(d)["future"].toObject()["option"].toString(), "keep");
        QCOMPARE(d.data["future_document"].toObject()["scf"].toObject()["future"].toInt(), 99);
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
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
    }
    void effectiveDefaultsAndScopes() {
        auto d = water();
        const auto text = [&](const QString &key) { return nwchemAdvancedDefault(option(key), d); };
        QCOMPARE(text("dft_energy"), QString("1e-7（SCF収束: 厳密）"));
        QCOMPARE(text("driver_gmax"), QString("0.00045（最適化収束: 標準）"));
        QCOMPARE(text("driver_trust"), QString("0.3（NWChem既定）"));
        QCOMPARE(text("cube_divisions"), QString("40")); // MolForma's own default.
        QCOMPARE(text("corr_freeze"), QString("0（凍結なし）"));
        QCOMPARE(text("freq_fd_delta"), QString("0.001"));
        base(d, "scf_convergence", "default");
        base(d, "opt_convergence", "tight");
        base(d, "freeze_core", true);
        base(d, "method", "dft");
        QCOMPARE(text("scf_thresh"), QString("NWChem既定（SCF収束: NWChem既定）"));
        QCOMPARE(text("driver_xrms"), QString("0.00004（最適化収束: 厳密）"));
        QCOMPARE(text("corr_freeze"), QString("atomic（内殻凍結）"));
        QCOMPARE(text("freq_fd_delta"), QString("0.010（DFT）"));
        base(d, "opt_convergence", "loose");
        QCOMPARE(text("driver_xmax"), QString("0.018（最適化収束: 緩い）"));
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
    void outlineMarksAdvancedAndExtraLines() {
        auto d = water();
        set(d, "method", "dft");
        set(d, "task", "optimize");
        set(d, "extra_input", "set cphf:maxiter 50");
        setNwchemAdvanced(d, "dft_grid_nodisk", true);
        setNwchemAdvanced(d, "driver_trust", 0.2);
        setNwchemAdvanced(d, "scratch_dir", "scratch");
        setNwchemAdvanced(d, "freq_animate", true); // Kept for frequency tasks: no line.
        const auto generated = generateNwchemInput(d);
        const auto lines = generated.text.split('\n');
        const auto outline = generated.outline;
        // The lines that the details and the additional input add or change.
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
        QCOMPARE(blocks["geometry"].targets, QList<SettingTarget>{SettingTarget::Molecule});
        QCOMPARE(blocks["geometry"].lines, 5);
        QCOMPARE(blocks["basis"].title, QString("basis \"sto-3g\""));
        QCOMPARE(blocks["dft"].targets, QList<SettingTarget>{SettingTarget::Calculation});
        QCOMPARE(blocks["dft"].setting, QString("method"));
        QCOMPARE(blocks["dft"].group, QString("dft"));
        QCOMPARE(lines[blocks["dft"].line + blocks["dft"].lines - 1], QString("end"));
        QCOMPARE(blocks["driver"].group, QString("driver"));
        QCOMPARE(blocks["additional"].targets, QList<SettingTarget>{SettingTarget::Calculation});
        QCOMPARE(blocks["additional"].setting, QString("extra_input"));
        QCOMPARE(blocks["task"].targets, QList<SettingTarget>{SettingTarget::Calculation});
        QCOMPARE(lines[blocks["task"].line], QString("task dft optimize"));
        // A block that only the details write belongs to them alone.
        setNwchemAdvanced(d, "molden_output", true);
        const auto withMolden = generateNwchemInput(d).outline;
        const auto property = std::find_if(withMolden.blocks.begin(), withMolden.blocks.end(),
                                           [](const auto &b) { return b.keyword == "property"; });
        QVERIFY(property != withMolden.blocks.end());
        QCOMPARE(property->targets, QList<SettingTarget>{SettingTarget::Calculation});
        QCOMPARE(property->setting, QString());
        QCOMPARE(property->group, QString("visualization"));
        // Without such settings nothing is marked; an empty input has no outline.
        const auto plain = water();
        const auto none = generateNwchemInput(plain).outline;
        QVERIFY(none.advanced.isEmpty() && none.extra.isEmpty() && !none.blocks.isEmpty());
        QVERIFY(NwchemGeneratedInput{}.outline.blocks.isEmpty());
    }
    void generatedProvenanceKeepsRepeatedLinesSeparate() {
        auto d = water();
        d.atoms[0].isotope = 18;
        set(d, "method", "mp2");
        set(d, "extra_input", "mp2\n  freeze 0\nend");
        set(d, "extra_tasks", "task mp2 energy");
        setNwchemAdvanced(d, "corr_freeze", 0); // Same text as the default, but explicitly set.
        const auto generated = generateNwchemInput(d);
        const auto lines = generated.text.split('\n');
        const auto own = generated.outline.settings.value("corr_freeze");
        QCOMPARE(own.size(), 1);
        const int freeze = *own.begin();
        QCOMPARE(lines[freeze], QString("  freeze 0"));
        QVERIFY(generated.outline.advanced.contains(freeze));
        QVERIFY(!generated.outline.extra.contains(freeze));
        QCOMPARE(generated.outline.settings.value("extra_input").size(), 4);
        QCOMPARE(generated.outline.settings.value("extra_tasks").size(), 2);
        for (const auto &block : generated.outline.blocks) {
            QVERIFY(block.line >= 0 && block.line + block.lines <= lines.size());
            if (block.keyword == "geometry") {
                QCOMPARE(block.title, QString("geometry（3 原子）"));
                QCOMPARE(block.lines, 5); // Isotope mass does not confuse atom counting.
            }
        }
        setNwchemAdvanced(d, "vectors_guess", "file");
        setNwchemAdvanced(d, "vectors_input", "input.movecs");
        setNwchemAdvanced(d, "vectors_output", "new.movecs");
        const auto plan = executionPlan(d, files.path(), files.filePath("job"), false);
        const auto resolvedLines = plan.input.split('\n');
        const auto vectors = plan.inputOutline.settings.value("vectors_input");
        QCOMPARE(vectors.size(), 1);
        const int vectorLine = *vectors.begin();
        QVERIFY(resolvedLines[vectorLine].contains(files.filePath("input.movecs")));
        QVERIFY(plan.inputOutline.advanced.contains(vectorLine));
    }
    void followupBlocksKeepTheirOwnSettingsAndSharedDetails() {
        auto d = water();
        set(d, "method", "dft");
        set(d, "task", "optimize");
        set(d, "followup", true);
        setNwchemAdvanced(d, "dft_grid_nodisk", true);
        const auto generated = generateNwchemInput(d);
        QStringList methods, bases;
        for (const auto &block : generated.outline.blocks) {
            if (block.keyword == "dft") {
                methods << block.setting;
                QCOMPARE(block.group, QString("dft"));
            }
            if (block.keyword == "basis")
                bases << block.setting;
        }
        QCOMPARE(methods, (QStringList{"method", "followup_method"}));
        QCOMPARE(bases, (QStringList{"basis", "followup_basis"}));
        QCOMPARE(generated.outline.settings.value("dft_grid_nodisk").size(), 2);
    }
    void cubeAxesKeepExplicitAndAutomaticSourcesSeparate() {
        auto d = water();
        setNwchemAdvanced(d, "cube_output", true);
        setNwchemAdvanced(d, "cube_x", QJsonArray{-3, 3, 20});
        const auto generated = generateNwchemInput(d);
        const auto x = generated.outline.settings.value("cube_x");
        const auto automatic = generated.outline.settings.value("cube_padding");
        QCOMPARE(x.size(), 1);
        QCOMPARE(automatic.size(), 2);
        QVERIFY(!automatic.contains(*x.begin()));
        QCOMPARE(generated.outline.settings.value("cube_divisions"), automatic);
    }
    void defaultsAndTasks() {
        auto d = water();
        const auto before = d.project();
        QVERIFY(!nwchemInput(d).contains("dplot"));
        QVERIFY(!nwchemInput(d).contains("property"));
        QVERIFY(nwchemOutputFiles(d, validateNwchem(d)).isEmpty());
        QCOMPARE(d.project(), before);
        both(d);
        const auto input = nwchemInput(d);
        QVERIFY(input.contains("task scf energy\n\n# Visualization"));
        QVERIFY(input.contains("property\n  vectors \"./molforma.movecs\"\n  moldenfile\n  molden_norm "
                               "nwchem\nend\ntask scf property"));
        QVERIFY(input.contains("spin total\n  gaussian\n  output molforma_density.cube\nend\ntask dplot"));
        QVERIFY(input.indexOf("task scf energy") < input.indexOf("task scf property"));
        QVERIFY(input.indexOf("task scf property") < input.indexOf("task dplot"));
        // 40 intervals along the longest side (X: 7.516 Å), the same spacing on Y and Z.
        auto grid = nwchemCubeGrid(d);
        QCOMPARE(grid.divisions, (std::array<int, 3>{40, 32, 35}));
        QCOMPARE(grid.points(), 41LL * 33 * 36);
        QCOMPARE(grid.minimum[0], -3.758);
        for (int axis = 0; axis < 3; ++axis)
            QVERIFY(std::abs((grid.maximum[axis] - grid.minimum[axis]) / grid.divisions[axis] - 7.516 / 40) <
                    1e-12);
        QVERIFY(grid.minimum[2] < -3 && grid.maximum[2] > 3.504);
        QVERIFY(std::abs(grid.minimum[2] + grid.maximum[2] - .504) < 1e-12); // Centered on the atoms.
        base(d, "geometry_units", "bohr");
        QVERIFY(nwchemInput(d).contains("limitxyz units angstrom"));
        QCOMPARE(nwchemCubeGrid(d).minimum, grid.minimum);
        base(d, "method", "dft");
        QVERIFY(nwchemInput(d).contains("task dft property"));
        base(d, "method", "mp2");
        QVERIFY(nwchemInput(d).contains("task mp2 energy"));
        QVERIFY(nwchemInput(d).contains("task scf property"));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("参照HF"));
        base(d, "method", "hf");
        base(d, "task", "optimize_frequencies");
        base(d, "numerical_frequencies", true);
        const auto frequency = nwchemInput(d);
        QVERIFY(frequency.indexOf("task scf optimize") < frequency.indexOf("task scf frequencies numerical"));
        QVERIFY(frequency.indexOf("task scf frequencies numerical") <
                frequency.lastIndexOf("task scf energy"));
        QVERIFY(frequency.lastIndexOf("task scf energy") < frequency.indexOf("task scf property"));
    }
    void orbitalsAndSpin() {
        auto d = water();
        both(d);
        setNwchemAdvanced(d, "cube_type", "orbital");
        QVERIFY(nwchemInput(d).contains("spin alpha\n  orbitals view; 1; 5"));
        QVERIFY(nwchemInput(d).contains("output molforma_alpha_mo5.cube"));
        setNwchemAdvanced(d, "cube_orbital", "lumo");
        QVERIFY(nwchemInput(d).contains("orbitals view; 1; 6"));
        setNwchemAdvanced(d, "cube_spin", "beta");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
        base(d, "reference", "uhf");
        d.charge = 1;
        d.multiplicity = 2;
        setNwchemAdvanced(d, "cube_orbital", "homo");
        QVERIFY(nwchemInput(d).contains("spin beta\n  orbitals view; 1; 4"));
        setNwchemAdvanced(d, "cube_orbital", "index");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
        setNwchemAdvanced(d, "cube_index", 7);
        QVERIFY(nwchemInput(d).contains("orbitals view; 1; 7"));
        setNwchemAdvanced(d, "cube_type", "density");
        setNwchemAdvanced(d, "cube_index", "bad but inactive");
        QVERIFY(!nwchemInput(d).contains("orbitals view"));
        const QMap<QString, QString> types{{"density", "total"},
                                           {"alpha_density", "alpha"},
                                           {"beta_density", "beta"},
                                           {"spin_density", "spindens"}};
        for (auto it = types.begin(); it != types.end(); ++it) {
            setNwchemAdvanced(d, "cube_type", it.key());
            QVERIFY(nwchemInput(d).contains("spin " + it.value()));
        }
        auto h = Document::fromXyz("1\nH\nH 0 0 0\n");
        h.multiplicity = 2;
        base(h, "reference", "uhf");
        both(h);
        setNwchemAdvanced(h, "cube_type", "orbital");
        setNwchemAdvanced(h, "cube_spin", "beta");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(h));
        setNwchemAdvanced(h, "cube_orbital", "lumo");
        QVERIFY(nwchemInput(h).contains("orbitals view; 1; 1"));
    }
    // CUBEs of an excited state of TDDFT: its transition density from the CI vectors, its density and
    // the difference from the ground state from the density matrices of a TDDFT gradient.
    void excitedStateDensities() {
        auto d = water();
        base(d, "method", "dft");
        setNwchemAdvanced(d, "cube_output", true);
        setNwchemAdvanced(d, "cube_type", "transition_density");
        QVERIFY(validateNwchem(d).errors.join('\n').contains(tr("励起状態（TDDFT）が必要")));
        base(d, "tddft", true);
        base(d, "tddft_roots", 3);
        auto input = nwchemInput(d);
        QVERIFY2(input.contains("tddft\n  nroots 3\n  notriplet\n  civecs\nend\n"), qPrintable(input));
        QVERIFY(input.contains("task tddft energy"));
        QVERIFY(input.contains("dplot\n  vectors \"./molforma.movecs\"\n"));
        QVERIFY(input.contains("  civecs \"./molforma.civecs_singlet\"\n  root 1\n"));
        QVERIFY(input.contains(
            "spin total\n  gaussian\n  output molforma_transition_root1.cube\nend\ntask dplot"));
        QVERIFY(input.indexOf("task tddft energy") < input.indexOf("task dplot"));
        setNwchemAdvanced(d, "cube_root", 4);
        QVERIFY(validateNwchem(d).errors.join('\n').contains("（3）"));
        setNwchemAdvanced(d, "cube_root", 2);
        QVERIFY(nwchemOutputFiles(d, validateNwchem(d)).contains("molforma_transition_root2.cube"));
        // The density of the state and its difference from the ground state need its gradient.
        setNwchemAdvanced(d, "cube_type", "excited_density");
        input = nwchemInput(d);
        QVERIFY2(input.contains("  civecs\n  target 2\n  grad\n    root 2\n  end\nend\n"), qPrintable(input));
        QVERIFY(input.contains("task tddft gradient") && !input.contains("task tddft energy"));
        QVERIFY(input.contains("dplot\n  limitxyz") && input.contains("  densmat \"./molforma.dmat\"\n"));
        QVERIFY(input.contains("output molforma_excited_root2.cube"));
        QVERIFY(validateNwchem(d).warnings.join('\n').contains("task tddft gradient"));
        setNwchemAdvanced(d, "cube_type", "difference_density");
        input = nwchemInput(d);
        QVERIFY(input.contains("  densmat \"./molforma.diffdmat\"\n") &&
                input.contains("output molforma_difference_root2.cube"));
        // A permanent folder holds what NWChem writes there.
        setNwchemAdvanced(d, "permanent_dir", "perm");
        QVERIFY(nwchemInput(d).contains("  densmat \"./perm/molforma.diffdmat\"\n"));
        setNwchemAdvanced(d, "permanent_dir", QJsonValue(QJsonValue::Undefined));
        // Triplets beside the singlets: a gradient would not know which state; the transition density is
        // that of a singlet.
        base(d, "tddft_triplets", true);
        QVERIFY(validateNwchem(d).errors.join('\n').contains(tr("三重項")));
        setNwchemAdvanced(d, "cube_type", "transition_density");
        QVERIFY(validateNwchem(d).errors.isEmpty());
        QVERIFY(validateNwchem(d).warnings.join('\n').contains(tr("一重項")));
        // An open shell writes one file of CI vectors.
        base(d, "tddft_triplets", false);
        d.charge = 1;
        d.multiplicity = 2;
        QVERIFY2(nwchemInput(d).contains("  civecs \"./molforma.civecs\"\n  root 2\n"),
                 qPrintable(nwchemInput(d)));
        // Without the CUBE, TDDFT is written as before.
        setNwchemAdvanced(d, "cube_output", false);
        input = nwchemInput(d);
        QVERIFY(!input.contains("civecs") && input.contains("task tddft energy"));
    }
    void validationAndPreservation() {
        auto d = water();
        both(d);
        d.envelope["future"] = "keep";
        for (const auto &bad : {QJsonValue(QJsonArray{-3, 3}), QJsonValue(QJsonArray{3, -3, 20}),
                                QJsonValue(QJsonArray{-3, 3, 1.5}), QJsonValue("NaN")}) {
            setNwchemAdvanced(d, "cube_x", bad);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
        }
        setNwchemAdvanced(d, "cube_x", QJsonArray{-3, 3, 30});
        setNwchemAdvanced(d, "cube_divisions", 1000);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
        setNwchemAdvanced(d, "cube_divisions", 20);
        QCOMPARE(nwchemCubeGrid(d).points(), 31LL * 17 * 19); // Y and Z keep the automatic spacing.
        for (const auto &name :
             {"../bad.cube", "input.nw", "bad name.cube", "bad;task.cube", "bad\n.cube", "/abs.cube"}) {
            setNwchemAdvanced(d, "cube_filename", name);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
        }
        setNwchemAdvanced(d, "cube_filename", "orbitals.CUBE");
        setNwchemAdvanced(d, "vectors_output", "orbitals.CUBE");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(d));
        setNwchemAdvanced(d, "vectors_output", "orbitals.movecs");
        const auto saved = d.project();
        auto next = Document::fromProject(saved);
        QCOMPARE(nwchemAdvanced(next), nwchemAdvanced(d));
        QCOMPARE(next.envelope["future"].toString(), "keep");
        applyNwchemPreset(next, nwchemPresets().first().settings);
        QCOMPARE(nwchemAdvanced(next), nwchemAdvanced(d)); // Output choices are not numerical presets.
        setNwchemAdvanced(next, "cube_output", QJsonValue::Undefined);
        setNwchemAdvanced(next, "cube_filename", "invalid while disabled");
        QVERIFY(!nwchemInput(next).contains("dplot"));
        QVERIFY(nwchemInput(next).contains("moldenfile"));
        setNwchemAdvanced(next, "cube_output", true);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, nwchemInput(next));
        resetNwchemAdvanced(next, "visualization");
        QVERIFY(!nwchemInput(next).contains("property"));
        QCOMPARE(nwchemAdvanced(next)["vectors_output"].toString(), "orbitals.movecs");
    }
};
int main(int argc, char **argv) {
    if (const int code = resultWorkerMain(argc, argv); code >= 0)
        return code;
    QCoreApplication app(argc, argv);
    NwchemTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_nwchem.moc"
