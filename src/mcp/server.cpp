// SPDX-License-Identifier: GPL-2.0-or-later
#include "mcp/server.h"
#include "mcp/json_schema.h"
#include "mcp/prompts.h"
#include "mcp/server_detail.h"
#include "nwchem/nwchem.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <cmath>

namespace mf {
namespace mcp_detail {
QJsonObject elicit(const QString &message, const QString &field, const QJsonObject &schema) {
    return {
        {"method", "elicitation/create"},
        {"params", QJsonObject{{"mode", "form"},
                               {"message", message},
                               {"requestedSchema", QJsonObject{{"type", "object"},
                                                               {"properties", QJsonObject{{field, schema}}},
                                                               {"required", QJsonArray{field}}}}}}};
}
} // namespace mcp_detail
using namespace mcp_detail;
namespace {
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
} // namespace

McpServer::McpServer(McpEditor hooks, McpPolicy rules, QObject *parent)
    : QObject(parent), editor(std::move(hooks)), policy(std::move(rules)) {
    if (editor.document && (!editor.candidate || !editor.edit || !editor.history))
        fail("Incomplete MCP editor hooks");
    if (bool(editor.additionalTools) != bool(editor.callAdditionalTool))
        fail("Incomplete MCP additional tool hooks");
    if (bool(editor.taskState) != bool(editor.taskWork))
        fail("Incomplete MCP task hooks");
    registerTools();
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
    tasks[taskId] = {work, "working", now, now};
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
QJsonArray McpServer::toolList() const {
    QJsonArray list;
    for (const auto &tool : builtIn)
        list.append(tool.spec.definition());
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
    const auto tool =
        std::find_if(builtIn.cbegin(), builtIn.cend(), [&](const Tool &t) { return t.spec.name == name; });
    if (tool != builtIn.cend() && tool->spec.global)
        return tool->call(args);
    if (args["documentId"] != id)
        fail("Unknown documentId; call list_documents to discover this process's workspace.");
    if (args.contains("expectedRevision") && args["expectedRevision"] != revision())
        fail("Document or candidate changed. Read get_document and review before retrying.");
    if (editor.additionalTools)
        for (const auto &additional : editor.additionalTools())
            if (additional.toObject()["name"] == name)
                return editor.callAdditionalTool(name, args);
    if (tool == builtIn.cend())
        fail("Unknown tool: " + name);
    return tool->call(args);
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
