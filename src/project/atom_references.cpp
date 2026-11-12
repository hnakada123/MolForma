// SPDX-License-Identifier: GPL-2.0-or-later
#include "project/atom_references.h"
#include <QJsonArray>
#include <QSet>
#include <algorithm>

namespace mf::project {
namespace {
// How a setting of "nwchem" holds its atom IDs.
enum class Shape {
    Ids,         // A list of IDs that names one coordinate: emptied when one of its atoms goes.
    Constraints, // Objects with the IDs of a coordinate in "atoms": one that loses an atom goes.
    Fragments,   // Objects with a set of IDs in "atoms": they lose their removed atoms; empty ones go.
    Positions    // An object with a member for each ID.
};
struct Reference {
    const char *key;
    Shape shape;
};
// The one list of the settings that refer to atoms: both adapters and nwchemAtomReferenceKeys read it.
constexpr Reference references[] = {
    {"constraints", Shape::Constraints},  // Internal-coordinate constraints.
    {"scan_atoms", Shape::Ids},           // The coordinates of a scan.
    {"scan2_atoms", Shape::Ids},          //
    {"fragment_guess", Shape::Fragments}, // The fragments of an initial guess.
    {"neb_end", Shape::Positions}};       // The end of an NEB path.
QStringList idsFromJson(const QJsonValue &value) {
    QStringList ids;
    for (const auto &id : value.toArray())
        ids << id.toString();
    return ids;
}
QJsonArray idsToJson(const QStringList &ids) {
    return QJsonArray::fromStringList(ids);
}
} // namespace
QStringList nwchemAtomReferenceKeys() {
    QStringList keys;
    for (const auto &reference : references)
        keys << reference.key;
    return keys;
}
void removeAtomReferences(QJsonObject &data, const QStringList &removed) {
    const QSet<QString> gone(removed.begin(), removed.end());
    const auto isGone = [&](const QString &id) { return gone.contains(id); };
    auto nwchem = data.value("nwchem").toObject();
    if (nwchem.isEmpty())
        return;
    const auto refersToGone = [&](const QJsonValue &atoms) {
        const auto list = idsFromJson(atoms);
        return std::any_of(list.begin(), list.end(), isGone);
    };
    for (const auto &[key, shape] : references) {
        const auto value = nwchem.value(key);
        switch (shape) {
        case Shape::Ids:
            if (refersToGone(value))
                nwchem[key] = QJsonArray();
            break;
        case Shape::Constraints:
            if (value.isArray()) {
                QJsonArray kept;
                for (const auto &c : value.toArray())
                    if (!refersToGone(c.toObject()["atoms"]))
                        kept.append(c);
                nwchem[key] = kept;
            }
            break;
        case Shape::Fragments:
            if (value.isArray()) {
                QJsonArray kept;
                for (const auto &item : value.toArray()) {
                    auto fragment = item.toObject();
                    auto atoms = idsFromJson(fragment["atoms"]);
                    atoms.removeIf(isGone);
                    if (atoms.isEmpty())
                        continue;
                    fragment["atoms"] = idsToJson(atoms);
                    kept.append(fragment);
                }
                nwchem[key] = kept;
            }
            break;
        case Shape::Positions:
            if (value.isObject()) {
                auto positions = value.toObject();
                for (const auto &id : removed)
                    positions.remove(id);
                nwchem[key] = positions;
            }
            break;
        }
    }
    data["nwchem"] = nwchem;
}
void renameAtomReferences(QJsonObject &data, const QHash<QString, QString> &renamed) {
    auto nwchem = data.value("nwchem").toObject();
    if (nwchem.isEmpty())
        return;
    const auto renamedIds = [&](const QJsonValue &value) {
        auto ids = idsFromJson(value);
        for (auto &id : ids)
            id = renamed.value(id, id);
        return idsToJson(ids);
    };
    // The objects of a list with their "atoms" renamed.
    const auto renamedItems = [&](const QJsonValue &value) {
        QJsonArray items;
        for (const auto &v : value.toArray()) {
            auto item = v.toObject();
            item["atoms"] = renamedIds(item["atoms"]);
            items.append(item);
        }
        return items;
    };
    for (const auto &[key, shape] : references) {
        const auto value = nwchem.value(key);
        switch (shape) {
        case Shape::Ids:
            if (value.isArray())
                nwchem[key] = renamedIds(value);
            break;
        case Shape::Constraints:
        case Shape::Fragments:
            if (value.isArray())
                nwchem[key] = renamedItems(value);
            break;
        case Shape::Positions:
            if (value.isObject()) {
                const auto end = value.toObject();
                QJsonObject positions;
                for (auto it = end.begin(); it != end.end(); ++it)
                    positions[renamed.value(it.key(), it.key())] = it.value();
                nwchem[key] = positions;
            }
            break;
        }
    }
    data["nwchem"] = nwchem;
}
} // namespace mf::project
