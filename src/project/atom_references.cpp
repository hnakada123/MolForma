// SPDX-License-Identifier: GPL-2.0-or-later
#include "project/atom_references.h"
#include <QJsonArray>
#include <QSet>
#include <algorithm>

namespace mf::project {
namespace {
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
void removeAtomReferences(QJsonObject &data, const QStringList &removed) {
    const QSet<QString> gone(removed.begin(), removed.end());
    const auto isGone = [&](const QString &id) { return gone.contains(id); };
    // NWChem internal-coordinate constraints, the scan coordinates and fragments refer to atoms by ID.
    if (auto nwchem = data.value("nwchem").toObject(); !nwchem.isEmpty()) {
        auto refersToGone = [&](const QJsonValue &atoms) {
            const auto list = idsFromJson(atoms);
            return std::any_of(list.begin(), list.end(), isGone);
        };
        if (nwchem.value("constraints").isArray()) {
            QJsonArray kept;
            for (const auto &c : nwchem.value("constraints").toArray())
                if (!refersToGone(c.toObject()["atoms"]))
                    kept.append(c);
            nwchem["constraints"] = kept;
        }
        for (const auto *key : {"scan_atoms", "scan2_atoms"})
            if (refersToGone(nwchem.value(key)))
                nwchem[key] = QJsonArray();
        // The fragments of an initial guess lose their removed atoms; empty ones go.
        if (nwchem.value("fragment_guess").isArray()) {
            QJsonArray kept;
            for (const auto &value : nwchem.value("fragment_guess").toArray()) {
                auto fragment = value.toObject();
                auto atoms = idsFromJson(fragment["atoms"]);
                atoms.removeIf(isGone);
                if (atoms.isEmpty())
                    continue;
                fragment["atoms"] = idsToJson(atoms);
                kept.append(fragment);
            }
            nwchem["fragment_guess"] = kept;
        }
        // The end of an NEB path, by atom ID.
        if (nwchem.value("neb_end").isObject()) {
            auto end = nwchem.value("neb_end").toObject();
            for (const auto &id : removed)
                end.remove(id);
            nwchem["neb_end"] = end;
        }
        data["nwchem"] = nwchem;
    }
}
void renameAtomReferences(QJsonObject &data, const QHash<QString, QString> &renamed) {
    const auto names = [&](QStringList &ids) {
        for (auto &id : ids)
            id = renamed.value(id, id);
    };
    if (auto nwchem = data.value("nwchem").toObject(); !nwchem.isEmpty()) {
        const auto renamedIds = [&](const QJsonValue &value) {
            auto ids = idsFromJson(value);
            names(ids);
            return idsToJson(ids);
        };
        if (nwchem.value("constraints").isArray()) {
            QJsonArray constraints;
            for (const auto &v : nwchem.value("constraints").toArray()) {
                auto c = v.toObject();
                c["atoms"] = renamedIds(c["atoms"]);
                constraints.append(c);
            }
            nwchem["constraints"] = constraints;
        }
        for (const auto *key : {"scan_atoms", "scan2_atoms"})
            if (nwchem.value(key).isArray())
                nwchem[key] = renamedIds(nwchem[key]);
        if (nwchem.value("fragment_guess").isArray()) {
            QJsonArray fragments;
            for (const auto &v : nwchem.value("fragment_guess").toArray()) {
                auto fragment = v.toObject();
                fragment["atoms"] = renamedIds(fragment["atoms"]);
                fragments.append(fragment);
            }
            nwchem["fragment_guess"] = fragments;
        }
        if (nwchem.value("neb_end").isObject()) {
            const auto end = nwchem.value("neb_end").toObject();
            QJsonObject positions;
            for (auto it = end.begin(); it != end.end(); ++it)
                positions[renamed.value(it.key(), it.key())] = it.value();
            nwchem["neb_end"] = positions;
        }
        data["nwchem"] = nwchem;
    }
}
} // namespace mf::project
