// SPDX-License-Identifier: GPL-2.0-or-later
#include "mcp/tools.h"
#include "model/setting_target.h"
#include "model/units.h"
#include "nwchem/remote_execution.h"
#include "nwchem/run_diagnostics.h"
#include "results/energy_profile.h"
#include "results/result_summary.h"
#include "results/spectrum.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
QJsonObject integer(int lo, int hi) {
    return {{"type", "integer"}, {"minimum", lo}, {"maximum", hi}};
}
int index(int n, int size) {
    if (n < 0 || n >= size)
        fail("Index is outside the available range (indices are zero-based).");
    return n;
}
QString json(const QJsonObject &value) {
    return QString::fromUtf8(QJsonDocument(value).toJson());
}
QString tail(QString text, bool omitted = false) {
    if (text.size() > McpResourceChars) {
        text = text.right(McpResourceChars);
        omitted = true;
    }
    return omitted
               ? QString("[Earlier text omitted; the last %1 characters follow.]\n").arg(text.size()) + text
               : text;
}
QString fileTail(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    // Four bytes per character bounds the read for any UTF-8 text.
    const qint64 skip = std::max<qint64>(0, file.size() - McpResourceChars * 4);
    file.seek(skip);
    auto text = QString::fromUtf8(file.readAll());
    if (skip > 0)
        text = text.mid(text.indexOf('\n') + 1);
    return tail(text, skip > 0);
}
} // namespace
bool appendPage(QJsonArray &items, const QJsonObject &item, qsizetype &bytes) {
    const auto size = QJsonDocument(item).toJson(QJsonDocument::Compact).size();
    if (size > McpEntryBytes)
        fail("A UI/result entry exceeds the 2 MiB entry limit.");
    if (!items.isEmpty() && bytes + size > McpPageBytes)
        return false;
    bytes += size;
    items.append(item);
    return true;
}
QJsonObject McpToolSpec::definition() const {
    auto inputs = properties;
    auto needed = required;
    if (!global) {
        inputs["documentId"] =
            QJsonObject{{"type", "string"}, {"description", "ID returned by list_documents"}};
        needed.append("documentId");
    }
    if (mutation) {
        inputs["expectedRevision"] =
            QJsonObject{{"type", "string"}, {"description", "Revision from the most recent get_document"}};
        needed.append("expectedRevision");
    }
    // The tools act on MolForma's own workspace and local files only.
    QJsonObject tool{{"name", name},
                     {"title", title},
                     {"description", description},
                     {"outputSchema", output},
                     {"inputSchema", QJsonObject{{"type", "object"},
                                                 {"properties", inputs},
                                                 {"required", needed},
                                                 {"additionalProperties", false}}},
                     {"annotations", QJsonObject{{"readOnlyHint", readOnly && !mutation},
                                                 {"destructiveHint", mutation || !readOnly},
                                                 {"openWorldHint", false}}}};
    if (!meta.isEmpty())
        tool["_meta"] = meta;
    return tool;
}
McpToolSpec resultsTool() {
    return {
        .name = "get_results",
        .title = "Read results",
        .description =
            "Read the loaded result summary (properties from NWChem logs: thermochemistry, dipole, "
            "optimization "
            "state, scan and irc paths with frame indices (a two-dimensional scan adds coordinate2 and "
            "value2 "
            "to each point), bsse, solvation, mulliken/esp/resp charges per "
            "atom, "
            "NMR shielding, NMR spin-spin couplings spinspin {a, b, isotope_a, isotope_b, hz}, "
            "polarizability "
            "in a.u.) or paginated frames/energies/orbitals/modes/excitations "
            "(TDDFT)/steps (optimization)/log. mode_displacements returns one mode with its reference "
            "geometry. "
            "Excitations list their orbital contributions (occupied -> virtual orbital numbers, X/Y "
            "amplitudes NWChem printed above 0.05, weight in % of the printed pairs). A Mulliken analysis "
            "adds valency and free_electrons per atom and bond_indices {a, b, value} (Mayer bond orders, "
            "basis-set dependent; 1-based atoms) to properties: the pairs of |value| >= 0.05, with every "
            "pair and its distance in section bond_indices. "
            "Frequencies retain negative values for imaginary modes; rigid marks translations and rotations, "
            "and significant marks imaginary frequencies below -10 cm-1 (smaller ones are often numerical "
            "noise).",
        .properties =
            {{"section",
              QJsonObject{{"type", "string"},
                          {"enum", QJsonArray{"summary", "frames", "energies", "orbitals", "modes",
                                              "excitations", "steps", "mode_displacements", "spectrum",
                                              "spectrum_peaks", "energy_profile", "bond_indices", "log"}}}},
             {"parameters",
              QJsonObject{
                  {"type", "object"},
                  {"description",
                   "Spectrum parameters: kind nmr/raman/epr/ecd/ord/ir/uv and width (FWHM), optional "
                   "minimum/maximum. ir is over cm-1 and uv (oscillator strengths, Gaussian) over eV. NMR "
                   "needs nucleus 1H/13C and reference_ppm, optional weights keyed by 1-based atom number; "
                   "coupling true with spectrometer_mhz (1H) draws first-order multiplets from the log's "
                   "spin-spin couplings, topologically equivalent nuclei as one peak (atoms, multiplet, "
                   "j_hz). "
                   "EPR needs microwave_ghz, nuclei [{atom,isotope}], optional derivative."}}},
             {"profile",
              QJsonObject{{"type", "object"},
                          {"description",
                           "For energy_profile: molforma-energy-profile v1 specification; records, "
                           "stages, metric, unit, temperature, standard_state, reference. Optional "
                           "corrections of NWChem's thermochemistry: frequency_scale (0.5-1.5, default 1), "
                           "entropy_model rrho/grimme/truhlar with entropy_cutoff (cm-1, default 100), "
                           "standard_state_correction none/1M (1 mol/L). With metric G the stages hold "
                           "boltzmann_factor and rate_constant (Eyring, s-1) against the reference."}}},
             {"offset", integer(0, 10000000)},
             {"limit", integer(1, 65536)},
             {"index", integer(0, 1000000)}},
        .meta = {{"anthropic/maxResultSizeChars", McpLargeResultChars}},
    };
}
McpToolSpec calculationTool(const QString &description, const QJsonObject &output, bool confirm) {
    return {
        .name = "calculation",
        .title = "Run NWChem",
        .description = description,
        .properties =
            {{"action",
              QJsonObject{{"type", "string"},
                          {"enum", QJsonArray{"start", "stop", "save_bundle", "enqueue", "queue_resume",
                                              "queue_pause", "queue_cancel", "queue_move", "queue_policy"}}}},
             {"id", QJsonObject{{"type", "string"}}},
             {"position", integer(0, 999)},
             {"stopOnFailure", QJsonObject{{"type", "boolean"}}},
             {"chainFrom",
              QJsonObject{{"type", "string"},
                          {"description",
                           "enqueue: the queue ID of an earlier entry whose structure this calculation "
                           "takes when that entry has finished (its bundle is saved then)."}}},
             {"chainStructure",
              QJsonObject{{"type", "string"},
                          {"enum", QJsonArray{"final", "highest", "irc_forward", "irc_backward"}},
                          {"description",
                           "Which structure of chainFrom: final, the highest point of a scan/IRC/NEB "
                           "path (NEB ends excluded), or the last point of an IRC direction."}}},
             {"chainSteps",
              QJsonObject{
                  {"type", "array"},
                  {"items", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"saddle", "irc", "ends"}}}},
                  {"uniqueItems", true},
                  {"description", "enqueue with chainFrom: register a reaction-path chain instead of the "
                                  "current task: saddle (TS search + frequencies), irc (both directions), "
                                  "ends (optimize both IRC ends), each with the current method and basis."}}},
             {"endFrequencies",
              QJsonObject{{"type", "boolean"},
                          {"description", "chainSteps ends: also compute frequencies (default true)."}}}},
        .required = {"action"},
        .mutation = true,
        .output = output,
        .meta = confirm ? QJsonObject{{"anthropic/requiresUserInteraction", true}} : QJsonObject{},
    };
}
bool calculationQueueAction(NwchemQueue &queue, const QJsonObject &args) {
    const auto action = args["action"].toString();
    if (action == "queue_resume")
        queue.resume();
    else if (action == "queue_pause")
        queue.pause();
    else if (action == "queue_cancel")
        queue.cancel(args["id"].toString());
    else if (action == "queue_move") {
        if (!args.contains("position"))
            fail("Specify the zero-based queue position.");
        queue.move(args["id"].toString(), args["position"].toInt());
    } else if (action == "queue_policy") {
        if (!args.contains("stopOnFailure"))
            fail("Specify stopOnFailure.");
        queue.setStopOnFailure(args["stopOnFailure"].toBool());
    } else
        return false;
    return true;
}
QJsonObject calculationJson(const NwchemRun &run, const QString &savedBundle, const NwchemQueue *queue) {
    QJsonObject result{{"active", run.active()},     {"state", runStateKey(run.state())},
                       {"message", run.message()},   {"directory", run.plan().directory},
                       {"savedBundle", savedBundle}, {"elapsedSeconds", double(run.elapsedSeconds())}};
    if (queue)
        result["queue"] = queue->json();
    QJsonArray diagnostics;
    for (const auto &value : run.plan().manifest["diagnostics"].toArray()) {
        auto item = value.toObject();
        // Preserve the legacy MCP response field; these are protocol compatibility values,
        // not positions used by the current UI or stored in new job records.
        if (const auto target = settingTargetFromKey(item["target"].toString())) {
            switch (*target) {
            case SettingTarget::Molecule:
                item["tab"] = 0;
                break;
            case SettingTarget::Calculation:
                item["tab"] = 1;
                break;
            case SettingTarget::Execution:
                item["tab"] = 2;
                break;
            }
        }
        item["message"] = runDiagnosisText(item["code"].toString());
        diagnostics.append(item);
    }
    result["diagnostics"] = diagnostics;
    // A run on a remote destination: where, its job there and how it stands.
    if (const auto manifest = run.plan().manifest; remoteRun(manifest)) {
        const auto remote = manifest["remote"].toObject();
        result["remote"] = QJsonObject{{"destination", remote["host"].toObject()["name"]},
                                       {"scheduler", remote["host"].toObject()["scheduler"]},
                                       {"job", remote["job"]},
                                       {"phase", remote["phase"]},
                                       {"state", remote["state"]},
                                       {"text", remoteRunText(manifest)}};
    }
    return result;
}
QJsonObject cubeTermsSchema() {
    return {{"type", "array"},
            {"minItems", 2},
            {"maxItems", 16},
            {"description",
             "For combine_cubes: CUBE files on one grid (the same points, origin and spacing) with their "
             "factors, summed into the CUBE named by path. A difference density rho(AB) - rho(A) - rho(B) "
             "is [{path: AB, factor: 1}, {path: A, factor: -1}, {path: B, factor: -1}]; the atoms written "
             "are those of the first file."},
            {"items", QJsonObject{{"type", "object"},
                                  {"properties", QJsonObject{{"path", QJsonObject{{"type", "string"}}},
                                                             {"factor", QJsonObject{{"type", "number"}}}}},
                                  {"required", QJsonArray{"path", "factor"}},
                                  {"additionalProperties", false}}}};
}
QVector<CubeTerm> cubeTerms(const QJsonValue &value) {
    const auto list = value.toArray();
    if (list.size() < 2 || list.size() > 16)
        fail("Specify terms: 2 to 16 CUBE files with their factors.");
    QVector<CubeTerm> terms;
    for (const auto &item : list) {
        const auto term = item.toObject();
        if (term["path"].toString().isEmpty() || !term["factor"].isDouble())
            fail("Each term needs a path and a numeric factor.");
        terms.append({term["path"].toString(), term["factor"].toDouble(), 0});
    }
    return terms;
}
QJsonObject resultsJson(const ResultData &r, const QJsonObject &args, QJsonObject out) {
    if (args["section"] == "energy_profile")
        return evaluateEnergyProfile(args["profile"].toObject());
    out["format"] = r.format;
    out["status"] = r.status;
    out["selectedOrbital"] = r.selectedOrbital;
    out["orbitalBasis"] = r.orbitalBasis;
    out["orbitalError"] = r.orbitalError;
    out["warnings"] = QJsonArray::fromStringList(r.warnings);
    const auto kinds = resultModeKinds(r);
    const auto modeCounts = resultModeCounts(kinds);
    // Every pair of the bond-index matrix is in its own section; the properties list the bonds.
    auto properties = r.properties;
    const auto bondIndices = r.properties.value("bond_indices").toArray();
    out["counts"] = QJsonObject{{"frames", r.frames.size()},
                                {"energies", r.energies.size()},
                                {"orbitals", r.orbitals.size()},
                                {"modes", r.modes.size()},
                                {"imaginaryModes", modeCounts.imaginary},
                                {"significantImaginaryModes", modeCounts.significant},
                                {"excitations", r.excitations.size()},
                                {"steps", r.steps.size()},
                                {"bondIndices", bondIndices.size()}};
    if (properties.contains("bond_indices")) {
        QJsonArray notable;
        for (const auto &pair : bondIndices)
            if (std::abs(pair.toObject()["value"].toDouble()) >= NotableBondIndex)
                notable.append(pair);
        properties["bond_indices"] = notable;
    }
    out["properties"] = properties;
    const auto section = args["section"].toString("summary");
    int offset = args["offset"].toInt(), limit = std::min(args["limit"].toInt(100), 1000);
    QJsonArray rows;
    qsizetype bytes = 0;
    if (section == "spectrum" || section == "spectrum_peaks") {
        const auto spectrum = calculateSpectrum(r, args["parameters"].toObject());
        out["parameters"] = spectrum.parameters;
        out["xUnit"] = spectrum.xUnit;
        out["yUnit"] = spectrum.yUnit;
        out["reversed"] = spectrum.reversed;
        const int count = section == "spectrum" ? spectrum.curve.size() : spectrum.peaks.size();
        for (int i = std::max(0, offset); i < std::min(count, offset + limit); ++i)
            rows.append(section == "spectrum"
                            ? QJsonValue(QJsonArray{spectrum.curve[i].x(), spectrum.curve[i].y()})
                            : spectrum.peaks[i]);
        out["total"] = count;
        out["offset"] = std::max(0, offset);
        out["nextOffset"] = std::max(0, offset) + rows.size();
    } else if (section == "mode_displacements") {
        int i = index(args["index"].toInt(-1), r.modes.size());
        out["frequency"] = r.modes[i].frequency;
        out["geometry"] = r.vibrationGeometry.xyz();
        for (const auto &v : r.modes[i].displacement)
            rows.append(vectorToJson(v));
    } else if (section == "log") {
        out["text"] = r.log.mid(offset, args["limit"].toInt(4096));
        out["total"] = r.log.size();
    } else {
        int count = section == "frames"         ? r.frames.size()
                    : section == "energies"     ? r.energies.size()
                    : section == "orbitals"     ? r.orbitals.size()
                    : section == "modes"        ? r.modes.size()
                    : section == "excitations"  ? r.excitations.size()
                    : section == "steps"        ? r.steps.size()
                    : section == "bond_indices" ? bondIndices.size()
                                                : 0;
        // Distances of atom pairs in the last structure, which a Mulliken analysis is of.
        const Document *molecule = r.frames.isEmpty() ? nullptr : &r.frames.last().molecule;
        for (int i = offset; i < std::min(count, offset + limit); ++i) {
            QJsonObject row{{"index", i}};
            if (section == "frames") {
                row["label"] = r.frames[i].label;
                row["xyz"] = r.frames[i].molecule.xyz();
            }
            if (section == "energies") {
                row["method"] = r.energies[i].method;
                row["hartree"] = r.energies[i].hartree;
                row["line"] = r.energies[i].line;
            }
            if (section == "orbitals") {
                const auto &o = r.orbitals[i];
                row["number"] = o.number;
                row["energy"] = o.energy;
                row["occupation"] = o.occupation;
                row["spin"] = o.spin;
                row["symmetry"] = o.symmetry;
            }
            if (section == "modes") {
                const auto &m = r.modes[i];
                row["frequency"] = m.frequency;
                row["imaginary"] = imaginaryMode(kinds[i]);
                row["significant"] = kinds[i] == ResultModeKind::SignificantImaginary;
                row["rigid"] = kinds[i] == ResultModeKind::Rigid;
                row["intensity"] = std::isfinite(m.intensity) ? QJsonValue(m.intensity) : QJsonValue();
                row["hasDisplacements"] = !m.displacement.isEmpty();
            }
            if (section == "excitations") {
                const auto &x = r.excitations[i];
                row["root"] = x.root;
                row["spin"] = x.spin;
                row["symmetry"] = x.symmetry;
                row["ev"] = x.ev;
                row["nm"] = EvNanometer / x.ev;
                row["oscillator"] = x.oscillator;
                row["s2"] = std::isfinite(x.s2) ? QJsonValue(x.s2) : QJsonValue();
                QJsonArray contributions;
                for (const auto &p : resultTransitions(x)) {
                    QJsonObject pair{{"from", p.from},
                                     {"to", p.to},
                                     {"spin", p.spin},
                                     {"x", p.x},
                                     {"transition", resultTransitionText(r, p)}};
                    if (std::isfinite(p.y))
                        pair["y"] = p.y;
                    if (std::isfinite(p.weight))
                        pair["weight"] = p.weight;
                    contributions.append(pair);
                }
                row["contributions"] = contributions;
            }
            if (section == "bond_indices") {
                const auto pair = bondIndices[i].toObject();
                const int a = pair["a"].toInt(), b = pair["b"].toInt();
                row["a"] = a;
                row["b"] = b;
                row["value"] = pair["value"];
                if (molecule && a >= 1 && b >= 1 && std::max(a, b) <= molecule->atoms.size()) {
                    row["elements"] =
                        QJsonArray{molecule->atoms[a - 1].element, molecule->atoms[b - 1].element};
                    row["distance"] =
                        (molecule->atoms[a - 1].position - molecule->atoms[b - 1].position).norm();
                }
            }
            if (section == "steps") {
                const auto &t = r.steps[i];
                row["step"] = t.step;
                row["energy"] = t.energy;
                row["deltaE"] = t.deltaE;
                row["gmax"] = t.gmax;
                row["grms"] = t.grms;
                row["xrms"] = t.xrms;
                row["xmax"] = t.xmax;
            }
            if (!appendPage(rows, row, bytes))
                break;
        }
        out["total"] = count;
        out["offset"] = offset;
        out["nextOffset"] = offset + rows.size();
    }
    if (section != "summary" && section != "log")
        out["items"] = rows;
    return out;
}
QJsonArray runtimeResources() {
    const auto resource = [](const QString &uri, const QString &name, const QString &title,
                             const QString &description, const QString &mimeType) {
        return QJsonObject{{"uri", uri},
                           {"name", name},
                           {"title", title},
                           {"description", description},
                           {"mimeType", mimeType}};
    };
    return {resource("molforma://app/state", "app-state", "Application state",
                     "Calculation and result-loading state (same as get_app_state).", "application/json"),
            resource("molforma://calculation/output", "calculation-output", "NWChem output",
                     "End of output.out of the current or last calculation of this process.", "text/plain"),
            resource("molforma://calculation/stderr", "calculation-stderr", "NWChem standard error",
                     "End of stderr.err (MPI and launcher messages) of the current or last calculation.",
                     "text/plain"),
            resource("molforma://results/summary", "results-summary", "Result summary",
                     "Summary of the loaded result (same as get_results summary).", "application/json"),
            resource("molforma://results/log", "results-log", "Result log", "End of the loaded result's log.",
                     "text/plain")};
}
std::optional<QJsonObject> runtimeResource(const QString &uri, const QJsonObject &state,
                                           const ResultData &result, const ExecutionPlan &run) {
    QString text, mimeType = "text/plain";
    if (uri == "molforma://app/state") {
        text = json(state);
        mimeType = "application/json";
    } else if (uri == "molforma://calculation/output" || uri == "molforma://calculation/stderr") {
        const auto file = uri.endsWith("stderr") ? QString("stderr.err") : QString("output.out");
        if (run.directory.isEmpty())
            text = "No calculation has been started in this MolForma process.";
        else if (!QFile::exists(QDir(run.directory).filePath(file)))
            text = file + " has not been written yet: " + run.directory;
        else
            text = fileTail(QDir(run.directory).filePath(file));
    } else if (uri == "molforma://results/summary") {
        text = json(resultsJson(result, {{"section", "summary"}}, state["results"].toObject()));
        mimeType = "application/json";
    } else if (uri == "molforma://results/log") {
        text = result.source.isEmpty() ? "No result is loaded." : tail(result.log);
    } else
        return std::nullopt;
    return QJsonObject{{"uri", uri}, {"mimeType", mimeType}, {"text", text}};
}
} // namespace mf
