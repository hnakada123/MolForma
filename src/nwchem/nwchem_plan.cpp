// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_plan.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"

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
    // Property requests stay: they run at this level, and MOLDEN output must clear them first.
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
QStringList NwchemTaskPlan::commands() const {
    QStringList all;
    for (const auto *phase : {&before, &fragments, &main, &followup, &properties, &esp, &grids, &output,
                              &raman, &ord, &spinspin, &after})
        all += *phase;
    return all;
}
QStringList nwchemOutputTasks(const Document &d) {
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
NwchemTaskPlan nwchemTaskPlan(const Document &d) {
    const auto s = nwchemSettings(d);
    const NwchemCalculation calc(s);
    const auto theory = theoryOf(s["method"].toString());
    NwchemTaskPlan p;
    p.before = nwchemTaskLines(s["extra_input"].toString()); // User text, never generated input.
    if (!nwchemRestart(s).active())
        for (int i = 0; i < s["fragment_guess"].toArray().size(); ++i)
            p.fragments << QString("task %1 energy").arg(calc.dft() ? "dft" : "scf");
    if (calc.task == NwchemTask::Scan) {
        const int count =
            s["scan_steps"].toInt() * (s["scan2_atoms"].toArray().isEmpty() ? 1 : s["scan2_steps"].toInt());
        for (int i = 0; i < count; ++i)
            p.main << "task " + theory + (calc.relaxedScan ? " optimize" : " energy");
    } else if (calc.task == NwchemTask::Irc)
        p.main << "task " + theory + " mepgs";
    else if (calc.task == NwchemTask::Neb)
        p.main << "task " + theory + " neb";
    else {
        if (calc.optimization())
            p.main << "task " + (calc.dft() && s["tddft"].toBool() ? QString("tddft") : theory) +
                          (calc.saddle() ? " saddle" : " optimize");
        if (calc.frequencies()) {
            p.frequency = p.main.size();
            p.main << "task " + theory + " frequencies" +
                          (nwchemNumericalFrequencies(d) && nwchemRestart(s).mode != "reuse" ? " numerical"
                                                                                             : "");
        } else if (calc.dft() && s["tddft"].toBool() && !calc.optimization())
            p.main << (nwchemCubeGradient(d) ? "task tddft gradient" : "task tddft energy");
        else if (!calc.optimization())
            p.main << "task " + theory + ' ' + s["task"].toString();
    }
    const auto last = s["followup"].toBool() ? followupDocument(d) : d;
    const auto finalTheory = theoryOf(nwchemSettings(last)["method"].toString());
    if (s["followup"].toBool())
        p.followup << "task " + finalTheory + " energy";
    if (s["prop_mulliken"].toBool() || s["prop_nmr"].toBool() || s["prop_polarizability"].toBool() ||
        s["prop_epr"].toBool())
        p.properties << "task " + finalTheory + " property";
    if (s["prop_esp"].toBool())
        p.esp << "task esp";
    for (const auto *key : {"prop_esp_grid", "prop_efield_grid"})
        if (s[key].toBool())
            p.grids << "task " + finalTheory + " property";
    p.output = nwchemOutputTasks(last);
    if (s["prop_raman"].toBool())
        p.raman << "task dft raman";
    if (s["prop_ord"].toBool())
        for (int i = 0; i < s["ord_points"].toInt(); ++i)
            p.ord << "task " + theory + " property";
    if (s["prop_spinspin"].toBool())
        p.spinspin << "task dft property";
    p.after = nwchemTaskLines(s["extra_tasks"].toString());
    return p;
}
} // namespace mf
