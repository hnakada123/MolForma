// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/elements.h"
#include "foundation/error.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>

static void initResources() {
    Q_INIT_RESOURCE(resources);
}
namespace mf {
namespace {
// "#rrggbb", as elements.json spells its colors.
Rgb rgbFromText(const QString &text) {
    static const QRegularExpression pattern("^#[0-9A-Fa-f]{6}$");
    if (!pattern.match(text).hasMatch())
        fail("Invalid element color: " + text);
    const auto value = text.mid(1).toUInt(nullptr, 16);
    return {quint8(value >> 16), quint8(value >> 8), quint8(value)};
}
} // namespace
const QVector<Element> &elements() {
    static QVector<Element> values = [] {
        initResources();
        QVector<Element> result;
        for (auto v : parseJson(readFile(":/data/elements.json"))["elements"].toArray()) {
            auto o = v.toObject();
            result.append({o["symbol"].toString(), o["z"].toInt(), o["mass"].toDouble(),
                           o["covalent_radius"].toDouble(), o["vdw_radius"].toDouble(),
                           rgbFromText(o["color"].toString()), o["name_ja"].toString(),
                           o["name_en"].toString()});
        }
        return result;
    }();
    return values;
}
QString Element::name() const {
    return english() ? nameEn : nameJa;
}
bool Element::metal() const {
    // The nonmetals and metalloids, by atomic number.
    static constexpr int others[] = {1,  2,  5,  6,  7,  8,  9,  10, 14, 15, 16, 17, 18,
                                     32, 33, 34, 35, 36, 51, 52, 53, 54, 84, 85, 86};
    return std::find(std::begin(others), std::end(others), number) == std::end(others);
}
const Element &element(const QString &s) {
    static const auto index = [] {
        QHash<QString, int> symbols;
        for (int i = 0; i < elements().size(); ++i)
            if (!symbols.contains(elements()[i].symbol))
                symbols.insert(elements()[i].symbol, i);
        return symbols;
    }();
    if (const int i = index.value(s, -1); i >= 0)
        return elements()[i];
    fail(tr("未知の元素: %1").arg(s));
}
} // namespace mf
