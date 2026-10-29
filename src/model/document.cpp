// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/document.h"
#include "model/i18n.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>
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
    for (const auto &e : elements())
        if (e.symbol == s)
            return e;
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
    data = {{"id", newId()}, {"units", "angstrom"}, {"metadata", QJsonObject()}, {"chains", QJsonObject()}};
    for (auto key : {"parts", "residues", "centers", "point_charges"})
        data[key] = QJsonArray();
}
int Document::index(const QString &id) const {
    for (int i = 0; i < atoms.size(); ++i)
        if (atoms[i].id == id)
            return i;
    return -1;
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
Ids Document::component(const QString &root, const QString &ca, const QString &cb) const {
    if (!contains(root))
        fail(tr("原子を選択してください。"));
    Ids todo{root}, seen;
    while (!todo.isEmpty()) {
        auto id = todo.takeFirst();
        if (seen.contains(id))
            continue;
        seen << id;
        for (const auto &n : neighbors(id))
            if (!((id == ca && n == cb) || (id == cb && n == ca)))
                todo << n;
    }
    return seen;
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
    atoms.erase(
        std::remove_if(atoms.begin(), atoms.end(), [&](const Atom &a) { return gone.contains(a.id); }),
        atoms.end());
    bonds.erase(std::remove_if(bonds.begin(), bonds.end(),
                               [&](const Bond &b) { return gone.contains(b.a) || gone.contains(b.b); }),
                bonds.end());
    for (auto key : {"parts", "residues"}) {
        QJsonArray out;
        for (auto v : data[key].toArray()) {
            auto o = v.toObject();
            if (QString(key) == "parts") {
                auto list = idsFromJson(o["atoms"]);
                auto connections = idsFromJson(o["connections"]);
                for (const auto &id : gone) {
                    list.removeAll(id);
                    connections.removeAll(id);
                }
                if (list.isEmpty())
                    continue;
                o["atoms"] = idsToJson(list);
                o["connections"] = idsToJson(connections);
            } else {
                auto names = o["atoms"].toObject();
                for (auto it = names.begin(); it != names.end();) {
                    if (gone.contains(it.value().toString()))
                        it = names.erase(it);
                    else
                        ++it;
                }
                if (names.isEmpty())
                    continue;
                o["atoms"] = names;
            }
            out.append(o);
        }
        data[key] = out;
    }
    // NWChem internal-coordinate constraints and the scan coordinate refer to atoms by ID.
    if (auto nwchem = data.value("nwchem").toObject(); !nwchem.isEmpty()) {
        auto refersToGone = [&](const QJsonValue &atoms) {
            const auto list = idsFromJson(atoms);
            return std::any_of(list.begin(), list.end(), [&](const auto &id) { return gone.contains(id); });
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
    for (auto r : data["residues"].toArray())
        residueIds.insert(r.toObject()["id"].toString());
    auto chains = data["chains"].toObject();
    for (auto it = chains.begin(); it != chains.end();) {
        QJsonArray list;
        for (auto id : it.value().toArray())
            if (residueIds.contains(id.toString()))
                list.append(id);
        if (list.isEmpty())
            it = chains.erase(it);
        else {
            it.value() = list;
            ++it;
        }
    }
    data["chains"] = chains;
    QJsonArray centers;
    for (auto v : data["centers"].toArray()) {
        auto c = v.toObject();
        if (gone.contains(c["atom"].toString()))
            continue;
        QJsonArray sites;
        for (auto s : c["sites"].toArray()) {
            auto site = s.toObject();
            if (gone.contains(site["donor"].toString())) {
                site.remove("donor");
                site.remove("part");
            }
            sites.append(site);
        }
        c["sites"] = sites;
        centers.append(c);
    }
    data["centers"] = centers;
}
QStringList Document::validate() const {
    QStringList errors;
    QSet<QString> seen;
    QSet<QString> positions;
    for (const auto &a : atoms) {
        if (a.id.isEmpty() || seen.contains(a.id))
            errors << tr("原子IDが空または重複しています。");
        seen.insert(a.id);
        if (!a.position.allFinite() || !std::isfinite(a.mass) || a.mass < 0 || a.isotope < 0 || a.radical < 0)
            errors << tr("原子の座標・属性が不正です。");
        QString pos = QString("%1,%2,%3")
                          .arg(a.position.x(), 0, 'f', 10)
                          .arg(a.position.y(), 0, 'f', 10)
                          .arg(a.position.z(), 0, 'f', 10);
        if (positions.contains(pos))
            errors << tr("2つの原子が同じ位置に重なっています。");
        positions.insert(pos);
        if (a.isotope && (a.isotope < element(a.element).number || a.mass > 0))
            errors << tr("同位体の指定が不正です。");
    }
    for (auto v : data["parts"].toArray()) {
        auto part = v.toObject();
        auto ids = idsFromJson(part["atoms"]);
        for (auto id : ids)
            if (!seen.contains(id))
                errors << tr("部品の原子参照が不正です。");
        for (auto id : idsFromJson(part["connections"]))
            if (!ids.contains(id))
                errors << tr("部品の接続点が不正です。");
    }
    for (auto v : data["residues"].toArray())
        for (auto id : v.toObject()["atoms"].toObject())
            if (!seen.contains(id.toString()))
                errors << tr("残基の原子参照が不正です。");
    for (auto v : data["centers"].toArray()) {
        auto c = v.toObject();
        if (!seen.contains(c["atom"].toString()))
            errors << tr("配位中心の参照が不正です。");
        for (auto x : c["sites"].toArray()) {
            auto site = x.toObject();
            if (!site["donor"].toString().isEmpty() && !seen.contains(site["donor"].toString()))
                errors << tr("配位サイトの参照が不正です。");
            try {
                if (vectorFromJson(site["direction"]).norm() < 1e-8 || site["distance"].toDouble() <= 0)
                    errors << tr("配位サイトの方向・距離が不正です。");
            } catch (...) {
                errors << tr("配位サイトの座標が不正です。");
            }
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
        if (!QStringList{"single", "double", "triple", "aromatic", "dative"}.contains(b.kind))
            errors << tr("結合種類が不正です。");
    }
    for (auto v : data["point_charges"].toArray()) {
        auto p = v.toObject();
        try {
            vectorFromJson(p["position"]);
        } catch (...) {
            errors << tr("点電荷の座標が不正です。");
        }
        if (!p["charge"].isDouble() || !std::isfinite(p["charge"].toDouble()))
            errors << tr("点電荷が不正です。");
    }
    return errors;
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
    const auto list = data["centers"].toArray();
    return std::any_of(list.begin(), list.end(),
                       [this](const QJsonValue &c) { return contains(c.toObject()["atom"].toString()); });
}
int Document::expectedCharge() const {
    int n = 0;
    QSet<QString> centers;
    for (const auto &v : data["centers"].toArray()) {
        const auto c = v.toObject();
        if (contains(c["atom"].toString()) && !centers.contains(c["atom"].toString())) {
            centers.insert(c["atom"].toString());
            n += c["oxidation"].toInt();
        }
    }
    for (const auto &a : atoms)
        if (!a.ghost && !centers.contains(a.id))
            n += a.charge;
    return n;
}
QJsonObject Document::toJson() const {
    auto o = data;
    QJsonArray aa, bb;
    for (const auto &a : atoms)
        aa.append(a.toJson());
    for (const auto &b : bonds)
        bb.append(b.toJson());
    o["atoms"] = aa;
    o["bonds"] = bb;
    o["charge"] = charge;
    o["multiplicity"] = multiplicity;
    return o;
}
Document Document::fromJson(const QJsonObject &o) {
    if (o["units"] != "angstrom")
        fail(tr("プロジェクトの座標単位はangstromである必要があります。"));
    Document d;
    d.data = o;
    for (auto key : {"atoms", "bonds", "charge", "multiplicity"})
        d.data.remove(key);
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
    QHash<QString, QVector<int>> cells;
    auto key = [](int x, int y, int z) { return QString("%1,%2,%3").arg(x).arg(y).arg(z); };
    double width = 6.0;
    for (int i = 0; i < atoms.size(); ++i) {
        const auto &a = atoms[i];
        Vec p = (a.position / width).array().floor();
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z)
                    for (int j : cells.value(key(int(p.x()) + x, int(p.y()) + y, int(p.z()) + z))) {
                        const auto &b = atoms[j];
                        double dist = (a.position - b.position).norm();
                        if (!a.ghost && !b.ghost && dist > .25 &&
                            dist < 1.22 * (element(a.element).radius + element(b.element).radius))
                            bonds.append({b.id, a.id, "single", {}, "distance"});
                    }
        cells[key(int(p.x()), int(p.y()), int(p.z()))].append(i);
    }
}
} // namespace mf
