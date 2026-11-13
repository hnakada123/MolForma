// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "mcp/server.h"
#include "test_support.h"
#include <QJsonDocument>

// Messages to an MCP server and what its replies hold, for the tests of the server without a window
// (test_mcp_server) and of MCP in the application (test_mcp).

inline QJsonObject metadata(const QString &version = mf::McpProtocolVersion) {
    return {{"io.modelcontextprotocol/protocolVersion", version},
            {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}};
}
inline QByteArray message(const QString &method, QJsonObject params = {}, QJsonValue id = 1,
                          bool modern = true) {
    if (modern)
        params["_meta"] = metadata();
    QJsonObject request{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (!id.isUndefined())
        request["id"] = id;
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}
// A modern request from a client declaring elicitation and/or the Tasks extension.
inline QJsonObject clientCapabilities(bool elicitation, bool tasks) {
    QJsonObject c;
    if (elicitation)
        c["elicitation"] = QJsonObject{{"form", QJsonObject{}}};
    if (tasks)
        c["extensions"] = QJsonObject{{mf::McpTasksExtension, QJsonObject{}}};
    return c;
}
inline QByteArray capable(const QString &method, QJsonObject params, const QJsonObject &capabilities,
                          QJsonValue id = 1) {
    auto meta = metadata();
    meta["io.modelcontextprotocol/clientCapabilities"] = capabilities;
    params["_meta"] = meta;
    return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}})
        .toJson(QJsonDocument::Compact);
}
inline QJsonObject call(mf::McpServer &server, const QString &name, QJsonObject args = {}) {
    return server.receive(message("tools/call", {{"name", name}, {"arguments", args}}));
}
inline QJsonObject data(const QJsonObject &response) {
    return response["result"].toObject()["structuredContent"].toObject();
}
inline bool failed(const QJsonObject &response) {
    return response.contains("error") || response["result"].toObject()["isError"].toBool();
}
inline QString text(const QJsonObject &response) {
    return response["result"].toObject()["content"].toArray()[0].toObject()["text"].toString();
}
inline QJsonObject toolNamed(mf::McpServer &server, const QString &name) {
    for (const auto &tool : server.receive(message("tools/list"))["result"].toObject()["tools"].toArray())
        if (tool.toObject()["name"] == name)
            return tool.toObject();
    return {};
}
// The arguments of a call on the server's document, as a change of its current revision or not.
inline QJsonObject scoped(mf::McpServer &server, QJsonObject args = {}, bool mutation = false) {
    args["documentId"] = server.documentId();
    if (mutation)
        args["expectedRevision"] = server.snapshot()["revision"];
    return args;
}
inline QJsonObject change(mf::McpServer &server, const QString &tool, QJsonObject args = {}) {
    return call(server, tool, scoped(server, args, true));
}
inline mf::Document committed(mf::McpServer &server) {
    return mf::Document::fromProject(server.snapshot()["project"].toObject());
}
