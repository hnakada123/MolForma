#include "mcp/server.h"
#include "editing/chemistry.h"
#include "editing/coordinates.h"
#include "editing/operations.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <cmath>

namespace mf {
namespace {
constexpr int MaxAtoms = 10000;
constexpr int SummaryIds = 100;
QJsonObject serverInfo() {
    return {{"name", "molforma"}, {"title", "MolForma"}, {"version", "0.1.0"}};
}
QJsonArray versions() {
    return {McpProtocolVersion, McpLegacyVersion};
}
QJsonObject capabilities() {
    return {{"tools", QJsonObject{}}, {"resources", QJsonObject{}}};
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
           "get_ui_command and inspect get_ui for modal dialogs; answer them with explicit calls. "
           "A completed GUI command is not proof that a background calculation converged.";
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
    bool valid = matches(t.toString());
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

McpServer::McpServer(McpEditor hooks, QObject *parent) : QObject(parent), editor(std::move(hooks)) {
    if (editor.document && (!editor.candidate || !editor.edit || !editor.history))
        fail("Incomplete MCP editor hooks");
    if (bool(editor.additionalTools) != bool(editor.callAdditionalTool))
        fail("Incomplete MCP additional tool hooks");
}
QString McpServer::documentId() const {
    return id;
}
Document McpServer::document() const {
    return editor.document ? editor.document() : memory;
}
std::optional<Document> McpServer::candidate() const {
    return editor.candidate ? editor.candidate() : pending;
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
void McpServer::commit(const Document &next) {
    if (QJsonDocument(memory.project()) == QJsonDocument(next.project()))
        return;
    undo.resize(undoIndex + 1);
    undo.append(next);
    if (undo.size() > 101)
        undo.removeFirst();
    undoIndex = undo.size() - 1;
    memory = next;
}
void McpServer::edit(const QString &name, const std::function<void(Document &)> &operation, bool preview) {
    auto checked = [&](Document &d) {
        const auto before = d;
        operation(d);
        for (const auto &atom : before.atoms)
            if (atom.fixed && d.contains(atom.id) && d.atom(atom.id).position != atom.position)
                fail("Fixed atoms cannot move.");
        validateDocument(d);
    };
    if (editor.edit) {
        editor.edit(name, checked, preview);
        return;
    }
    if (pending)
        fail("Confirm or cancel the existing candidate before editing.");
    Document next = memory;
    checked(next);
    if (QJsonDocument(next.project()) == QJsonDocument(memory.project()))
        return;
    if (preview)
        pending = next;
    else
        commit(next);
}
void McpServer::history(const QString &action) {
    if (editor.history) {
        editor.history(action);
        return;
    }
    if (action == "confirm") {
        if (!pending)
            fail("No candidate to confirm.");
        commit(*pending);
        pending.reset();
    } else if (action == "cancel") {
        if (!pending)
            fail("No candidate to cancel.");
        pending.reset();
    } else {
        if (pending)
            fail("Confirm or cancel the existing candidate before undo/redo.");
        if (action == "undo" && undoIndex > 0)
            memory = undo[--undoIndex];
        else if (action == "redo" && undoIndex + 1 < undo.size())
            memory = undo[++undoIndex];
        else
            fail("No history entry for " + action);
    }
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
        list.append(
            QJsonObject{{"name", name},
                        {"description", description},
                        {"inputSchema", QJsonObject{{"type", "object"},
                                                    {"properties", properties},
                                                    {"required", required},
                                                    {"additionalProperties", false}}},
                        {"annotations", QJsonObject{{"readOnlyHint", !mutation && name != "save_file"},
                                                    {"destructiveHint", mutation || name == "save_file"},
                                                    {"openWorldHint", false}}}});
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
    add("import_document",
        "Replace structure from XYZ text (preserving project metadata) or a complete "
        "KAGAMI v2 project JSON string. This is undoable.",
        {{"format", choice({"xyz", "project"})}, {"text", type("string")}}, {"format", "text"}, true, true);
    add("open_file",
        "Load a local XYZ/MOL/SDF/PDB/KAGAMI file as an undoable edit. PDB uses model 1 "
        "and automatic alternate-location selection. Maximum file size: 8 MiB.",
        {{"path", type("string")}}, {"path"}, true, true);
    add("export_document", "Return the committed document as XYZ, KAGAMI v2 JSON, or validated NWChem input.",
        {{"format", choice({"xyz", "project", "nwchem"})}}, {"format"});
    add("save_file",
        "Save the committed KAGAMI v2 project locally. Refuse existing paths unless "
        "overwrite is true; reject pending candidates. Does not mark GUI history clean.",
        {{"path", type("string")}, {"overwrite", type("boolean")}, {"expectedRevision", type("string")}},
        {"path", "expectedRevision"});
    add("get_nwchem", "Read NWChem settings, defaults and validation errors/warnings.");
    add("get_advanced", "Read all advanced/output options, their types, applicability and stored values.");
    add("set_advanced", "Merge typed advanced/output settings. A null value resets that option.",
        {{"settings", type("object")}}, {"settings"}, true, true);
    add("get_execution", "Read launch settings and local validation. Never starts a process.");
    add("set_execution", "Merge launch settings (including explicit environment overrides). Does not run.",
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
                {"warnings", QJsonArray::fromStringList(report.warnings)}};
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
        return {{"settings", nwchemSettings(d)},
                {"defaults", nwchemDefaults()},
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
        QFileInfo info(path);
        if ((info.exists() || info.isSymLink()) && !args["overwrite"].toBool())
            fail("File exists; explicitly set overwrite to replace it.");
        if (args["overwrite"].toBool())
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
                next.setBond(a, b, args["kind"].toString());
                next.invalidateAssignments();
            } else if (name == "delete_atoms" || name == "translate") {
                auto ids = idsFromJson(args["ids"]);
                for (const auto &atomId : ids) {
                    const auto &a = next.atom(atomId);
                    if (name == "translate" && a.fixed)
                        fail("Fixed atoms cannot move.");
                }
                if (name == "delete_atoms")
                    next.remove(ids);
                else
                    transform(next, ids, Mat::Identity(), vectorFromJson(args["offset"]));
            } else if (name == "import_document") {
                if (args["format"] == "xyz")
                    replaceCoordinateStructure(next, Document::fromXyz(args["text"].toString()));
                else
                    next = Document::fromProject(parseJson(args["text"].toString().toUtf8()));
            } else if (name == "open_file") {
                auto path = args["path"].toString();
                QFileInfo info(path);
                if (!info.isFile() || info.size() > 8 * 1024 * 1024)
                    fail("Expected a regular structure file no larger than 8 MiB.");
                next = importStructure(path);
            } else if (name == "set_advanced") {
                const auto settings = args["settings"].toObject();
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
                            }
                    setNwchemAdvanced(next, it.key(),
                                      it.value().isNull() ? QJsonValue(QJsonValue::Undefined) : it.value());
                }
                const auto report = validateNwchem(next);
                if (!report.errors.isEmpty())
                    fail(report.errors.join('\n'));
            } else if (name == "set_execution") {
                const auto settings = args["settings"].toObject();
                for (auto it = settings.begin(); it != settings.end(); ++it) {
                    if (!executionDefaults().contains(it.key()))
                        fail("Unknown execution setting: " + it.key());
                    setExecutionSetting(next, it.key(), it.value());
                }
                const auto report = validateExecution(
                    next, editor.baseDirectory ? editor.baseDirectory() : QDir::currentPath(), false);
                if (!report.errors.isEmpty())
                    fail(report.errors.join('\n'));
            } else if (name == "set_nwchem") {
                auto settings = nwchemSettings(next), defaults = nwchemDefaults();
                const auto requested = args["settings"].toObject();
                for (auto it = requested.begin(); it != requested.end(); ++it) {
                    if (!defaults.contains(it.key()) || defaults[it.key()].type() != it.value().type())
                        fail("Unknown NWChem setting or incorrect type: " + it.key());
                    settings[it.key()] = it.value();
                }
                next.data["nwchem"] = settings;
                for (const auto &key : {"charge", "multiplicity"})
                    if (args.contains(key))
                        next.setJobValue(key, args[key]);
                const auto validation = validateNwchem(next);
                if (!validation.errors.isEmpty())
                    fail(validation.errors.join('\n'));
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
    try {
        if (method == "ping")
            return result(requestId, {}, modern);
        if (method == "server/discover" && modern)
            return result(requestId,
                          cacheable({{"supportedVersions", versions()},
                                     {"capabilities", capabilities()},
                                     {"instructions", instructions()}},
                                    true),
                          true);
        if (method == "tools/list" || method == "resources/list" || method == "resources/templates/list") {
            if (params.contains("cursor"))
                return error(requestId, -32602, "This server has one page; omit cursor");
            if (method == "tools/list")
                return result(requestId, cacheable({{"tools", toolList()}}, modern), modern);
            if (method == "resources/templates/list")
                return result(requestId, cacheable({{"resourceTemplates", QJsonArray{}}}, modern), modern);
            return result(
                requestId,
                cacheable({{"resources", QJsonArray{QJsonObject{{"uri", "molforma://documents/" + id},
                                                                {"name", "MolForma document"},
                                                                {"mimeType", "application/json"}}}}},
                          modern),
                modern);
        }
        if (method == "resources/read") {
            if (params["uri"] != "molforma://documents/" + id)
                return error(requestId, modern ? -32602 : -32002, "Unknown document resource");
            return result(requestId,
                          cacheable({{"contents",
                                      QJsonArray{QJsonObject{
                                          {"uri", params["uri"]},
                                          {"mimeType", "application/json"},
                                          {"text", QString::fromUtf8(QJsonDocument(snapshot()).toJson())}}}}},
                                    modern),
                          modern);
        }
        if (method == "tools/call") {
            if (!params["name"].isString() ||
                (params.contains("arguments") && !params["arguments"].isObject()))
                return error(requestId, -32602, "Expected tool name and an arguments object");
            const auto name = params["name"].toString();
            QJsonObject schema;
            for (const auto &t : toolList())
                if (t.toObject()["name"] == name)
                    schema = t.toObject()["inputSchema"].toObject();
            if (schema.isEmpty())
                return error(requestId, -32602, "Unknown tool: " + name);
            try {
                const auto args = params["arguments"].toObject();
                validateValue(args, schema, "arguments");
                return result(requestId, toolResult(callTool(name, args)), modern);
            } catch (const std::exception &e) {
                return result(requestId,
                              {{"isError", true},
                               {"content", QJsonArray{QJsonObject{{"type", "text"},
                                                                  {"text", QString::fromUtf8(e.what())}}}}},
                              modern);
            }
        }
        return error(requestId, -32601, "Method not found: " + method);
    } catch (const std::exception &) {
        return error(requestId, -32603, "Internal server error");
    }
}
} // namespace mf
