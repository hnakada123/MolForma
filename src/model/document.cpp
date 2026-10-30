// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/document.h"
#include "model/i18n.h"
#include "model/memo.h"
#include "model/topology.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <charconv>
#include <cmath>

static void initResources() {
    Q_INIT_RESOURCE(resources);
}
namespace mf {
QString newId() {
    return QUuid::createUuid().toString(QUuid::Id128);
}
void fail(const QString &s) {
    throw std::runtime_error(s.toStdString());
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
QByteArray readFile(const QString &p) {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    return f.readAll();
}
void writeFile(const QString &p, const QByteArray &bytes) {
    QSaveFile f(p);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        fail(f.errorString());
}
QJsonObject parseJson(const QByteArray &b) {
    QJsonParseError error;
    auto d = QJsonDocument::fromJson(b, &error);
    if (error.error != QJsonParseError::NoError || !d.isObject())
        fail(tr("JSONを読めません: %1").arg(error.errorString()));
    return d.object();
}
const QVector<Element> &elements() {
    static QVector<Element> values = [] {
        initResources();
        QVector<Element> result;
        for (auto v : parseJson(readFile(":/data/elements.json"))["elements"].toArray()) {
            auto o = v.toObject();
            result.append({o["symbol"].toString(), o["z"].toInt(), o["mass"].toDouble(),
                           o["covalent_radius"].toDouble(), QColor(o["color"].toString()),
                           o["name_ja"].toString(), o["name_en"].toString()});
        }
        return result;
    }();
    return values;
}
QString Element::name() const {
    return english() ? nameEn : nameJa;
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
    for (auto &p : parts) {
        p.atoms.removeIf(isGone);
        p.connections.removeIf(isGone);
    }
    parts.removeIf([](const Part &p) { return p.atoms.isEmpty(); });
    for (auto &r : residues)
        r.atoms.removeIf([&](const auto &it) { return isGone(it.value()); });
    residues.removeIf([](const Residue &r) { return r.atoms.isEmpty(); });
    // NWChem internal-coordinate constraints and the scan coordinate refer to atoms by ID.
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
        if (refersToGone(nwchem.value("scan_atoms")))
            nwchem["scan_atoms"] = QJsonArray();
        data["nwchem"] = nwchem;
    }
    QSet<QString> residueIds;
    for (const auto &r : residues)
        residueIds.insert(r.id);
    for (auto &sequence : chains)
        sequence.removeIf([&](const QString &id) { return !residueIds.contains(id); });
    chains.removeIf([](const auto &it) { return it.value().isEmpty(); });
    centers.removeIf([&](const Center &c) { return isGone(c.atom); });
    for (auto &c : centers)
        for (auto &site : c.sites)
            if (isGone(site.donor))
                site.donor = site.part = QString();
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
        if (nwchem.value("scan_atoms").isArray())
            nwchem["scan_atoms"] = renamedIds(nwchem["scan_atoms"]);
        data["nwchem"] = nwchem;
    }
}
namespace {
// What Document::validate checks: everything but the chains, settings and envelope.
struct Structure {
    QVector<Atom> atoms;
    QVector<Bond> bonds;
    QVector<Residue> residues;
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
        if (part.connections.isEmpty())
            continue;
        const QSet<QString> members(part.atoms.begin(), part.atoms.end());
        for (const auto &id : part.connections)
            if (!members.contains(id))
                errors << tr("部品の接続点が不正です。");
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
    const Structure structure{atoms, bonds, residues, parts, centers, pointCharges};
    return memo.get(structure, [&] { return validateStructure(*this); });
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
    auto errors = d.validate() + residueErrors(d);
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
void Document::save(const QString &path) const {
    auto errors = validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    writeFile(path, QJsonDocument(project()).toJson());
}
Document Document::load(const QString &path) {
    return fromProject(parseJson(readFile(path)));
}
QString Document::xyz() const {
    QString s = QString::number(atoms.size()) + "\nMolForma | " + formula() + " | angstrom\n";
    for (const auto &a : atoms)
        s += QString("%1 %2 %3 %4%5\n")
                 .arg((a.ghost ? "@" : "") + a.element)
                 .arg(a.position.x(), 0, 'f', 10)
                 .arg(a.position.y(), 0, 'f', 10)
                 .arg(a.position.z(), 0, 'f', 10)
                 .arg(a.isotope    ? " " + QString::number(a.isotope)
                      : a.mass > 0 ? " " + QString::number(a.mass, 'g', 15)
                                   : "");
    return s;
}
Document Document::fromXyz(const QString &text) {
    auto lines = text.split('\n');
    if (lines.size() < 2)
        fail(tr("XYZには原子数とコメント行が必要です。"));
    bool ok = false;
    int n = lines[0].trimmed().toInt(&ok);
    if (!ok || n < 0 || n > 100000 || lines.size() < n + 2)
        fail(tr("XYZの原子数が不正です。"));
    Document d;
    for (int i = 0; i < n; ++i) {
        auto w = lines[i + 2].simplified().split(' ');
        if (w.size() < 4 || w.size() > 5)
            fail(tr("XYZ %1行目の形式が不正です。").arg(i + 3));
        Atom a;
        a.element = w[0];
        a.ghost = a.element.startsWith('@');
        if (a.ghost)
            a.element.remove(0, 1);
        if (a.element == "D" || a.element == "T") {
            a.isotope = a.element == "D" ? 2 : 3;
            a.element = "H";
        }
        try {
            element(a.element);
        } catch (const std::exception &e) {
            fail(tr("XYZ %1行目: %2").arg(QString::number(i + 3), QString::fromUtf8(e.what())));
        }
        for (int j = 0; j < 3; ++j) {
            a.position[j] = w[j + 1].toDouble(&ok);
            if (!ok || !std::isfinite(a.position[j]))
                fail(tr("XYZ %1行目の座標は有限の数値にしてください: %2")
                         .arg(QString::number(i + 3), lines[i + 2].trimmed()));
        }
        if (w.size() == 5) {
            int mass = w[4].toInt(&ok);
            if (ok && mass > 0)
                a.isotope = mass;
            else {
                a.mass = w[4].toDouble(&ok);
                if (!ok || a.mass <= 0 || !std::isfinite(a.mass))
                    fail(tr("XYZ %1行目の質量が不正です: %2")
                             .arg(QString::number(i + 3), lines[i + 2].trimmed()));
            }
        }
        d.atoms.append(a);
    }
    for (int i = n + 2; i < lines.size(); ++i)
        if (!lines[i].trimmed().isEmpty())
            fail(tr("複数フレームのXYZです。1フレームのファイルを指定してください。"));
    d.inferBonds();
    return d;
}
void Document::inferBonds() {
    bonds.clear();
    // Spatial cells keep PDB-size structures from doing an all-pairs distance search.
    // Cells by their packed coordinates (21 bits each: neighboring cells never share a key).
    QHash<quint64, QVector<int>> cells;
    auto key = [](int x, int y, int z) {
        return (quint64(quint32(x) & 0x1fffff) << 42) | (quint64(quint32(y) & 0x1fffff) << 21) |
               quint64(quint32(z) & 0x1fffff);
    };
    // Covalent radii, looked up when a pair first needs them (an unknown element fails there).
    QVector<double> radii(atoms.size(), -1);
    const auto radius = [&](int k) {
        if (radii[k] < 0)
            radii[k] = element(atoms[k].element).radius;
        return radii[k];
    };
    // Real atoms have at most about a dozen neighbors (16 C around U in uranocene); atoms packed
    // closer than any molecule would otherwise give every pair a bond.
    constexpr int maximumBonds = 16;
    QVector<int> degree(atoms.size(), 0);
    double width = 6.0;
    for (int i = 0; i < atoms.size(); ++i) {
        const auto &a = atoms[i];
        // Clamped so that the conversion to int stays defined for coordinates far out of range.
        Vec p = (a.position / width).array().floor().cwiseMax(-1e6).cwiseMin(1e6);
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z) {
                    const auto cell = cells.constFind(key(int(p.x()) + x, int(p.y()) + y, int(p.z()) + z));
                    if (cell == cells.cend())
                        continue;
                    for (int j : *cell) {
                        const auto &b = atoms[j];
                        double dist = (a.position - b.position).norm();
                        if (!a.ghost && !b.ghost && dist > .25 && dist < 1.22 * (radius(i) + radius(j)) &&
                            degree[i] < maximumBonds && degree[j] < maximumBonds) {
                            bonds.append({b.id, a.id, "single", {}, "distance"});
                            ++degree[i];
                            ++degree[j];
                        }
                    }
                }
        cells[key(int(p.x()), int(p.y()), int(p.z()))].append(i);
    }
}
} // namespace mf
