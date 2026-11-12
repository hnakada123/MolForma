// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

// Internal to src/mcp: the JSON Schemas (2020-12) that the server publishes for the arguments and
// results of its tools, and the check of a value against one of them.
namespace mf::mcp_detail {
// The parts the schemas of the tools are made of.
QJsonObject type(const QString &name, const QString &description = {});
QJsonObject choice(const QStringList &values);
QJsonObject vectorSchema(); // Three numbers.
QJsonObject idsSchema();    // One or more strings, each once.
QJsonObject object(const QJsonObject &properties, const QStringList &required);
QJsonObject strings();
// Fails with the path of the first value that does not fit (types, ranges, enum, required and unknown
// properties, array lengths and unique items). Only schemas of this server: no client-supplied schema
// and no $ref is evaluated.
void validateValue(const QJsonValue &value, const QJsonObject &schema, const QString &path);
} // namespace mf::mcp_detail
