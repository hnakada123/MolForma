// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_plan.h"
#include "foundation/error.h"
#include "foundation/i18n.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"
#include <utility>

namespace mf {
namespace nwchem_detail {
QString theoryOf(const QString &method) {
    return method == "hf" ? QString("scf") : method;
}
Document followupDocument(const Document &d) {
    Document next = d;
    const auto full = nwchemSettings(d);
    auto s = d.data["nwchem"].toObject();
    s["method"] = full["followup_method"];
    s["functional"] = full["followup_functional"];
    s["dispersion"] = full["followup_dispersion"];
    s["basis"] = full["followup_basis"];
    s["xc_custom"] = "";
    s["basis_overrides"] = QJsonObject();
    s["basis_custom"] = ""; // The follow-up basis is one library basis for all elements.
    s["ecp_custom"] = "";
    s["task"] = "energy";
    // Property requests stay: they run at this level.
    for (const auto *key : {"tddft", "followup", "bsse"})
        s[key] = false;
    s["constraints"] = QJsonArray();
    s["extra_input"] = "";
    s["extra_tasks"] = "";
    auto advanced = s["advanced"].toObject();
    advanced.remove("vectors_guess"); // Orbitals of the first basis do not fit the second one.
    advanced.remove("vectors_input");
    if (advanced.isEmpty())
        s.remove("advanced");
    else
        s["advanced"] = advanced;
    next.data["nwchem"] = s;
    return next;
}
} // namespace nwchem_detail
using namespace nwchem_detail;
namespace {
// The tasks that write CUBE and MOLDEN files, as nwchemOutputLines takes them.
QStringList outputTasks(const Document &d) {
    const auto s = nwchemSettings(d), a = nwchemAdvanced(d);
    QStringList tasks;
    if (!a["cube_output"].toBool() && !a["molden_output"].toBool())
        return tasks;
    const NwchemCalculation calc(s);
    const auto theory = calc.dft() ? QString("dft") : QString("scf");
    if (calc.frequencies())
        tasks << "task " + theory + " energy";
    if (a["molden_output"].toBool())
        tasks << "task " + theory + " property";
    if (a["cube_output"].toBool())
        tasks << "task dplot";
    return tasks;
}
} // namespace
QStringList NwchemTaskPlan::commands() const {
    QStringList all;
    for (const auto &step : steps)
        all << step.command;
    return all;
}
QStringList NwchemTaskPlan::commands(NwchemTaskPhase phase) const {
    QStringList all;
    for (const auto &step : steps)
        if (step.phase == phase)
            all << step.command;
    return all;
}
NwchemTaskCursor::NwchemTaskCursor(NwchemTaskPlan plan) : plan(std::move(plan)) {
    advance();
}
void NwchemTaskCursor::advance() {
    while (next < plan.steps.size() && !plan.steps[next].generated())
        ++next;
}
bool NwchemTaskCursor::at(NwchemTaskPhase phase) const {
    return next < plan.steps.size() && plan.steps[next].phase == phase;
}
const NwchemTaskStep &NwchemTaskCursor::take(NwchemTaskPhase phase) {
    if (!at(phase))
        fail("The NWChem input writer expects a task that the task plan does not have next.");
    const auto &step = plan.steps[next++];
    advance();
    return step;
}
void NwchemTaskCursor::finish() const {
    if (next < plan.steps.size())
        fail("The NWChem input writer left out a task of the task plan: " + plan.steps[next].command);
}
NwchemTaskPlan nwchemTaskPlan(const Document &d) {
    const auto s = nwchemSettings(d);
    const NwchemCalculation calc(s);
    const auto theory = theoryOf(s["method"].toString());
    const auto restart = nwchemRestart(s);
    using Phase = NwchemTaskPhase;
    NwchemTaskPlan p;
    const auto add = [&](Phase phase, const QString &command) { p.steps << NwchemTaskStep{phase, command}; };
    for (const auto &line : nwchemTaskLines(s["extra_input"].toString())) // User text, never generated input.
        add(Phase::Before, line);
    if (!restart.active())
        for (int i = 0; i < s["fragment_guess"].toArray().size(); ++i)
            add(Phase::Fragments, QString("task %1 energy").arg(calc.dft() ? "dft" : "scf"));
    if (calc.task == NwchemTask::Scan) {
        const int count =
            s["scan_steps"].toInt() * (s["scan2_atoms"].toArray().isEmpty() ? 1 : s["scan2_steps"].toInt());
        for (int i = 0; i < count; ++i)
            add(Phase::Main, "task " + theory + (calc.relaxedScan ? " optimize" : " energy"));
    } else if (calc.task == NwchemTask::Irc)
        add(Phase::Main, "task " + theory + " mepgs");
    else if (calc.task == NwchemTask::Neb)
        add(Phase::Main, "task " + theory + " neb");
    else {
        if (calc.optimization())
            add(Phase::Main, "task " + (calc.dft() && s["tddft"].toBool() ? QString("tddft") : theory) +
                                 (calc.saddle() ? " saddle" : " optimize"));
        if (calc.frequencies()) {
            p.frequency = int(p.commands(Phase::Main).size());
            add(Phase::Main,
                "task " + theory + " frequencies" +
                    (nwchemNumericalFrequencies(d) && restart.mode != "reuse" ? " numerical" : ""));
        } else if (calc.dft() && s["tddft"].toBool() && !calc.optimization())
            add(Phase::Main, nwchemCubeGradient(d) ? "task tddft gradient" : "task tddft energy");
        else if (!calc.optimization())
            add(Phase::Main, "task " + theory + ' ' + s["task"].toString());
    }
    const auto last = s["followup"].toBool() ? followupDocument(d) : d;
    const auto finalTheory = theoryOf(nwchemSettings(last)["method"].toString());
    if (s["followup"].toBool())
        add(Phase::Followup, "task " + finalTheory + " energy");
    if (s["prop_mulliken"].toBool() || s["prop_nmr"].toBool() || s["prop_polarizability"].toBool() ||
        s["prop_epr"].toBool())
        add(Phase::Properties, "task " + finalTheory + " property");
    if (s["prop_esp"].toBool())
        add(Phase::Esp, "task esp");
    for (const auto *key : {"prop_esp_grid", "prop_efield_grid"})
        if (s[key].toBool())
            add(Phase::Grids, "task " + finalTheory + " property");
    for (const auto &task : outputTasks(last))
        add(Phase::Output, task);
    if (s["prop_raman"].toBool())
        add(Phase::Raman, "task dft raman");
    if (s["prop_ord"].toBool())
        for (int i = 0; i < s["ord_points"].toInt(); ++i)
            add(Phase::Ord, "task " + theory + " property");
    if (s["prop_spinspin"].toBool())
        add(Phase::SpinSpin, "task dft property");
    for (const auto &line : nwchemTaskLines(s["extra_tasks"].toString()))
        add(Phase::After, line);
    if (restart.mode == "continue") {
        // The tasks the other job finished begin with those of its additional input, which are not
        // generated; the generated ones among them are those the input leaves out.
        const auto done = restart.done.size() - p.commands(Phase::Before).size();
        for (auto &step : p.steps)
            if (step.generated() && p.finished < done) {
                step.skipped = true;
                ++p.finished;
            }
    }
    return p;
}
int nwchemInputDriverSteps(const QString &input) {
    bool driver = false;
    for (const auto &line : input.split('\n')) {
        if (!line.isEmpty() && !line[0].isSpace())
            driver = line == "driver";
        else if (driver && line.trimmed().startsWith("maxiter "))
            return line.trimmed().section(' ', 1, 1).toInt();
    }
    return 0;
}
QStringList nwchemInputStages(const QString &input) {
    // An operation that is also a calculation of the setting "task" has the words of that choice (one
    // table). The others have theirs here: no choice stands for them ("property", "dplot"; a choice of
    // several tasks is none of them), or the choice explains what a stage only names ("IRC", "NEB").
    static const QStringList choices{"energy", "gradient", "optimize", "saddle", "frequencies"};
    static const QHash<QString, const char *> names{{"mepgs", "IRC"},
                                                    {"neb", "NEB"},
                                                    {"property", QT_TR_NOOP("物性")},
                                                    {"dplot", QT_TR_NOOP("CUBE出力")}};
    // An operation without a name stays as written in the input.
    const auto name = [](const QString &written) {
        const auto operation = written == "freq" ? QString("frequencies") : written;
        if (choices.contains(operation))
            for (const auto &choice : nwchemChoices("task"))
                if (choice.value == operation)
                    return choice.label;
        const char *text = names.value(operation);
        return text ? tr(text) : operation;
    };
    QStringList result;
    for (const auto &line : input.split('\n')) {
        if (!line.startsWith("task "))
            continue;
        const auto words = line.split(' ', Qt::SkipEmptyParts);
        // "task dft optimize", "task esp", "task dplot", "task tddft energy".
        const auto theory = words.value(1).toLower(), operation = words.value(2).toLower();
        if (theory == "tddft" && operation == "optimize")
            result << tr("励起状態の構造最適化（TDDFT）");
        else if (theory == "tddft")
            result << tr("励起状態（TDDFT）");
        else if (theory == "esp")
            result << tr("ESP電荷");
        else if (theory == "dplot")
            result << name("dplot");
        else
            result << name(operation.isEmpty() ? "energy" : operation);
    }
    return result;
}
} // namespace mf
