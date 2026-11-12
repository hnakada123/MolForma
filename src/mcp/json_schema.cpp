// SPDX-License-Identifier: GPL-2.0-or-later
#include "mcp/json_schema.h"
#include "foundation/error.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <cmath>

namespace mf::mcp_detail {
QJsonObject type(const QString &name, const QString &description) {
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
} // namespace mf::mcp_detail
