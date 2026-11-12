// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

// Internal to src/mcp: the prompt catalogue (prompts/list, prompts/get).
namespace mf::mcp_detail {
// The prompts a server with these tools offers.
QJsonArray promptList(const QSet<QString> &tools);
// The text of a prompt of that list with its arguments.
QString promptText(const QString &name, const QJsonObject &args, const QSet<QString> &tools);
} // namespace mf::mcp_detail
