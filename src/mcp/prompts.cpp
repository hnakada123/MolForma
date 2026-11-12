// SPDX-License-Identifier: GPL-2.0-or-later
#include "mcp/prompts.h"
#include <QFileInfo>
#include <QJsonDocument>

namespace mf::mcp_detail {
namespace {
QJsonObject argument(const QString &name, const QString &description, bool required = false) {
    return {{"name", name}, {"description", description}, {"required", required}};
}
QString quoted(const QString &value) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact))
        .mid(1)
        .chopped(1);
}
} // namespace
// User-selected workflow templates (exposed by Claude Code as slash commands). They depend
// only on the tools of this mode, never on the connection.
QJsonArray promptList(const QSet<QString> &tools) {
    const bool run = tools.contains("calculation");
    QJsonArray list{
        QJsonObject{{"name", "review_candidate"},
                    {"title", "Review the pending candidate"},
                    {"description", "Summarize the unconfirmed candidate and ask before confirming it."}},
        QJsonObject{{"name", "geometry_optimization"},
                    {"title", "Optimize geometry with NWChem"},
                    {"description", run ? "Configure, run and check an NWChem geometry optimization."
                                        : "Configure an NWChem geometry optimization and export its input."},
                    {"arguments", QJsonArray{argument("method", "hf, dft, mp2, ccsd or ccsd(t)"),
                                             argument("basis", "Basis set, for example 6-31g*"),
                                             argument("frequencies", "yes to follow with frequencies")}}}};
    if (tools.contains("chemistry"))
        list.append(QJsonObject{{"name", "build_from_smiles"},
                                {"title", "Build a structure from SMILES"},
                                {"description", "Generate a 3D candidate from SMILES and review it."},
                                {"arguments", QJsonArray{argument("smiles", "SMILES string", true)}}});
    if (tools.contains("get_results"))
        list.append(QJsonObject{
            {"name", "frequency_analysis"},
            {"title", "Inspect vibrational modes"},
            {"description", tools.contains("capture_screenshot")
                                ? "Open frequency results, report imaginary modes and animate one."
                                : "Open frequency results and report imaginary modes."},
            {"arguments", QJsonArray{argument("path", "NWChem output file or run directory")}}});
    return list;
}
QString promptText(const QString &name, const QJsonObject &args, const QSet<QString> &tools) {
    const bool gui = tools.contains("get_ui_command");
    const QString start =
        "Use the MolForma MCP tools. Call list_documents, then get_document with include [] "
        "for the documentId and revision; pass the latest revision as expectedRevision. ";
    const QString wait =
        gui ? "If tools/call returns a task, poll it until it completes; otherwise poll get_ui_command "
              "and get_app_state. If a confirmation dialog appears, show its text to the user and "
              "answer only as they decide. "
            : "If tools/call returns a task, poll it until it completes; otherwise poll get_app_state. ";
    if (name == "review_candidate")
        return start +
               "If hasCandidate is false, say so and stop. Otherwise read get_document with include "
               "[\"project\", \"candidate\"], compare formula, atom count, added, removed and "
               "changed atoms, and point out anything chemically suspicious (unusual bond lengths, "
               "overlapping atoms, moved fixed atoms). Do not call history confirm or cancel until the "
               "user explicitly chooses.";
    if (name == "geometry_optimization") {
        const auto method = args["method"].toString(), basis = args["basis"].toString();
        const bool frequencies = args["frequencies"].toString().compare("yes", Qt::CaseInsensitive) == 0;
        QString text = start +
                       "If hasCandidate is true, ask the user to confirm or cancel it first. Call "
                       "get_nwchem to read settings, allowed values and validation. Then call "
                       "set_nwchem with preview false and settings {\"task\": " +
                       quoted(frequencies ? "optimize_frequencies" : "optimize");
        if (!method.isEmpty())
            text += ", \"method\": " + quoted(method);
        if (!basis.isEmpty())
            text += ", \"basis\": " + quoted(basis);
        text += "}. Change charge or multiplicity only if the user asks. Call get_nwchem again and resolve "
                "errors; report warnings. ";
        if (!tools.contains("calculation"))
            return text + "This server cannot run NWChem: return export_document with format nwchem.";
        return text +
               "Call get_execution and resolve its errors; the user, not you, chooses the NWChem executable, "
               "launcher and environment. Then call calculation with action start. " +
               wait +
               "When the calculation finishes, open get_app_state.calculation.directory with results_action "
               "open_directory and read get_results (summary, energies" +
               (frequencies ? ", modes" : "") +
               "). Report the final energy and whether the log shows convergence; exit code zero alone does "
               "not prove convergence." +
               (frequencies ? " List imaginary modes (frequency < 0)." : "");
    }
    if (name == "build_from_smiles")
        return start + "Call chemistry with action smiles and smiles " + quoted(args["smiles"].toString()) +
               ". " + wait +
               "When chemistry is no longer running, read get_document with include [\"candidate\"] and "
               "summarize the formula, atom count and any problems. Ask the user before calling history "
               "confirm; cancel if they decline.";
    const auto path = args["path"].toString();
    QString text =
        start +
        (path.isEmpty() ? QString("Use the results that are already loaded (get_results summary). ")
                        : "Call results_action with action " +
                              quoted(QFileInfo(path).isDir() ? "open_directory" : "open_file") +
                              " and path " + quoted(path) + ". ") +
        wait +
        "Read get_results with section modes, list every mode with its frequency and mark imaginary modes "
        "(frequency < 0). ";
    if (tools.contains("capture_screenshot"))
        text += "If there is an imaginary mode with displacements, play it with results_action play_mode "
                "(amplitude 0.3), capture_screenshot the main window, then stop the animation. ";
    return text + "Explain whether the structure looks like a minimum or a saddle point.";
}
} // namespace mf::mcp_detail
