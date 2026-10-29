#include "mcp/tools.h"
#include "model/units.h"
#include "results/result_summary.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
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
            "state, scan and irc paths with frame indices, bsse, solvation, mulliken/esp/resp charges per "
            "atom, "
            "NMR shielding, polarizability in a.u.) or paginated frames/energies/orbitals/modes/excitations "
            "(TDDFT)/steps (optimization)/log. mode_displacements returns one mode with its reference "
            "geometry. "
            "Frequencies retain negative values for imaginary modes; rigid marks translations and rotations, "
            "and significant marks imaginary frequencies below -10 cm-1 (smaller ones are often numerical "
            "noise).",
        .properties = {{"section", QJsonObject{{"type", "string"},
                                               {"enum", QJsonArray{"summary", "frames", "energies",
                                                                   "orbitals", "modes", "excitations",
                                                                   "steps", "mode_displacements", "log"}}}},
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
        .properties = {{"action", QJsonObject{{"type", "string"},
                                              {"enum", QJsonArray{"start", "stop", "save_bundle"}}}}},
        .required = {"action"},
        .mutation = true,
        .output = output,
        .meta = confirm ? QJsonObject{{"anthropic/requiresUserInteraction", true}} : QJsonObject{},
    };
}
QJsonObject calculationJson(const NwchemRun &run, const QString &savedBundle) {
    return {{"active", run.active()},     {"state", runStateKey(run.state())},
            {"message", run.message()},   {"directory", run.plan().directory},
            {"savedBundle", savedBundle}, {"elapsedSeconds", double(run.elapsedSeconds())}};
}
QJsonObject resultsJson(const ResultData &r, const QJsonObject &args, QJsonObject out) {
    out["format"] = r.format;
    out["status"] = r.status;
    out["selectedOrbital"] = r.selectedOrbital;
    out["orbitalBasis"] = r.orbitalBasis;
    out["orbitalError"] = r.orbitalError;
    out["warnings"] = QJsonArray::fromStringList(r.warnings);
    const auto kinds = resultModeKinds(r);
    const auto modeCounts = resultModeCounts(kinds);
    out["counts"] = QJsonObject{{"frames", r.frames.size()},
                                {"energies", r.energies.size()},
                                {"orbitals", r.orbitals.size()},
                                {"modes", r.modes.size()},
                                {"imaginaryModes", modeCounts.imaginary},
                                {"significantImaginaryModes", modeCounts.significant},
                                {"excitations", r.excitations.size()},
                                {"steps", r.steps.size()}};
    out["properties"] = r.properties;
    const auto section = args["section"].toString("summary");
    int offset = args["offset"].toInt(), limit = std::min(args["limit"].toInt(100), 1000);
    QJsonArray rows;
    qsizetype bytes = 0;
    if (section == "mode_displacements") {
        int i = index(args["index"].toInt(-1), r.modes.size());
        out["frequency"] = r.modes[i].frequency;
        out["geometry"] = r.vibrationGeometry.xyz();
        for (const auto &v : r.modes[i].displacement)
            rows.append(vectorToJson(v));
    } else if (section == "log") {
        out["text"] = r.log.mid(offset, args["limit"].toInt(4096));
        out["total"] = r.log.size();
    } else {
        int count = section == "frames"        ? r.frames.size()
                    : section == "energies"    ? r.energies.size()
                    : section == "orbitals"    ? r.orbitals.size()
                    : section == "modes"       ? r.modes.size()
                    : section == "excitations" ? r.excitations.size()
                    : section == "steps"       ? r.steps.size()
                                               : 0;
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
