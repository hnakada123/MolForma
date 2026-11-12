// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/document.h"
#include "foundation/i18n.h"
#include "model/memo.h"
#include "model/topology.h"
#include "project/atom_references.h"
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <charconv>
#include <cmath>

namespace mf {
QString newId() {
    return QUuid::createUuid().toString(QUuid::Id128);
}
Vec vectorFromJson(const QJsonValue &v) {
    const auto a = v.toArray();
    if (a.size() != 3 || !a[0].isDouble() || !a[1].isDouble() || !a[2].isDouble())
        fail(tr("座標は3個の数値で指定してください。"));
    Vec p(a[0].toDouble(), a[1].toDouble(), a[2].toDouble());
    if (!p.allFinite())
        fail(tr("座標に有限でない値があります。"));
    return p;
}
QJsonArray vectorToJson(const Vec &p) {
    return {p.x(), p.y(), p.z()};
}
Ids idsFromJson(const QJsonValue &v) {
    Ids ids;
    for (const auto &x : v.toArray())
        ids << x.toString();
    return ids;
}
QJsonArray idsToJson(const Ids &ids) {
    return QJsonArray::fromStringList(ids);
}
QJsonObject Atom::toJson() const {
    auto o = extra;
    o["id"] = id;
    o["element"] = element;
    o["position"] = vectorToJson(position);
    o["formal_charge"] = charge;
    o["radical"] = radical;
    o["isotope_mass_number"] = isotope ? QJsonValue(isotope) : QJsonValue();
    o["mass_amu"] = mass > 0 ? QJsonValue(mass) : QJsonValue();
    o["ghost"] = ghost;
    o["fixed"] = fixed;
    o["auto_hydrogen"] = automaticH;
    o["name"] = name;
    o["stereo"] = stereo;
    if (!o.contains("metadata"))
        o["metadata"] = QJsonObject();
    return o;
}
Atom Atom::fromJson(const QJsonObject &o) {
    Atom a;
    a.extra = o;
    a.id = o["id"].toString(a.id);
    a.element = o["element"].toString();
    a.position = vectorFromJson(o["position"]);
    a.name = o["name"].toString();
    a.charge = o["formal_charge"].toInt();
    a.radical = o["radical"].toInt();
    a.isotope = o["isotope_mass_number"].toInt();
    a.mass = o["mass_amu"].toDouble();
    a.ghost = o["ghost"].toBool();
    a.fixed = o["fixed"].toBool();
    a.automaticH = o["auto_hydrogen"].toBool();
    a.stereo = o["stereo"].toString();
    mf::element(a.element);
    return a;
}
// The fields of an object that MolForma does not read, kept on round trips.
static QJsonObject unknownFields(QJsonObject o, std::initializer_list<const char *> known) {
    for (const auto *key : known)
        o.remove(key);
    return o;
}
QJsonObject Residue::toJson() const {
    auto o = extra;
    QJsonObject names;
    for (auto it = atoms.begin(); it != atoms.end(); ++it)
        names[it.key()] = it.value();
    o["id"] = id;
    o["code"] = code;
    o["atoms"] = names;
    o["chain"] = chain;
    o["number"] = number;
    o["insertion_code"] = insertionCode;
    o["chirality"] = chirality;
    o["state"] = state;
    o["n_terminal"] = nTerminal;
    o["c_terminal"] = cTerminal;
    o["matched"] = matched;
    o["modified"] = modified;
    o["metadata"] = metadata;
    return o;
}
Residue Residue::fromJson(const QJsonObject &o) {
    Residue r;
    r.id = o["id"].toString();
    r.code = o["code"].toString();
    const auto names = o["atoms"].toObject();
    for (auto it = names.begin(); it != names.end(); ++it)
        r.atoms[it.key()] = it.value().toString();
    r.chain = o["chain"].toString();
    r.number = o["number"].toInt();
    r.insertionCode = o["insertion_code"].toString();
    r.chirality = o["chirality"].toString(r.chirality);
    r.state = o["state"].toString(r.state);
    r.nTerminal = o["n_terminal"].toString(r.nTerminal);
    r.cTerminal = o["c_terminal"].toString(r.cTerminal);
    r.matched = o["matched"].toBool(r.matched);
    r.modified = o["modified"].toBool();
    r.metadata = o["metadata"].toObject();
    r.extra = unknownFields(o, {"id", "code", "atoms", "chain", "number", "insertion_code", "chirality",
                                "state", "n_terminal", "c_terminal", "matched", "modified", "metadata"});
    return r;
}
QJsonObject Part::toJson() const {
    auto o = extra;
    o["id"] = id;
    o["template"] = templateId;
    o["atoms"] = idsToJson(atoms);
    o["connections"] = idsToJson(connections);
    o["metadata"] = metadata;
    return o;
}
Part Part::fromJson(const QJsonObject &o) {
    Part p;
    p.id = o["id"].toString();
    p.templateId = o["template"].toString();
    p.atoms = idsFromJson(o["atoms"]);
    p.connections = idsFromJson(o["connections"]);
    p.metadata = o["metadata"].toObject();
    p.extra = unknownFields(o, {"id", "template", "atoms", "connections", "metadata"});
    return p;
}
QVector<Ids> Part::hapto() const {
    QVector<Ids> groups;
    for (const auto &group : metadata["hapto"].toArray())
        groups << idsFromJson(group);
    return groups;
}
void Part::setHapto(const QVector<Ids> &groups) {
    QJsonArray rows;
    for (const auto &group : groups)
        rows.append(idsToJson(group));
    if (rows.isEmpty())
        metadata.remove("hapto");
    else
        metadata["hapto"] = rows;
}
QJsonObject Center::toJson() const {
    auto o = extra;
    QJsonArray rows;
    for (const auto &site : sites) {
        auto s = site.extra;
        s["direction"] = vectorToJson(site.direction);
        s["distance"] = site.distance;
        if (!site.donor.isEmpty()) {
            s["donor"] = site.donor;
            s["part"] = site.part;
        }
        rows.append(s);
    }
    o["atom"] = atom;
    o["geometry"] = geometry;
    o["oxidation"] = oxidation;
    o["sites"] = rows;
    return o;
}
Center Center::fromJson(const QJsonObject &o) {
    Center c;
    c.atom = o["atom"].toString();
    c.geometry = o["geometry"].toString();
    c.oxidation = o["oxidation"].toInt();
    for (const auto &v : o["sites"].toArray()) {
        const auto s = v.toObject();
        CoordinationSite site;
        try {
            site.direction = vectorFromJson(s["direction"]);
        } catch (const std::exception &) {
            fail(tr("配位サイトの座標が不正です。"));
        }
        site.distance = s["distance"].toDouble();
        site.donor = s["donor"].toString();
        site.part = s["part"].toString();
        site.extra = unknownFields(s, {"direction", "distance", "donor", "part"});
        c.sites << site;
    }
    c.extra = unknownFields(o, {"atom", "geometry", "oxidation", "sites"});
    return c;
}
QJsonObject PointCharge::toJson() const {
    auto o = extra;
    o["id"] = id;
    o["position"] = vectorToJson(position);
    o["charge"] = charge;
    o["source"] = source;
    o["source_unit"] = sourceUnit;
    return o;
}
PointCharge PointCharge::fromJson(const QJsonObject &o) {
    PointCharge q;
    q.id = o["id"].toString();
    try {
        q.position = vectorFromJson(o["position"]);
    } catch (const std::exception &) {
        fail(tr("点電荷の座標が不正です。"));
    }
    if (!o["charge"].isDouble())
        fail(tr("点電荷が不正です。"));
    q.charge = o["charge"].toDouble();
    q.source = o["source"].toString();
    q.sourceUnit = o["source_unit"].toString();
    q.extra = unknownFields(o, {"id", "position", "charge", "source", "source_unit"});
    return q;
}
double Bond::order() const {
    if (kind == "dative")
        return 0;
    if (kind == "aromatic")
        return 1.5;
    return kind == "triple" ? 3 : kind == "double" ? 2 : 1;
}
QJsonObject Bond::toJson() const {
    return {{"a", a}, {"b", b}, {"kind", kind}, {"source", source}, {"stereo", stereo}};
}
Document::Document() {
    data = {{"id", newId()}, {"units", "angstrom"}, {"metadata", QJsonObject()}};
}
int Document::index(const QString &id) const {
    for (int i = 0; i < atoms.size(); ++i)
        if (atoms[i].id == id)
            return i;
    return -1;
}
QVector<int> Document::indices(const Ids &ids) const {
    QHash<QString, int> found;
    found.reserve(ids.size());
    for (const auto &id : ids)
        found.insert(id, -1);
    qsizetype missing = found.size();
    for (int i = 0; i < atoms.size() && missing; ++i)
        if (auto it = found.find(atoms[i].id); it != found.end() && *it < 0) {
            *it = i;
            --missing;
        }
    QVector<int> out;
    out.reserve(ids.size());
    for (const auto &id : ids) {
        out << found.value(id);
        if (out.last() < 0)
            atom(id); // Fails.
    }
    return out;
}
bool Document::contains(const QString &id) const {
    return index(id) >= 0;
}
Atom &Document::atom(const QString &id) {
    int i = index(id);
    if (i < 0)
        fail(tr("原子が見つかりません: %1").arg(id));
    return atoms[i];
}
const Atom &Document::atom(const QString &id) const {
    int i = index(id);
    if (i < 0)
        fail(tr("原子が見つかりません: %1").arg(id));
    return atoms[i];
}
Ids Document::ids() const {
    Ids out;
    for (const auto &a : atoms)
        out << a.id;
    return out;
}
Ids Document::neighbors(const QString &id, bool dative) const {
    Ids out;
    for (const auto &b : bonds)
        if (dative || b.kind != "dative") {
            if (b.a == id)
                out << b.b;
            else if (b.b == id)
                out << b.a;
        }
    return out;
}
const Bond *Document::bond(const QString &a, const QString &b) const {
    for (const auto &x : bonds)
        if ((x.a == a && x.b == b) || (x.a == b && x.b == a))
            return &x;
    return nullptr;
}
void Document::setBond(const QString &a, const QString &b, const QString &kind) {
    if (!contains(a) || !contains(b) || a == b)
        fail(tr("異なる2原子を指定してください。"));
    const auto same = [&](const Bond &x) { return (x.a == a && x.b == b) || (x.a == b && x.b == a); };
    if (kind == "none") {
        bonds.erase(std::remove_if(bonds.begin(), bonds.end(), same), bonds.end());
        return;
    }
    // An existing bond keeps its place, so setting the kind it already has changes nothing.
    if (auto it = std::find_if(bonds.begin(), bonds.end(), same); it != bonds.end()) {
        if (it->kind != kind) {
            it->kind = kind;
            it->stereo.clear();
            it->source = "explicit";
        }
        return;
    }
    bonds.append({a, b, kind, {}, "explicit"});
}
const Residue *Document::findResidue(const QString &id) const {
    for (const auto &r : residues)
        if (r.id == id)
            return &r;
    return nullptr;
}
const Center *Document::findCenter(const QString &atom) const {
    for (const auto &c : centers)
        if (c.atom == atom)
            return &c;
    return nullptr;
}
Ids Document::component(const QString &root, const QString &ca, const QString &cb) const {
    const Topology topology(*this);
    const int i = topology.index(root);
    if (i < 0)
        fail(tr("原子を選択してください。"));
    Ids out;
    for (int j : topology.component(i, topology.index(ca), topology.index(cb)))
        out << atoms[j].id;
    return out;
}
Ids Document::bondSide(const QString &a, const QString &b) const {
    if (!bond(a, b))
        fail(tr("結合の両端を選択してください。"));
    auto side = component(b, a, b);
    if (side.contains(a))
        fail(tr("環内の結合は片側だけ動かせません。全体を回転・移動するか、整形を使用してください。"));
    return side;
}
void Document::remove(const Ids &removed) {
    QSet<QString> gone(removed.begin(), removed.end());
    for (const auto &a : atoms)
        if (gone.contains(a.id) && a.fixed)
            fail(tr("固定原子を削除できません。"));
    const auto isGone = [&](const QString &id) { return gone.contains(id); };
    atoms.removeIf([&](const Atom &a) { return isGone(a.id); });
    bonds.removeIf([&](const Bond &b) { return isGone(b.a) || isGone(b.b); });
    // What a ligand that lost atoms still holds at its center: its sites in the order of its metadata (one
    // per donor, then one per eta group) and the atom each site names (of an eta group the first).
    struct Ligand {
        QString part, center;
        QJsonArray sites;
        Ids donors;
    };
    QVector<Ligand> ligands;
    for (auto &p : parts) {
        if (std::none_of(p.atoms.begin(), p.atoms.end(), isGone))
            continue;
        const auto groups = p.hapto();
        const auto sites = p.metadata.value("sites").toArray();
        const bool placed = !p.center().isEmpty() && sites.size() == p.connections.size() + groups.size();
        Ids donors;
        QVector<Ids> eta;
        QJsonArray donorSites, etaSites;
        for (qsizetype k = 0; k < p.connections.size(); ++k)
            if (!isGone(p.connections[k])) {
                donors << p.connections[k];
                donorSites.append(sites.at(k));
            }
        for (qsizetype g = 0; g < groups.size(); ++g) {
            auto members = groups[g];
            members.removeIf(isGone);
            const auto site = sites.at(p.connections.size() + g);
            // An eta group has two atoms or more: one that is left alone binds as a donor of its own.
            if (members.size() == 1) {
                donors << members.first();
                donorSites.append(site);
            } else if (members.size() > 1) {
                eta << members;
                etaSites.append(site);
            }
        }
        p.atoms.removeIf(isGone);
        p.connections = donors;
        p.setHapto(eta);
        if (!placed)
            continue;
        Ligand ligand{p.id, p.center(), donorSites, donors};
        for (qsizetype g = 0; g < eta.size(); ++g) {
            ligand.sites.append(etaSites.at(g));
            ligand.donors << eta[g].first();
        }
        // A ligand that binds with nothing any more is no longer placed at its center.
        if (ligand.sites.isEmpty()) {
            p.metadata.remove("coordination_center");
            p.metadata.remove("sites");
        } else
            p.metadata["sites"] = ligand.sites;
        ligands << ligand;
    }
    parts.removeIf([](const Part &p) { return p.atoms.isEmpty(); });
    for (auto &r : residues)
        r.atoms.removeIf([&](const auto &it) { return isGone(it.value()); });
    residues.removeIf([](const Residue &r) { return r.atoms.isEmpty(); });
    project::removeAtomReferences(data, removed);
    QSet<QString> residueIds;
    for (const auto &r : residues)
        residueIds.insert(r.id);
    for (auto &sequence : chains)
        sequence.removeIf([&](const QString &id) { return !residueIds.contains(id); });
    chains.removeIf([](const auto &it) { return it.value().isEmpty(); });
    centers.removeIf([&](const Center &c) { return isGone(c.atom); });
    for (auto &c : centers) {
        for (auto &site : c.sites)
            if (isGone(site.donor))
                site.donor = site.part = QString();
        // The sites of the ligands that lost atoms: those they still hold, named by atoms that are left.
        for (const auto &ligand : ligands) {
            if (ligand.center != c.atom)
                continue;
            for (auto &site : c.sites)
                if (site.part == ligand.part)
                    site.donor = site.part = QString();
            for (qsizetype k = 0; k < ligand.sites.size(); ++k)
                if (const int i = ligand.sites.at(k).toInt(-1); i >= 0 && i < c.sites.size()) {
                    c.sites[i].donor = ligand.donors[k];
                    c.sites[i].part = ligand.part;
                }
        }
    }
    // A ligand whose center is gone is no longer placed at its sites.
    for (auto &p : parts)
        if (isGone(p.center())) {
            p.metadata.remove("coordination_center");
            p.metadata.remove("sites");
        }
}
void Document::renameIds(const QHash<QString, QString> &renamed) {
    const auto name = [&](QString &id) { id = renamed.value(id, id); };
    const auto names = [&](Ids &ids) {
        for (auto &id : ids)
            name(id);
    };
    for (auto &a : atoms)
        name(a.id);
    for (auto &b : bonds) {
        name(b.a);
        name(b.b);
    }
    for (auto &r : residues) {
        name(r.id);
        name(r.chain);
        for (auto &id : r.atoms)
            name(id);
    }
    QMap<QString, Ids> renamedChains;
    for (auto it = chains.begin(); it != chains.end(); ++it) {
        auto sequence = it.value();
        names(sequence);
        renamedChains[renamed.value(it.key(), it.key())] = sequence;
    }
    chains = renamedChains;
    for (auto &p : parts) {
        name(p.id);
        names(p.atoms);
        names(p.connections);
        if (auto groups = p.hapto(); !groups.isEmpty()) {
            for (auto &group : groups)
                names(group);
            p.setHapto(groups);
        }
        if (const auto center = p.center(); renamed.contains(center))
            p.metadata["coordination_center"] = renamed[center];
    }
    for (auto &c : centers) {
        name(c.atom);
        for (auto &site : c.sites) {
            name(site.donor);
            name(site.part);
        }
    }
    project::renameAtomReferences(data, renamed);
}
namespace {
// The structure checked by Document::validate, independent of settings and the project envelope.
struct Structure {
    QVector<Atom> atoms;
    QVector<Bond> bonds;
    QVector<Residue> residues;
    QMap<QString, Ids> chains;
    QVector<Part> parts;
    QVector<Center> centers;
    QVector<PointCharge> pointCharges;
    bool operator==(const Structure &) const = default;
};
// The coordinates written to 10 decimals (0 for −0, as QString::arg writes it), as the key of a position.
QByteArray positionKey(const Vec &p) {
    char text[3 * 340];
    char *end = text;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis)
            *end++ = ',';
        end = std::to_chars(end, text + sizeof text, p[axis] + 0., std::chars_format::fixed, 10).ptr;
    }
    return QByteArray(text, end - text);
}
QStringList validateStructure(const Document &d) {
    const auto &[atoms, bonds, residues, parts, centers, pointCharges] =
        std::tie(d.atoms, d.bonds, d.residues, d.parts, d.centers, d.pointCharges);
    QStringList errors;
    QSet<QString> seen;
    QSet<QByteArray> positions;
    for (const auto &a : atoms) {
        if (a.id.isEmpty() || seen.contains(a.id))
            errors << tr("原子IDが空または重複しています。");
        seen.insert(a.id);
        // Coordinates stay within 10^6 Å, as in results: far beyond any molecule, and safe in grids.
        if (!a.position.allFinite() || a.position.cwiseAbs().maxCoeff() > 1e6 || !std::isfinite(a.mass) ||
            a.mass < 0 || a.isotope < 0 || a.radical < 0)
            errors << tr("原子の座標・属性が不正です。");
        const auto pos = positionKey(a.position);
        if (positions.contains(pos))
            errors << tr("2つの原子が同じ位置に重なっています。");
        positions.insert(pos);
        if (a.isotope && (a.isotope < element(a.element).number || a.mass > 0))
            errors << tr("同位体の指定が不正です。");
    }
    for (const auto &part : parts) {
        for (const auto &id : part.atoms)
            if (!seen.contains(id))
                errors << tr("部品の原子参照が不正です。");
        const auto groups = part.hapto();
        if (part.connections.isEmpty() && groups.isEmpty())
            continue;
        const QSet<QString> members(part.atoms.begin(), part.atoms.end());
        for (const auto &id : part.connections)
            if (!members.contains(id))
                errors << tr("部品の接続点が不正です。");
        for (const auto &group : groups)
            if (group.size() < 2 || std::any_of(group.begin(), group.end(),
                                                [&](const QString &id) { return !members.contains(id); }))
                errors << tr("部品のη配位の原子群が不正です。");
    }
    for (const auto &r : residues)
        for (const auto &id : r.atoms)
            if (!seen.contains(id))
                errors << tr("残基の原子参照が不正です。");
    for (const auto &c : centers) {
        if (!seen.contains(c.atom))
            errors << tr("配位中心の参照が不正です。");
        for (const auto &site : c.sites) {
            if (!site.donor.isEmpty() && !seen.contains(site.donor))
                errors << tr("配位サイトの参照が不正です。");
            if (!site.direction.allFinite())
                errors << tr("配位サイトの座標が不正です。");
            else if (site.direction.norm() < 1e-8 || !(site.distance > 0))
                errors << tr("配位サイトの方向・距離が不正です。");
        }
    }
    QSet<QString> pairs;
    for (const auto &b : bonds) {
        if (!seen.contains(b.a) || !seen.contains(b.b) || b.a == b.b)
            errors << tr("結合の参照が不正です。");
        QString pair = b.a < b.b ? b.a + b.b : b.b + b.a;
        if (pairs.contains(pair))
            errors << tr("結合が重複しています。");
        pairs.insert(pair);
        static const QStringList kinds{"single", "double", "triple", "aromatic", "dative"};
        if (!kinds.contains(b.kind))
            errors << tr("結合種類が不正です。");
    }
    for (const auto &q : pointCharges) {
        if (!q.position.allFinite() || q.position.cwiseAbs().maxCoeff() > 1e6)
            errors << tr("点電荷の座標が不正です。");
        if (!std::isfinite(q.charge))
            errors << tr("点電荷が不正です。");
    }
    errors.removeDuplicates(); // Each kind once, however many atoms share the problem.
    return errors;
}
// Residue IDs are unique; a chain lists each of its residues once, and only residues of that chain.
QStringList residueErrors(const Document &d) {
    QHash<QString, QString> chainOf;
    for (const auto &r : d.residues) {
        if (r.id.isEmpty() || chainOf.contains(r.id))
            return {tr("残基IDが空または重複しています。")};
        chainOf.insert(r.id, r.chain);
    }
    QSet<QString> listed;
    for (auto it = d.chains.begin(); it != d.chains.end(); ++it)
        for (const auto &id : it.value()) {
            if (!chainOf.contains(id) || chainOf[id] != it.key() || listed.contains(id))
                return {tr("鎖の残基参照が不正です。")};
            listed.insert(id);
        }
    for (const auto &r : d.residues)
        if (!r.chain.isEmpty() && !listed.contains(r.id))
            return {tr("鎖の残基参照が不正です。")};
    return {};
}
} // namespace
QStringList Document::validate() const {
    // Settings edits leave the structure shared, and its checks need not run again.
    thread_local Memo<Structure, QStringList, 2> memo;
    const Structure structure{atoms, bonds, residues, chains, parts, centers, pointCharges};
    return memo.get(structure, [&] { return validateStructure(*this) + residueErrors(*this); });
}
QString Document::formula() const {
    QMap<QString, int> counts;
    for (const auto &a : atoms)
        if (!a.ghost)
            counts[a.element]++;
    auto keys = counts.keys();
    if (counts.contains("C")) {
        keys.removeAll("C");
        keys.removeAll("H");
        keys.prepend("H");
        keys.prepend("C");
    }
    QString out;
    for (const auto &s : keys)
        if (counts[s])
            out += s + (counts[s] == 1 ? "" : QString::number(counts[s]));
    return out;
}
int Document::formalCharge() const {
    int n = 0;
    for (const auto &a : atoms)
        if (!a.ghost)
            n += a.charge;
    return n;
}
bool Document::hasCenters() const {
    return std::any_of(centers.begin(), centers.end(), [this](const Center &c) { return contains(c.atom); });
}
int Document::expectedCharge() const {
    int n = 0;
    QSet<QString> counted;
    for (const auto &c : centers)
        if (contains(c.atom) && !counted.contains(c.atom)) {
            counted.insert(c.atom);
            n += c.oxidation;
        }
    for (const auto &a : atoms)
        if (!a.ghost && !counted.contains(a.id))
            n += a.charge;
    return n;
}
QJsonObject Document::toJson() const {
    auto o = data;
    const auto rows = [](const auto &items) {
        QJsonArray out;
        for (const auto &item : items)
            out.append(item.toJson());
        return out;
    };
    o["atoms"] = rows(atoms);
    o["bonds"] = rows(bonds);
    o["residues"] = rows(residues);
    o["parts"] = rows(parts);
    o["centers"] = rows(centers);
    o["point_charges"] = rows(pointCharges);
    QJsonObject sequences;
    for (auto it = chains.begin(); it != chains.end(); ++it)
        sequences[it.key()] = idsToJson(it.value());
    o["chains"] = sequences;
    o["charge"] = charge;
    o["multiplicity"] = multiplicity;
    return o;
}
Document Document::fromJson(const QJsonObject &o) {
    if (o["units"] != "angstrom")
        fail(tr("プロジェクトの座標単位はangstromである必要があります。"));
    Document d;
    d.data = o;
    for (auto key : {"atoms", "bonds", "residues", "chains", "parts", "centers", "point_charges", "charge",
                     "multiplicity"})
        d.data.remove(key);
    for (const auto &v : o["residues"].toArray())
        d.residues << Residue::fromJson(v.toObject());
    const auto sequences = o["chains"].toObject();
    for (auto it = sequences.begin(); it != sequences.end(); ++it)
        d.chains[it.key()] = idsFromJson(it.value());
    for (const auto &v : o["parts"].toArray())
        d.parts << Part::fromJson(v.toObject());
    for (const auto &v : o["centers"].toArray())
        d.centers << Center::fromJson(v.toObject());
    for (const auto &v : o["point_charges"].toArray())
        d.pointCharges << PointCharge::fromJson(v.toObject());
    d.charge = o["charge"].toInt();
    d.multiplicity = o["multiplicity"].toInt(1);
    for (auto a : o["atoms"].toArray())
        d.atoms.append(Atom::fromJson(a.toObject()));
    for (auto v : o["bonds"].toArray()) {
        auto b = v.toObject();
        d.bonds.append({b["a"].toString(), b["b"].toString(), b["kind"].toString("single"),
                        b["stereo"].toString(), b["source"].toString("explicit")});
    }
    auto errors = d.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    return d;
}
QJsonObject Document::project() const {
    auto o = envelope;
    o["format"] = "molforma-project";
    o["version"] = 1;
    o["coordinate_units"] = "angstrom";
    o["document"] = toJson();
    return o;
}
Document Document::fromProject(const QJsonObject &o) {
    if (o["format"] != "molforma-project" || o["version"].toInt() != 1 || o["coordinate_units"] != "angstrom")
        fail(tr("MolFormaプロジェクト（.molforma.json、version 1）を指定してください。"));
    Document d = fromJson(o["document"].toObject());
    d.envelope = o;
    return d;
}
} // namespace mf
