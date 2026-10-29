#include "mcp/server.h"
#include "editing/atom_properties.h"
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/operations.h"
#include "io/structure_io.h"
#include "mcp/tools.h"
#include "model/geometry.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <cmath>

namespace mf {
namespace {
constexpr int MaxAtoms = 10000;
constexpr int SummaryIds = 100;
QJsonObject serverInfo() {
    return {{"name", "molforma"}, {"title", "MolForma"}, {"version", MOLFORMA_VERSION}};
}
QJsonArray versions() {
    return {McpProtocolVersion, McpLegacyVersion};
}
QJsonObject capabilities(bool tasks = false) {
    QJsonObject c{{"tools", QJsonObject{}}, {"resources", QJsonObject{}}, {"prompts", QJsonObject{}}};
    if (tasks)
        c["extensions"] = QJsonObject{{McpTasksExtension, QJsonObject{}}};
    return c;
}
// Settings tools refuse only the errors their change brings in. Problems already there (an empty
// molecule, a multiplicity still to be chosen) are listed by get_nwchem/get_execution and keep other
// settings editable, as in the GUI.
void failOnAddedErrors(const ValidationReport &before, const ValidationReport &after) {
    QSet<QString> existing;
    for (const auto &d : before.diagnostics)
        if (d.severity == DiagnosticSeverity::Error)
            existing.insert(d.identity());
    QStringList added;
    for (const auto &d : after.diagnostics)
        if (d.severity == DiagnosticSeverity::Error && !existing.contains(d.identity()))
            added << d.message;
    if (!added.isEmpty())
        fail(added.join('\n'));
}
// Thrown by a tool that needs client input first (multi round-trip request).
struct InputRequired {
    QJsonObject inputRequests;
    QString requestState;
};
QJsonObject elicit(const QString &message, const QString &field, const QJsonObject &schema) {
    return {
        {"method", "elicitation/create"},
        {"params", QJsonObject{{"mode", "form"},
                               {"message", message},
                               {"requestedSchema", QJsonObject{{"type", "object"},
                                                               {"properties", QJsonObject{{field, schema}}},
                                                               {"required", QJsonArray{field}}}}}}};
}
QString timestamp(const QDateTime &time) {
    return time.toUTC().toString(Qt::ISODateWithMs);
}
// CacheableResult hints (2026-07-28). Documents change through GUI edits, so nothing is cached.
QJsonObject cacheable(QJsonObject value, bool modern) {
    if (modern) {
        value["ttlMs"] = 0;
        value["cacheScope"] = "private";
    }
    return value;
}
QString instructions() {
    return "Use list_documents to obtain the explicit documentId, then get_document for stable atom IDs "
           "and the current revision. All mutations require expectedRevision. Edits default to a separate "
           "candidate and return only a change summary; inspect the candidate with get_document before "
           "history(confirm). get_document include:[] returns only metadata for large documents. Coordinates "
           "are in angstrom. The workspace "
           "belongs to this MolForma process and expires when it exits. Export or save to retain work. "
           "In GUI mode, get_ui discovers controls and dialogs. GUI mutations return a commandId: poll "
           "get_ui_command and inspect get_ui for modal dialogs; answer them with explicit calls. Clients "
           "declaring the Tasks extension receive a task instead, which also covers the started background "
           "work and surfaces confirmation dialogs as elicitations. "
           "A completed GUI command is not proof that a background calculation converged. Both modes run "
           "NWChem with calculation and read results with results_action/get_results. Over MCP the NWChem "
           "executable, launcher and environment overrides are chosen by the user, not by the client.";
}
QJsonObject error(const QJsonValue &id, int code, const QString &message, QJsonObject data = {},
                  bool legacyNullId = false) {
    QJsonObject e{{"code", code}, {"message", message}};
    if (!data.isEmpty())
        e["data"] = data;
    QJsonObject response{{"jsonrpc", "2.0"}, {"error", e}};
    // The modern error schema omits an unreadable ID; legacy JSON-RPC uses null.
    if ((!id.isNull() && !id.isUndefined()) || legacyNullId)
        response["id"] = id;
    return response;
}
QJsonObject result(const QJsonValue &id, QJsonObject value, bool modern) {
    if (modern) {
        if (!value.contains("resultType"))
            value["resultType"] = "complete";
        value["_meta"] = QJsonObject{{"io.modelcontextprotocol/serverInfo", serverInfo()}};
    }
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", value}};
}
QJsonObject toolResult(QJsonObject value) {
    const auto image = value.take("_mcpImage");
    QJsonArray content{QJsonObject{
        {"type", "text"}, {"text", QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact))}}};
    if (image.isObject())
        content.append(image);
    return {{"content", content}, {"structuredContent", value}, {"isError", false}};
}
QJsonObject type(const QString &name, const QString &description = {}) {
    QJsonObject o{{"type", name}};
    if (!description.isEmpty())
        o["description"] = description;
    return o;
}
QJsonObject choice(const QStringList &values) {
    return {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}};
}
QJsonObject vectorSchema() {
    return {{"type", "array"}, {"items", type("number")}, {"minItems", 3}, {"maxItems", 3}};
}
QJsonObject idsSchema() {
    return {{"type", "array"}, {"items", type("string")}, {"minItems", 1}, {"uniqueItems", true}};
}
// Validate the fixed schemas published by this server (JSON Schema 2020-12).
// No client-supplied schemas or remote $refs are evaluated.
void validateValue(const QJsonValue &value, const QJsonObject &schema, const QString &path) {
    const auto matches = [&](const QString &t) {
        return t == "object"    ? value.isObject()
               : t == "array"   ? value.isArray()
               : t == "string"  ? value.isString()
               : t == "boolean" ? value.isBool()
               : t == "integer" ? value.isDouble() && std::isfinite(value.toDouble()) &&
                                      std::floor(value.toDouble()) == value.toDouble()
               : t == "number"  ? value.isDouble() && std::isfinite(value.toDouble())
               : t == "null"    ? value.isNull()
                                : false;
    };
    const auto t = schema["type"];
    bool valid = !schema.contains("type") || matches(t.toString());
    for (const auto &alternative : t.toArray())
        valid |= matches(alternative.toString());
    if (!valid)
        fail(path + ": incorrect value type");
    if (value.isDouble() &&
        ((schema.contains("minimum") && value.toDouble() < schema["minimum"].toDouble()) ||
         (schema.contains("maximum") && value.toDouble() > schema["maximum"].toDouble())))
        fail(path + ": value out of range");
    if (schema.contains("enum") && !schema["enum"].toArray().contains(value))
        fail(path + ": unsupported value");
    if (value.isObject()) {
        const auto o = value.toObject(), properties = schema["properties"].toObject();
        for (auto required : schema["required"].toArray())
            if (!o.contains(required.toString()))
                fail(path + ": missing " + required.toString());
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (properties.contains(it.key()))
                validateValue(it.value(), properties[it.key()].toObject(), path + "." + it.key());
            else if (schema["additionalProperties"] == false)
                fail(path + ": unknown argument " + it.key());
        }
    }
    if (value.isArray()) {
        auto a = value.toArray();
        if (a.size() < schema["minItems"].toInt() ||
            (schema.contains("maxItems") && a.size() > schema["maxItems"].toInt()))
            fail(path + ": invalid array length");
        QSet<QByteArray> seen;
        for (const auto &v : a) {
            validateValue(v, schema["items"].toObject(), path + "[]");
            const auto key = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
            if (schema["uniqueItems"].toBool() && seen.contains(key))
                fail(path + ": duplicate item");
            seen.insert(key);
        }
    }
}
QJsonObject object(const QJsonObject &properties, const QStringList &required) {
    return {
        {"type", "object"}, {"properties", properties}, {"required", QJsonArray::fromStringList(required)}};
}
QJsonObject strings() {
    return {{"type", "array"}, {"items", type("string")}};
}
QString toolTitle(const QString &name) {
    static const QHash<QString, QString> titles{{"list_documents", "List documents"},
                                                {"get_document", "Read document"},
                                                {"list_templates", "List templates"},
                                                {"place_template", "Place template"},
                                                {"add_atom", "Add atom"},
                                                {"set_positions", "Set atom positions"},
                                                {"set_bond", "Set bond"},
                                                {"delete_atoms", "Delete atoms"},
                                                {"translate", "Translate atoms"},
                                                {"measure", "Measure geometry"},
                                                {"set_dimension", "Set distance, angle or dihedral"},
                                                {"set_atom_properties", "Set atom properties"},
                                                {"import_document", "Import structure"},
                                                {"open_file", "Open structure file"},
                                                {"export_document", "Export document"},
                                                {"save_file", "Save project"},
                                                {"get_nwchem", "Read NWChem settings"},
                                                {"get_advanced", "Read advanced settings"},
                                                {"set_advanced", "Set advanced settings"},
                                                {"get_execution", "Read execution settings"},
                                                {"set_execution", "Set execution settings"},
                                                {"set_nwchem", "Set NWChem settings"},
                                                {"history", "Confirm, cancel, undo or redo"}};
    return titles.value(name);
}
// Results are checked against these schemas before they are returned.
QJsonObject outputSchema(const QString &name) {
    const auto text = type("string");
    const auto summary = object({{"documentId", text},
                                 {"revision", text},
                                 {"formula", text},
                                 {"atomCount", type("integer")},
                                 {"candidate", QJsonObject{{"type", QJsonArray{"object", "null"}}}}},
                                {"documentId", "revision", "formula", "atomCount", "candidate"});
    if (name == "get_document")
        return object({{"documentId", text},
                       {"revision", text},
                       {"formula", text},
                       {"atomCount", type("integer")},
                       {"project", type("object")},
                       {"candidate", QJsonObject{{"type", QJsonArray{"object", "null"}}}},
                       {"hasCandidate", type("boolean")}},
                      {"documentId", "revision", "formula", "atomCount"});
    if (name == "list_documents")
        return object(
            {{"documents",
              QJsonObject{
                  {"type", "array"},
                  {"items",
                   object({{"documentId", text}, {"revision", text}, {"uri", text}, {"gui", type("boolean")}},
                          {"documentId", "revision", "uri", "gui"})}}}},
            {"documents"});
    if (name == "list_templates")
        return object(
            {{"templates", QJsonObject{{"type", "array"}, {"items", object({{"id", text}}, {"id"})}}}},
            {"templates"});
    if (name == "measure")
        return object({{"value", type("number")}, {"unit", choice({"angstrom", "degree"})}},
                      {"value", "unit"});
    if (name == "export_document")
        return object({{"text", text}, {"format", text}, {"revision", text}}, {"text", "format", "revision"});
    if (name == "save_file")
        return object({{"path", text}, {"revision", text}}, {"path", "revision"});
    if (name == "get_nwchem")
        return object({{"settings", type("object")},
                       {"defaults", type("object")},
                       {"choices", type("object")},
                       {"errors", strings()},
                       {"warnings", strings()}},
                      {"settings", "defaults", "choices", "errors", "warnings"});
    if (name == "get_advanced")
        return object({{"settings", type("object")}, {"options", type("array")}}, {"settings", "options"});
    if (name == "get_execution")
        return object({{"settings", type("object")},
                       {"defaults", type("object")},
                       {"errors", strings()},
                       {"warnings", strings()}},
                      {"settings", "defaults", "errors", "warnings"});
    return summary;
}
QJsonObject argument(const QString &name, const QString &description, bool required = false) {
    return {{"name", name}, {"description", description}, {"required", required}};
}
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
QString quoted(const QString &value) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact))
        .mid(1)
        .chopped(1);
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
void validateDocument(const Document &d) {
    if (d.atoms.size() > MaxAtoms)
        fail("MCP documents are limited to 10000 atoms.");
    for (const auto &a : d.atoms)
        element(a.element);
    auto errors = d.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
}
} // namespace

McpServer::McpServer(McpEditor hooks, McpPolicy rules, QObject *parent)
    : QObject(parent), editor(std::move(hooks)), policy(std::move(rules)) {
    if (editor.document && (!editor.candidate || !editor.edit || !editor.history))
        fail("Incomplete MCP editor hooks");
    if (bool(editor.additionalTools) != bool(editor.callAdditionalTool))
        fail("Incomplete MCP additional tool hooks");
    if (bool(editor.taskState) != bool(editor.taskWork))
        fail("Incomplete MCP task hooks");
}
void McpServer::resetDocument(const Document &initial) {
    session.reset(initial);
}
QString McpServer::documentId() const {
    return id;
}
Document McpServer::document() const {
    return editor.document ? editor.document() : session.document();
}
std::optional<Document> McpServer::candidate() const {
    return editor.candidate ? editor.candidate() : session.candidate();
}
QString McpServer::revision() const {
    auto c = candidate();
    auto bytes = QJsonDocument(QJsonObject{{"document", document().project()},
                                           {"candidate", c ? QJsonValue(c->project()) : QJsonValue()}})
                     .toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QJsonObject McpServer::snapshot() const {
    const auto d = document();
    const auto c = candidate();
    return {{"documentId", id},       {"revision", revision()},
            {"formula", d.formula()}, {"atomCount", d.atoms.size()},
            {"project", d.project()}, {"candidate", c ? QJsonValue(c->project()) : QJsonValue()}};
}
QJsonObject McpServer::summary() const {
    const auto d = document();
    const auto c = candidate();
    QJsonObject out{{"documentId", id},
                    {"revision", revision()},
                    {"formula", d.formula()},
                    {"atomCount", d.atoms.size()},
                    {"candidate", QJsonValue()}};
    if (!c)
        return out;
    // Atom-level differences let a client review a candidate without reading the full project.
    QJsonArray added, removed, changed;
    bool truncated = false;
    const auto note = [&](QJsonArray &list, const QString &atomId) {
        if (list.size() < SummaryIds)
            list.append(atomId);
        else
            truncated = true;
    };
    for (const auto &a : c->atoms)
        if (!d.contains(a.id))
            note(added, a.id);
        else if (d.atom(a.id).toJson() != a.toJson())
            note(changed, a.id);
    for (const auto &a : d.atoms)
        if (!c->contains(a.id))
            note(removed, a.id);
    out["candidate"] =
        QJsonObject{{"formula", c->formula()}, {"atomCount", c->atoms.size()}, {"addedIds", added},
                    {"removedIds", removed},   {"changedIds", changed},        {"idsTruncated", truncated}};
    return out;
}
bool McpServer::elicitation() const {
    const auto e = incoming.capabilities["elicitation"];
    return incoming.modern && e.isObject() && (e.toObject().isEmpty() || e.toObject().contains("form"));
}
// requestState is client-held: authenticate it and bind it to a short expiry.
QString McpServer::sealState(const QJsonObject &state) const {
    auto payload = state;
    payload["expires"] = QDateTime::currentSecsSinceEpoch() + 600;
    const auto bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const auto mac = QMessageAuthenticationCode::hash(bytes, stateKey, QCryptographicHash::Sha256);
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding) + '.' + mac.toHex());
}
std::optional<QJsonObject> McpServer::openState() const {
    const auto parts = incoming.requestState.toLatin1().split('.');
    if (parts.size() != 2)
        return std::nullopt;
    const auto bytes = QByteArray::fromBase64(parts[0], QByteArray::Base64UrlEncoding);
    const auto mac = QMessageAuthenticationCode::hash(bytes, stateKey, QCryptographicHash::Sha256).toHex();
    auto state = QJsonDocument::fromJson(bytes).object();
    if (mac != parts[1] || state["expires"].toInteger() < QDateTime::currentSecsSinceEpoch())
        return std::nullopt;
    state.remove("expires");
    return state;
}
QJsonObject McpServer::createTask(const QString &tool, const QString &work) {
    const auto taskId = newId();
    const auto now = QDateTime::currentDateTimeUtc();
    tasks[taskId] = {work, tool, "working", now, now};
    taskOrder.append(taskId);
    while (taskOrder.size() > 128)
        tasks.remove(taskOrder.takeFirst());
    return {{"resultType", "task"},        {"taskId", taskId},
            {"status", "working"},         {"statusMessage", "Queued " + tool},
            {"createdAt", timestamp(now)}, {"lastUpdatedAt", timestamp(now)},
            {"ttlMs", QJsonValue()},       {"pollIntervalMs", 500}};
}
QJsonObject McpServer::taskResult(const QString &taskId) {
    auto &task = tasks[taskId];
    const auto work = editor.taskState(task.work);
    if (!work) {
        tasks.remove(taskId);
        taskOrder.removeAll(taskId);
        fail("Failed to retrieve task: Task has expired");
    }
    const auto state = (*work)["state"].toString();
    auto message = (*work)["message"].toString();
    QJsonObject out;
    QString status = "working";
    // Work that ends after a granted tasks/cancel is reported as cancelled.
    if (state == "cancelled" || (task.cancelRequested && (state == "done" || state == "failed"))) {
        status = "cancelled";
        if (message.isEmpty())
            message = "Cancelled by tasks/cancel.";
    } else if (state == "done") {
        status = "completed";
        out["result"] = toolResult((*work)["result"].toObject());
    } else if (state == "failed") {
        // A failed GUI command is a tool execution error, not a JSON-RPC failure.
        status = "completed";
        out["result"] = QJsonObject{
            {"isError", true},
            {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", (*work)["error"].toString()}}}}};
    } else if (const auto prompt = editor.pendingPrompt ? editor.pendingPrompt() : QJsonObject{};
               !prompt.isEmpty()) {
        if (elicitation()) {
            status = "input_required";
            out["inputRequests"] =
                QJsonObject{{prompt["key"].toString(),
                             elicit(prompt["message"].toString(), "choice",
                                    {{"type", "string"}, {"title", "Choice"}, {"enum", prompt["choices"]}})}};
        } else {
            message += " A confirmation dialog is open; read it with get_ui and answer with ui_activate.";
        }
    }
    const auto now = QDateTime::currentDateTimeUtc();
    if (task.state != status + message) {
        task.state = status + message;
        task.updated = now;
    }
    out["taskId"] = taskId;
    out["status"] = status;
    out["statusMessage"] = message;
    out["createdAt"] = timestamp(task.created);
    out["lastUpdatedAt"] = timestamp(task.updated);
    out["ttlMs"] = QJsonValue();
    out["pollIntervalMs"] = 500;
    return out;
}
void McpServer::edit(const QString &name, const std::function<void(Document &)> &operation, bool preview) {
    const auto checked = [&](Document &d) {
        operation(d);
        validateDocument(d); // MCP's input size limits apply in both frontends.
    };
    if (editor.edit)
        editor.edit(name, checked, preview);
    else
        session.edit(name, checked, preview, true);
}
void McpServer::history(const QString &action) {
    if (editor.history)
        editor.history(action);
    else
        session.navigate(action);
}
QJsonArray McpServer::toolList() const {
    QJsonArray list;
    auto add = [&](const QString &name, const QString &description, QJsonObject properties = {},
                   QJsonArray required = {}, bool scoped = true, bool mutation = false) {
        if (scoped) {
            properties["documentId"] = type("string", "ID returned by list_documents");
            required.append("documentId");
        }
        if (mutation) {
            properties["expectedRevision"] = type("string", "Revision from the most recent get_document");
            required.append("expectedRevision");
            if (name != "history") {
                properties["preview"] = QJsonObject{{"type", "boolean"}, {"default", true}};
            }
        }
        QJsonObject tool{{"name", name},
                         {"title", toolTitle(name)},
                         {"description", description},
                         {"outputSchema", outputSchema(name)},
                         {"inputSchema", QJsonObject{{"type", "object"},
                                                     {"properties", properties},
                                                     {"required", required},
                                                     {"additionalProperties", false}}},
                         {"annotations", QJsonObject{{"readOnlyHint", !mutation && name != "save_file"},
                                                     {"destructiveHint", mutation || name == "save_file"},
                                                     {"openWorldHint", false}}}};
        // Claude Code hints: keep large documents inline, and confirm program choices with a person.
        QJsonObject meta;
        if (name == "get_document" || name == "export_document")
            meta["anthropic/maxResultSizeChars"] = McpLargeResultChars;
        if (name == "set_execution" && policy.allowExecutionSettings)
            meta["anthropic/requiresUserInteraction"] = true;
        if (!meta.isEmpty())
            tool["_meta"] = meta;
        list.append(tool);
    };
    add("list_documents", "List the workspace owned by this process. Its ID expires on process exit.", {}, {},
        false);
    add("get_document",
        "Read the revision with the committed project and/or separate candidate (stable atom IDs). "
        "include defaults to both; [] returns only metadata.",
        {{"include", QJsonObject{{"type", "array"},
                                 {"items", choice({"project", "candidate"})},
                                 {"uniqueItems", true}}}});
    add("list_templates", "List built-in molecular templates and their IDs.", {}, {}, false);
    add("place_template", "Place a built-in template at an angstrom position.",
        {{"templateId", type("string")}, {"position", vectorSchema()}}, {"templateId", "position"}, true,
        true);
    add("add_atom", "Add an atom; hydrogen completion defaults to false. Optionally bond to anchorId.",
        {{"element", type("string")},
         {"position", vectorSchema()},
         {"anchorId", type("string")},
         {"hydrogens", type("boolean")}},
        {"element", "position"}, true, true);
    add("set_positions", "Set atom positions by stable ID in angstrom. Fixed atoms cannot move.",
        {{"positions",
          QJsonObject{{"type", "array"},
                      {"minItems", 1},
                      {"maxItems", MaxAtoms},
                      {"items", QJsonObject{{"type", "object"},
                                            {"properties", QJsonObject{{"id", type("string")},
                                                                       {"position", vectorSchema()}}},
                                            {"required", QJsonArray{"id", "position"}},
                                            {"additionalProperties", false}}}}}},
        {"positions"}, true, true);
    add("set_bond", "Set or remove a bond between stable atom IDs.",
        {{"a", type("string")},
         {"b", type("string")},
         {"kind", choice({"single", "double", "triple", "aromatic", "dative", "none"})}},
        {"a", "b", "kind"}, true, true);
    add("delete_atoms", "Delete atoms and update dependent references.", {{"ids", idsSchema()}}, {"ids"},
        true, true);
    add("translate", "Translate atoms in angstrom; reject selections containing fixed atoms.",
        {{"ids", idsSchema()}, {"offset", vectorSchema()}}, {"ids", "offset"}, true, true);
    add("measure", "Measure 2/3/4 ordered atoms: distance (angstrom), angle/dihedral (degrees).",
        {{"ids", idsSchema()}}, {"ids"});
    add("set_dimension",
        "Set the distance (angstrom), angle or dihedral (degrees) of 2/3/4 ordered atoms, as in the editor: "
        "the side of the later atom moves (for unbonded atoms of different molecules, the whole later "
        "molecule). Fixed atoms cannot move.",
        {{"ids", idsSchema()}, {"value", type("number")}}, {"ids", "value"}, true, true);
    add("set_atom_properties",
        "Set properties of atoms by stable ID, as in the editor's atom panel. element replaces the element "
        "(hydrogens: adjust automatic hydrogens); charge is the formal charge; radical the unpaired "
        "electrons; "
        "isotope a mass number (0 clears it) or mass an exact mass in amu; fixed keeps atoms still in edits "
        "and optimizations; ghost makes basis-only centers; name the atom name.",
        {{"ids", idsSchema()},
         {"element", type("string")},
         {"hydrogens", type("boolean")},
         {"charge",
          QJsonObject{{"type", "integer"}, {"minimum", MinimumAtomCharge}, {"maximum", MaximumAtomCharge}}},
         {"radical", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", MaximumAtomRadical}}},
         {"isotope", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 400}}},
         {"mass", QJsonObject{{"type", "number"}, {"exclusiveMinimum", 0}}},
         {"fixed", type("boolean")},
         {"ghost", type("boolean")},
         {"name", type("string")}},
        {"ids"}, true, true);
    add("import_document",
        "Replace structure from XYZ text (preserving project metadata) or a complete "
        "MolForma project JSON string (.molforma.json, version 1). With format xyz and append true, the XYZ "
        "molecule is added 3 Å "
        "beside the current one instead (e.g. to build a dimer). This is undoable.",
        {{"format", choice({"xyz", "project"})}, {"text", type("string")}, {"append", type("boolean")}},
        {"format", "text"}, true, true);
    add("open_file",
        "Load a local XYZ/MOL/SDF/Mol2/CML/PDB/MolForma project file, NWChem input (.nw) or Gaussian input "
        "(.gjf/.com) "
        "as an undoable edit. Structure files keep the calculation settings; NWChem and Gaussian inputs also "
        "set charge and multiplicity, and NWChem inputs the calculation settings they share with MolForma. "
        "Multi-frame XYZ files need frame (0-based). PDB uses model 1 and automatic alternate-location "
        "selection. Maximum file size: 8 MiB.",
        {{"path", type("string")}, {"frame", QJsonObject{{"type", "integer"}, {"minimum", 0}}}}, {"path"},
        true, true);
    add("export_document",
        "Return the committed document as XYZ, MolForma project JSON, or validated NWChem input.",
        {{"format", choice({"xyz", "project", "nwchem"})}}, {"format"});
    add("save_file",
        "Save the committed MolForma project (.molforma.json) locally. Refuse existing paths unless "
        "overwrite is true; reject pending candidates. Does not mark GUI history clean.",
        {{"path", type("string")}, {"overwrite", type("boolean")}, {"expectedRevision", type("string")}},
        {"path", "expectedRevision"});
    add("get_nwchem",
        "Read NWChem settings, defaults, allowed choice values and validation errors/warnings.");
    add("get_advanced", "Read all advanced/output options, their types, applicability and stored values.");
    add("set_advanced", "Merge typed advanced/output settings. A null value resets that option.",
        {{"settings", type("object")}}, {"settings"}, true, true);
    add("get_execution", "Read launch settings and local validation. Never starts a process.");
    add("set_execution",
        policy.allowExecutionSettings
            ? "Merge launch settings, including the executable, launcher and environment overrides. Does not "
              "run."
            : "Merge launch settings. The executable, launcher and environment overrides are set by the user "
              "in MolForma and cannot be changed over MCP. Does not run.",
        {{"settings", type("object")}}, {"settings"}, true, true);
    add("set_nwchem",
        "Merge NWChem settings and optional charge/multiplicity. Unknown keys or invalid "
        "calculation conditions are rejected.",
        {{"settings", type("object")}, {"charge", type("integer")}, {"multiplicity", type("integer")}},
        {"settings"}, true, true);
    add("history", "Confirm/cancel a candidate, or undo/redo a committed edit.",
        {{"action", choice({"confirm", "cancel", "undo", "redo"})}}, {"action"}, true, true);
    if (editor.additionalTools)
        for (auto tool : editor.additionalTools())
            list.append(tool);
    return list;
}

QSet<QString> McpServer::toolNames() const {
    QSet<QString> names;
    for (const auto &tool : toolList())
        names.insert(tool.toObject()["name"].toString());
    return names;
}
QJsonArray McpServer::resourceList() const {
    const auto uri = "molforma://documents/" + id;
    QJsonArray list{
        QJsonObject{{"uri", uri},
                    {"name", "document"},
                    {"title", "MolForma document"},
                    {"description", "Committed project, candidate and revision (same as get_document)."},
                    {"mimeType", "application/json"}},
        QJsonObject{{"uri", uri + "/xyz"},
                    {"name", "document-xyz"},
                    {"title", "Structure (XYZ)"},
                    {"description", "Committed structure as XYZ in angstrom."},
                    {"mimeType", "chemical/x-xyz"}},
        QJsonObject{{"uri", uri + "/nwchem"},
                    {"name", "document-nwchem"},
                    {"title", "NWChem input"},
                    {"description", "Validated NWChem input generated from the committed document."},
                    {"mimeType", "text/plain"}}};
    if (editor.additionalResources)
        for (const auto &resource : editor.additionalResources())
            list.append(resource);
    return list;
}
QJsonObject McpServer::callTool(const QString &name, const QJsonObject &args) {
    if (name == "list_documents")
        return {{"documents", QJsonArray{QJsonObject{{"documentId", id},
                                                     {"revision", revision()},
                                                     {"uri", "molforma://documents/" + id},
                                                     {"gui", bool(editor.document)}}}}};
    if (name == "list_templates") {
        QJsonArray entries;
        for (auto v : templates()) {
            auto t = v.toObject();
            entries.append(QJsonObject{{"id", t["id"]},
                                       {"name", t["name_ja"]},
                                       {"nameEn", t["name_en"]},
                                       {"category", t["category"]}});
        }
        return {{"templates", entries}};
    }
    if (args["documentId"] != id)
        fail("Unknown documentId; call list_documents to discover this process's workspace.");
    if (args.contains("expectedRevision") && args["expectedRevision"] != revision())
        fail("Document or candidate changed. Read get_document and review before retrying.");
    if (editor.additionalTools)
        for (const auto &tool : editor.additionalTools())
            if (tool.toObject()["name"] == name)
                return editor.callAdditionalTool(name, args);
    if (name == "get_document") {
        auto out = snapshot();
        if (args.contains("include")) {
            const auto include = args["include"].toArray();
            for (const auto &section : {"project", "candidate"})
                if (!include.contains(QJsonValue(section)))
                    out.remove(section);
            out["hasCandidate"] = candidate().has_value();
        }
        return out;
    }
    const auto d = document();
    if (name == "get_execution") {
        const auto report =
            validateExecution(d, editor.baseDirectory ? editor.baseDirectory() : QDir::currentPath());
        return {{"settings", executionSettings(d)},
                {"defaults", executionDefaults()},
                {"errors", QJsonArray::fromStringList(report.errors)},
                {"warnings", QJsonArray::fromStringList(report.warnings)},
                {"policy", policy.toJson()}};
    }
    if (name == "get_advanced") {
        QJsonArray options;
        for (const auto &o : nwchemAdvancedOptions()) {
            QJsonArray choices;
            for (const auto &c : o.choices)
                choices.append(QJsonObject{{"value", c.value}, {"label", c.label}});
            const QStringList kinds{"real", "integer", "choice", "flag", "path", "temperatures", "grid_axis"};
            options.append(QJsonObject{{"key", o.key},
                                       {"group", o.group},
                                       {"label", o.label},
                                       {"help", o.help},
                                       {"type", kinds[int(o.kind)]},
                                       {"minimum", o.minimum},
                                       {"maximum", o.maximum},
                                       {"choices", choices},
                                       {"default", o.defaultText},
                                       {"applicable", nwchemAdvancedApplies(o, d)}});
        }
        return {{"settings", nwchemAdvanced(d)}, {"options", options}};
    }
    if (name == "measure") {
        auto ids = idsFromJson(args["ids"]);
        if (ids.size() < 2 || ids.size() > 4)
            fail("measure requires 2, 3 or 4 atom IDs.");
        return {{"value", measure(d, ids)}, {"unit", ids.size() == 2 ? "angstrom" : "degree"}};
    }
    if (name == "export_document") {
        auto format = args["format"].toString();
        QString text = format == "xyz"       ? d.xyz()
                       : format == "project" ? QString::fromUtf8(QJsonDocument(d.project()).toJson())
                                             : nwchemInput(d);
        return {{"text", text}, {"format", format}, {"revision", revision()}};
    }
    if (name == "get_nwchem") {
        auto validation = validateNwchem(d);
        QJsonObject choices; // Allowed values of each choice setting, as offered in the GUI.
        for (const auto &key : nwchemDefaults().keys()) {
            QJsonArray values;
            for (const auto &c : nwchemChoices(key))
                values.append(QJsonObject{{"value", c.value}, {"label", c.label}});
            if (!values.isEmpty())
                choices[key] = values;
        }
        return {{"settings", nwchemSettings(d)},
                {"defaults", nwchemDefaults()},
                {"choices", choices},
                {"charge", d.job()["charge"]},
                {"multiplicity", d.job()["multiplicity"]},
                {"errors", QJsonArray::fromStringList(validation.errors)},
                {"warnings", QJsonArray::fromStringList(validation.warnings)}};
    }
    if (name == "save_file") {
        if (candidate())
            fail("Confirm or cancel the candidate before saving.");
        auto path = args["path"].toString();
        if (path.isEmpty())
            fail("path must not be empty.");
        policy.checkPath(path);
        QFileInfo info(path);
        bool overwrite = args["overwrite"].toBool();
        if ((info.exists() || info.isSymLink()) && !overwrite) {
            if (args.contains("overwrite") || !elicitation())
                fail("File exists; explicitly set overwrite to replace it.");
            // Ask the user through the client, bound to this path and document revision.
            const QJsonObject bound{
                {"tool", name}, {"path", info.absoluteFilePath()}, {"revision", revision()}};
            const auto answer = incoming.inputResponses["overwrite"].toObject();
            if (openState() != bound || answer.isEmpty())
                throw InputRequired{
                    {{"overwrite", elicit(info.absoluteFilePath() +
                                              " already exists. Replace it with the current project?",
                                          "overwrite",
                                          {{"type", "boolean"},
                                           {"title", "Replace the existing file"},
                                           {"default", false}})}},
                    sealState(bound)};
            if (answer["action"] != "accept" || !answer["content"].toObject()["overwrite"].toBool())
                fail("File exists and the user did not accept replacing it.");
            overwrite = true;
        }
        if (overwrite)
            d.save(path);
        else {
            // NewOnly makes the no-overwrite promise atomic, including for symbolic links.
            validateDocument(d);
            QFile file(path);
            const auto bytes = QJsonDocument(d.project()).toJson();
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                fail(file.errorString());
            if (file.write(bytes) != bytes.size() || !file.flush()) {
                const auto message = file.errorString();
                file.remove();
                fail(message);
            }
        }
        return {{"path", info.absoluteFilePath()}, {"revision", revision()}};
    }
    if (name == "history") {
        history(args["action"].toString());
        return summary();
    }
    edit(
        "MCP: " + name,
        [&](Document &next) {
            if (name == "place_template")
                placeTemplate(next, findTemplate(args["templateId"].toString()),
                              vectorFromJson(args["position"]));
            else if (name == "add_atom")
                addAtom(next, args["element"].toString(), vectorFromJson(args["position"]),
                        args["anchorId"].toString(), "single", args["hydrogens"].toBool());
            else if (name == "set_positions") {
                QSet<QString> seen;
                for (auto v : args["positions"].toArray()) {
                    auto o = v.toObject();
                    auto atomId = o["id"].toString();
                    if (seen.contains(atomId))
                        fail("Duplicate atom ID in positions.");
                    seen.insert(atomId);
                    auto &a = next.atom(atomId);
                    auto p = vectorFromJson(o["position"]);
                    if (a.fixed && a.position != p)
                        fail("Fixed atoms cannot move.");
                    a.position = p;
                }
            } else if (name == "set_bond") {
                const auto a = args["a"].toString(), b = args["b"].toString();
                next.atom(a);
                next.atom(b);
                if (a == b)
                    fail("A bond requires two different atoms.");
                changeBond(next, a, b, args["kind"].toString());
            } else if (name == "set_dimension") {
                const auto ids = idsFromJson(args["ids"]);
                for (const auto &atomId : ids)
                    next.atom(atomId);
                setDimension(next, ids, args["value"].toDouble());
            } else if (name == "set_atom_properties") {
                const auto ids = idsFromJson(args["ids"]);
                if (ids.isEmpty())
                    fail("Specify atom IDs.");
                for (const auto &atomId : ids)
                    next.atom(atomId);
                if (args.contains("isotope") && args.contains("mass"))
                    fail("Set either isotope or mass.");
                if (args.contains("element"))
                    replaceElement(next, ids, args["element"].toString(), args["hydrogens"].toBool());
                AtomProperties properties;
                if (args.contains("charge"))
                    properties.charge = args["charge"].toInt();
                if (args.contains("radical"))
                    properties.radical = args["radical"].toInt();
                if (args.contains("isotope"))
                    properties.isotope = args["isotope"].toInt();
                if (args.contains("mass"))
                    properties.mass = args["mass"].toDouble();
                if (args.contains("fixed"))
                    properties.fixed = args["fixed"].toBool();
                if (args.contains("ghost"))
                    properties.ghost = args["ghost"].toBool();
                if (args.contains("name"))
                    properties.name = args["name"].toString();
                setAtomProperties(next, ids, properties);
            } else if (name == "delete_atoms" || name == "translate") {
                auto ids = idsFromJson(args["ids"]);
                for (const auto &atomId : ids) {
                    const auto &a = next.atom(atomId);
                    if (name == "translate" && a.fixed)
                        fail("Fixed atoms cannot move.");
                }
                if (name == "delete_atoms")
                    deleteAtoms(next, ids);
                else
                    transform(next, ids, Mat::Identity(), vectorFromJson(args["offset"]));
            } else if (name == "import_document") {
                if (args["append"].toBool() && args["format"] != "xyz")
                    fail("append is available for XYZ text only.");
                if (args["append"].toBool())
                    insertFragment(next, xyzStructure(args["text"].toString()));
                else if (args["format"] == "xyz")
                    replaceCoordinateStructure(next, xyzStructure(args["text"].toString()));
                else
                    next = Document::fromProject(parseJson(args["text"].toString().toUtf8()));
            } else if (name == "open_file") {
                auto path = args["path"].toString();
                policy.checkPath(path);
                QFileInfo info(path);
                if (!info.isFile() || info.size() > 8 * 1024 * 1024)
                    fail("Expected a regular structure file no larger than 8 MiB.");
                // A project replaces everything; a structure file only the structure, as in the GUI's
                // coordinate table, so calculation and execution settings stay. NWChem and Gaussian
                // inputs also set charge and multiplicity, and NWChem inputs the calculation settings
                // (keeping the advanced options and the additional input the user chose).
                const auto suffix = info.suffix().toLower();
                const int frame = args.contains("frame") ? args["frame"].toInt() : -1;
                if (suffix == "json")
                    next = importStructure(path);
                else {
                    const auto imported = importStructure(path, 1, {}, frame);
                    replaceCoordinateStructure(next, imported);
                    if (QStringList{"nw", "nwinp", "gjf", "com", "gau"}.contains(suffix))
                        for (const auto *key : {"charge", "multiplicity"})
                            if (imported.job().contains(key))
                                next.setJobValue(key, imported.job()[key]);
                    if (suffix == "nw" || suffix == "nwinp") {
                        auto settings = imported.data["nwchem"].toObject();
                        const auto current = next.data["nwchem"].toObject();
                        for (const auto *key : {"advanced", "extra_input", "extra_tasks"})
                            if (current.contains(key))
                                settings[key] = current[key];
                        next.data["nwchem"] = settings;
                    }
                }
            } else if (name == "set_advanced") {
                const auto settings = args["settings"].toObject();
                const auto before = validateNwchem(next);
                for (auto it = settings.begin(); it != settings.end(); ++it) {
                    if (std::none_of(nwchemAdvancedOptions().cbegin(), nwchemAdvancedOptions().cend(),
                                     [&](const auto &o) { return o.key == it.key(); }))
                        fail("Unknown advanced setting: " + it.key());
                    if (!it.value().isNull())
                        for (const auto &o : nwchemAdvancedOptions())
                            if (o.key == it.key()) {
                                const auto error = nwchemAdvancedError(o, it.value());
                                if (!error.isEmpty())
                                    fail(error);
                                if (o.kind == AdvancedKind::Path &&
                                    QDir::isAbsolutePath(it.value().toString()))
                                    policy.checkPath(it.value().toString());
                            }
                    setNwchemAdvanced(next, it.key(),
                                      it.value().isNull() ? QJsonValue(QJsonValue::Undefined) : it.value());
                }
                failOnAddedErrors(before, validateNwchem(next));
            } else if (name == "set_execution") {
                const auto settings = args["settings"].toObject();
                const auto current = executionSettings(next);
                const auto base = editor.baseDirectory ? editor.baseDirectory() : QDir::currentPath();
                const auto before = validateExecution(next, base, false);
                for (auto it = settings.begin(); it != settings.end(); ++it) {
                    if (!executionDefaults().contains(it.key()))
                        fail("Unknown execution setting: " + it.key());
                    if (!policy.allowExecutionSettings && isProtectedExecutionKey(it.key()) &&
                        it.value() != current[it.key()])
                        fail(it.key() + " is chosen by the user in MolForma's execution panel and cannot be "
                                        "changed over MCP. Start the server with "
                                        "--mcp-allow-execution-settings to permit it.");
                    if ((it.key() == "job_root" || it.key() == "reference_directory") &&
                        !it.value().toString().isEmpty())
                        policy.checkPath(it.value().toString(), base);
                    setExecutionSetting(next, it.key(), it.value());
                }
                failOnAddedErrors(before, validateExecution(next, base, false));
            } else if (name == "set_nwchem") {
                const auto settings = nwchemSettings(next), defaults = nwchemDefaults();
                const auto before = validateNwchem(next);
                const auto requested = args["settings"].toObject();
                for (auto it = requested.begin(); it != requested.end(); ++it) {
                    if (!defaults.contains(it.key()) || defaults[it.key()].type() != it.value().type())
                        fail("Unknown NWChem setting or incorrect type: " + it.key());
                    if (!policy.allowExecutionSettings && isProtectedNwchemKey(it.key()) &&
                        it.value() != settings[it.key()])
                        fail(it.key() + " is additional NWChem input that MolForma does not check; only the "
                                        "user can change it in the calculation tab. Start the server with "
                                        "--mcp-allow-execution-settings to permit it.");
                    setNwchemSetting(next, it.key(), it.value());
                }
                for (const auto &key : {"charge", "multiplicity"})
                    if (args.contains(key))
                        setNwchemSetting(next, key, args[key]);
                failOnAddedErrors(before, validateNwchem(next));
            }
        },
        args["preview"].toBool(true));
    return summary();
}

QJsonObject McpServer::receive(const QByteArray &line) {
    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return error(QJsonValue(), -32700, "Parse error", {}, legacy != LegacyState::None);
    if (!json.isObject())
        return error(QJsonValue(), -32600, "Expected a single JSON-RPC object; batches are unsupported", {},
                     legacy != LegacyState::None);
    const auto request = json.object();
    const auto requestId = request.value("id");
    const auto method = request["method"].toString();
    bool validId = requestId.isString() || (requestId.isDouble() && std::isfinite(requestId.toDouble()) &&
                                            std::floor(requestId.toDouble()) == requestId.toDouble());
    if (request["jsonrpc"] != "2.0" || method.isEmpty() || request.contains("result") ||
        request.contains("error") || (request.contains("id") && !validId))
        return error(validId ? requestId : QJsonValue(), -32600, "Invalid JSON-RPC request", {},
                     legacy != LegacyState::None);
    const auto params = request["params"].toObject();
    if (!request.contains("id")) {
        if (request.contains("params") && !request["params"].isObject())
            return {};
        if (method == "notifications/initialized" && legacy == LegacyState::Initializing)
            legacy = LegacyState::Ready;
        // Synchronous, bounded operations complete before another message is dispatched.
        // Cancellation of already completed or unknown request IDs is deliberately ignored.
        return {};
    }
    if ((request.contains("params") && !request["params"].isObject()) ||
        (params.contains("_meta") && !params["_meta"].isObject()))
        return error(requestId, -32602, "params and _meta must be objects");
    const auto meta = params["_meta"].toObject();
    const auto version = meta["io.modelcontextprotocol/protocolVersion"];
    const bool modern = meta.contains("io.modelcontextprotocol/protocolVersion");
    if (!modern && (meta.contains("io.modelcontextprotocol/clientCapabilities") ||
                    meta.contains("io.modelcontextprotocol/clientInfo")))
        return error(requestId, -32602, "Missing per-request protocolVersion");
    if (modern) {
        if (!version.isString() || !meta["io.modelcontextprotocol/clientCapabilities"].isObject())
            return error(requestId, -32602,
                         "Required protocolVersion/clientCapabilities metadata is invalid");
        if (version != McpProtocolVersion)
            return error(requestId, -32022, "Unsupported protocol version",
                         {{"supported", versions()}, {"requested", version}});
        if (meta.contains("io.modelcontextprotocol/clientInfo")) {
            const auto info = meta["io.modelcontextprotocol/clientInfo"].toObject();
            if (!info["name"].isString() || !info["version"].isString())
                return error(requestId, -32602, "Invalid clientInfo");
        }
    } else if (method == "initialize") {
        auto info = params["clientInfo"].toObject();
        if (!params["protocolVersion"].isString() || !params["capabilities"].isObject() ||
            !info["name"].isString() || !info["version"].isString())
            return error(requestId, -32602, "Invalid initialize parameters");
        if (legacy != LegacyState::None)
            return error(requestId, -32600, "Already initialized");
        legacy = LegacyState::Initializing;
        return result(requestId,
                      {{"protocolVersion", McpLegacyVersion},
                       {"capabilities", capabilities()},
                       {"serverInfo", serverInfo()},
                       {"instructions", instructions()}},
                      false);
    } else if (method != "ping" && legacy != LegacyState::Ready) {
        return error(requestId, -32602, "Supply required per-request MCP metadata, or initialize legacy MCP");
    }
    if ((params.contains("inputResponses") && !params["inputResponses"].isObject()) ||
        (params.contains("requestState") && !params["requestState"].isString()))
        return error(requestId, -32602, "inputResponses must be an object and requestState a string");
    incoming = {modern, meta["io.modelcontextprotocol/clientCapabilities"].toObject(),
                params["inputResponses"].toObject(), params["requestState"].toString()};
    const bool taskClient = modern && editor.taskState &&
                            incoming.capabilities["extensions"].toObject()[McpTasksExtension].isObject();
    try {
        if (method == "ping") {
            if (modern)
                return error(requestId, -32601, "ping was removed in MCP 2026-07-28");
            return result(requestId, {}, false);
        }
        if (method == "server/discover" && modern)
            return result(requestId,
                          cacheable({{"supportedVersions", versions()},
                                     {"capabilities", capabilities(bool(editor.taskState))},
                                     {"instructions", instructions()}},
                                    true),
                          true);
        if (method.startsWith("tasks/") && modern) {
            if (!taskClient)
                return error(
                    requestId, -32021, "Missing required client capability",
                    {{"requiredCapabilities",
                      QJsonObject{{"extensions", QJsonObject{{McpTasksExtension, QJsonObject{}}}}}}});
            const auto taskId = params["taskId"].toString();
            if (!tasks.contains(taskId))
                return error(requestId, -32602, "Failed to retrieve task: Task not found");
            if (method == "tasks/get") {
                try {
                    return result(requestId, taskResult(taskId), true);
                } catch (const std::exception &e) {
                    return error(requestId, -32602, QString::fromUtf8(e.what()));
                }
            }
            if (method == "tasks/update") {
                if (!params["inputResponses"].isObject())
                    return error(requestId, -32602, "tasks/update requires inputResponses");
                const auto prompt = editor.pendingPrompt ? editor.pendingPrompt() : QJsonObject{};
                const auto key = prompt["key"].toString();
                // Unknown, answered or superseded keys are ignored.
                if (!key.isEmpty() && incoming.inputResponses.contains(key)) {
                    const auto answer = incoming.inputResponses[key].toObject();
                    const auto choice = answer["content"].toObject()["choice"].toString();
                    if (answer["action"] != "accept")
                        editor.answerPrompt(key, {});
                    else if (prompt["choices"].toArray().contains(choice))
                        editor.answerPrompt(key, choice);
                }
                return result(requestId, {}, true);
            }
            if (method == "tasks/cancel") {
                // Cooperative: only work that can still be stopped is marked; finished work keeps its result.
                auto &task = tasks[taskId];
                const auto work = editor.taskState(task.work);
                if (work && ((*work)["state"] == "queued" || (*work)["state"] == "running") &&
                    editor.cancelTask && editor.cancelTask(task.work))
                    task.cancelRequested = true;
                return result(requestId, {}, true);
            }
        }
        if (method == "prompts/list")
            return result(requestId, cacheable({{"prompts", promptList(toolNames())}}, modern), modern);
        if (method == "prompts/get") {
            const auto name = params["name"].toString();
            QJsonObject prompt;
            const auto tools = toolNames();
            for (const auto &p : promptList(tools))
                if (p.toObject()["name"] == name)
                    prompt = p.toObject();
            if (prompt.isEmpty())
                return error(requestId, -32602, "Unknown prompt: " + name);
            if (params.contains("arguments") && !params["arguments"].isObject())
                return error(requestId, -32602, "Prompt arguments must be an object");
            const auto args = params["arguments"].toObject();
            for (auto it = args.begin(); it != args.end(); ++it)
                if (!it.value().isString())
                    return error(requestId, -32602, "Prompt argument values must be strings: " + it.key());
            for (const auto &a : prompt["arguments"].toArray())
                if (a.toObject()["required"].toBool() &&
                    args[a.toObject()["name"].toString()].toString().isEmpty())
                    return error(requestId, -32602,
                                 "Missing prompt argument: " + a.toObject()["name"].toString());
            return result(
                requestId,
                {{"description", prompt["description"]},
                 {"messages",
                  QJsonArray{QJsonObject{
                      {"role", "user"},
                      {"content", QJsonObject{{"type", "text"}, {"text", promptText(name, args, tools)}}}}}}},
                modern);
        }
        if (method == "tools/list" || method == "resources/list" || method == "resources/templates/list") {
            if (params.contains("cursor"))
                return error(requestId, -32602, "This server has one page; omit cursor");
            if (method == "tools/list")
                return result(requestId, cacheable({{"tools", toolList()}}, modern), modern);
            if (method == "resources/templates/list")
                return result(requestId, cacheable({{"resourceTemplates", QJsonArray{}}}, modern), modern);
            return result(requestId, cacheable({{"resources", resourceList()}}, modern), modern);
        }
        if (method == "resources/read") {
            const auto uri = params["uri"].toString(), document = "molforma://documents/" + id;
            std::optional<QJsonObject> content;
            if (uri == document)
                content = QJsonObject{{"uri", uri},
                                      {"mimeType", "application/json"},
                                      {"text", QString::fromUtf8(QJsonDocument(snapshot()).toJson())}};
            else if (uri == document + "/xyz")
                content = QJsonObject{
                    {"uri", uri}, {"mimeType", "chemical/x-xyz"}, {"text", this->document().xyz()}};
            else if (uri == document + "/nwchem") {
                try {
                    content = QJsonObject{
                        {"uri", uri}, {"mimeType", "text/plain"}, {"text", nwchemInput(this->document())}};
                } catch (const std::exception &e) {
                    return error(requestId, -32603,
                                 "NWChem input cannot be generated: " + QString::fromUtf8(e.what()));
                }
            } else if (editor.readResource)
                content = editor.readResource(uri);
            if (!content)
                return error(requestId, modern ? -32602 : -32002, "Unknown resource: " + uri);
            return result(requestId, cacheable({{"contents", QJsonArray{*content}}}, modern), modern);
        }
        if (method == "tools/call") {
            if (!params["name"].isString() ||
                (params.contains("arguments") && !params["arguments"].isObject()))
                return error(requestId, -32602, "Expected tool name and an arguments object");
            const auto name = params["name"].toString();
            QJsonObject schema, output;
            for (const auto &t : toolList())
                if (t.toObject()["name"] == name) {
                    schema = t.toObject()["inputSchema"].toObject();
                    output = t.toObject()["outputSchema"].toObject();
                }
            if (schema.isEmpty())
                return error(requestId, -32602, "Unknown tool: " + name);
            QJsonObject value;
            try {
                const auto args = params["arguments"].toObject();
                validateValue(args, schema, "arguments");
                value = callTool(name, args);
            } catch (const InputRequired &input) {
                QJsonObject interim{{"resultType", "input_required"}, {"inputRequests", input.inputRequests}};
                if (!input.requestState.isEmpty())
                    interim["requestState"] = input.requestState;
                return result(requestId, interim, true);
            } catch (const std::exception &e) {
                return result(requestId,
                              {{"isError", true},
                               {"content", QJsonArray{QJsonObject{{"type", "text"},
                                                                  {"text", QString::fromUtf8(e.what())}}}}},
                              modern);
            }
            // A server bug must never return data that contradicts the published schema.
            if (!output.isEmpty()) {
                auto structured = value;
                structured.remove("_mcpImage");
                validateValue(structured, output, "result");
            }
            if (taskClient)
                if (const auto work = editor.taskWork(name, value); !work.isEmpty())
                    return result(requestId, createTask(name, work), true);
            return result(requestId, toolResult(value), modern);
        }
        return error(requestId, -32601, "Method not found: " + method);
    } catch (const std::exception &) {
        return error(requestId, -32603, "Internal server error");
    }
}
} // namespace mf
