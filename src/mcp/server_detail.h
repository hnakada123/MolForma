// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QJsonObject>
#include <QString>

// Internal to src/mcp: what the files of McpServer share (server.cpp: protocol, tasks, resources and
// dispatch; document_adapter.cpp: the document behind the tools; document_tools.cpp: the tools).
namespace mf::mcp_detail {
// MCP's input size limit: no edit over MCP makes a document of more atoms (McpServer::edit).
inline constexpr int MaxAtoms = 10000;
// Thrown by a tool that needs client input first (multi round-trip request).
struct InputRequired {
    QJsonObject inputRequests;
    QString requestState;
};
// An elicitation request for one field of that schema (server.cpp).
QJsonObject elicit(const QString &message, const QString &field, const QJsonObject &schema);
// Fails for unknown elements and for what Document::validate finds (document_adapter.cpp).
void validateDocument(const Document &d);
} // namespace mf::mcp_detail
