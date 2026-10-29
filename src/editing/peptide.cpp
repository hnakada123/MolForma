// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/peptide.h"
#include "model/i18n.h"
#include <QRegularExpression>
#include <cmath>
namespace mf {
QStringList parseSequence(const QString &text) {
    QMap<QString, QString> codes;
    for (auto v : templates()) {
        auto t = v.toObject();
        if (t["category"] == "amino")
            codes[t["one_letter"].toString()] = t["code"].toString();
    }
    auto s = text.toUpper().trimmed();
    QStringList result;
    bool three = s.contains('-') || s.contains(' ');
    if (three) {
        for (auto c : s.split(QRegularExpression("[-\\s]+"), Qt::SkipEmptyParts)) {
            if (!codes.values().contains(c))
                fail(tr("未知の3文字残基コード: %1").arg(c));
            result << c;
        }
    } else
        for (auto c : s) {
            QString key(c);
            if (!codes.contains(key))
                fail(tr("未知の1文字残基コード: %1").arg(key));
            result << codes[key];
        }
    if (result.isEmpty())
        fail(tr("配列を入力してください（AGS または Ala-Gly-Ser）。"));
    return result;
}
QJsonObject residue(const Document &d, const QString &id) {
    for (auto v : d.data["residues"].toArray())
        if (v.toObject()["id"] == id)
            return v.toObject();
    fail(tr("残基を選択してください。"));
}
QString residueForAtom(const Document &d, const QString &id) {
    for (auto v : d.data["residues"].toArray()) {
        auto r = v.toObject();
        for (auto a : r["atoms"].toObject())
            if (a.toString() == id)
                return r["id"].toString();
    }
    return {};
}
QHash<QString, QString> atomPlaces(const Document &d) {
    QHash<QString, QString> places;
    for (auto v : d.data["residues"].toArray()) {
        const auto r = v.toObject();
        const auto code = QString("%1 %2").arg(r["code"].toString()).arg(r["number"].toInt());
        const auto names = r["atoms"].toObject();
        for (auto it = names.begin(); it != names.end(); ++it)
            if (const auto id = it.value().toString(); !places.contains(id))
                places.insert(id, code + " · " + it.key());
    }
    return places;
}
QString atomPlace(const Document &d, const QString &atom) {
    const auto rid = residueForAtom(d, atom);
    if (rid.isEmpty())
        return {};
    const auto r = residue(d, rid);
    const auto names = r["atoms"].toObject();
    for (auto it = names.begin(); it != names.end(); ++it)
        if (it.value().toString() == atom)
            return QString("%1 %2 · %3").arg(r["code"].toString()).arg(r["number"].toInt()).arg(it.key());
    return {};
}
void updateResidue(Document &d, const QJsonObject &r) {
    auto rows = d.data["residues"].toArray();
    for (int i = 0; i < rows.size(); ++i)
        if (rows[i].toObject()["id"] == r["id"]) {
            rows[i] = r;
            d.data["residues"] = rows;
            return;
        }
    rows.append(r);
    d.data["residues"] = rows;
}
static QString named(const QJsonObject &r, const QString &name) {
    auto id = r["atoms"].toObject()[name].toString();
    if (id.isEmpty())
        fail(tr("残基に原子 %1 がありません。").arg(name));
    return id;
}
static Ids hydrogens(const Document &d, const QString &id) {
    Ids out;
    for (auto n : d.neighbors(id))
        if (d.atom(n).element == "H")
            out << n;
    return out;
}
static QJsonObject aminoTemplate(QString code, QString state, QString chirality) {
    if (code == "HIS" && state == "default")
        state = "HIE";
    auto t = findTemplate("amino:" + code + ":" + state);
    if (chirality == "D" && code != "GLY") {
        auto aa = t["atoms"].toArray();
        for (int i = 0; i < aa.size(); ++i) {
            auto a = aa[i].toObject();
            Vec p = vectorFromJson(a["position"]);
            p.z() *= -1;
            a["position"] = vectorToJson(p);
            a["stereo"] = a["stereo"] == "S" ? "R" : a["stereo"] == "R" ? "S" : "";
            aa[i] = a;
        }
        t["atoms"] = aa;
    }
    return t;
}
static QString placeAmino(Document &d, const QString &code, const QString &state, const QString &chirality,
                          const QMap<QString, Vec> &targets, const Vec &position, const QString &chain = {},
                          int number = 1) {
    auto t = aminoTemplate(code, state, chirality);
    auto aa = t["atoms"].toArray();
    QVector<Vec> points;
    QMap<QString, int> names;
    for (int i = 0; i < aa.size(); ++i) {
        auto a = aa[i].toObject();
        names[a["name"].toString()] = i;
        points << vectorFromJson(a["position"]);
    }
    if (!targets.isEmpty()) {
        QVector<Vec> source, dest;
        for (auto name : {"N", "CA", "C"}) {
            source << points[names[name]];
            dest << targets[name];
        }
        auto rt = fitPoints(source, dest);
        for (auto &p : points)
            p = rt.first * p + rt.second;
        Vec shift = targets["CA"] - points[names["CA"]];
        for (auto &p : points)
            p += shift;
        for (auto it = targets.begin(); it != targets.end(); ++it)
            points[names[it.key()]] = it.value();
    } else {
        Vec shift = position - points[names["CA"]];
        for (auto &p : points)
            p += shift;
    }
    auto part = instantiate(d, t, points);
    QJsonObject atomNames;
    for (auto it = names.begin(); it != names.end(); ++it)
        atomNames[it.key()] = part.mapping[it.value()];
    QString rid = newId(), cid = chain.isEmpty() ? newId() : chain;
    QJsonObject r{{"id", rid},
                  {"code", code},
                  {"atoms", atomNames},
                  {"chain", cid},
                  {"number", number},
                  {"insertion_code", ""},
                  {"chirality", code == "GLY" ? "" : chirality},
                  {"state", state},
                  {"n_terminal", "neutral"},
                  {"c_terminal", "neutral"},
                  {"matched", true},
                  {"modified", false},
                  {"metadata", QJsonObject()}};
    updateResidue(d, r);
    auto chains = d.data["chains"].toObject();
    auto seq = chains[cid].toArray();
    seq.append(rid);
    chains[cid] = seq;
    d.data["chains"] = chains;
    return rid;
}
static QString addNamedAtom(Document &d, const QString &rid, const QString &parent, const QString &name,
                            const QString &symbol, const Vec &pos) {
    Atom a;
    a.element = symbol;
    a.name = name;
    a.position = pos;
    d.atoms << a;
    d.setBond(parent, a.id, "single");
    auto r = residue(d, rid), names = r["atoms"].toObject();
    QString unique = name;
    int i = 1;
    while (names.contains(unique))
        unique = name + QString::number(++i);
    d.atom(a.id).name = unique;
    names[unique] = a.id;
    r["atoms"] = names;
    updateResidue(d, r);
    auto parts = d.data["parts"].toArray();
    for (int j = 0; j < parts.size(); ++j) {
        auto p = parts[j].toObject();
        auto ids = idsFromJson(p["atoms"]);
        if (ids.contains(parent)) {
            ids << a.id;
            p["atoms"] = idsToJson(ids);
            parts[j] = p;
            break;
        }
    }
    d.data["parts"] = parts;
    return a.id;
}
static void addH(Document &d, const QString &rid, const QString &parent, const QString &name) {
    Vec direction = freeDirection(d, parent);
    addNamedAtom(d, rid, parent, name, "H",
                 d.atom(parent).position + unit(direction) * (d.atom(parent).element == "N" ? 1.01 : .98));
}
void setTerminals(Document &d, const QString &rid, const QString &n, const QString &c) {
    if (n == "ACE" || c == "NME") {
        if (n == "ACE")
            addCap(d, rid, "ACE");
        else if (!n.isEmpty())
            setTerminals(d, rid, n, {});
        if (c == "NME")
            addCap(d, rid, "NME");
        else if (!c.isEmpty())
            setTerminals(d, rid, {}, c);
        return;
    }
    auto r = residue(d, rid);
    // Remove the complete cap before rebuilding the free terminal group.
    for (auto key : {QString("n_terminal"), QString("c_terminal")}) {
        if ((key == "n_terminal" ? n : c).isEmpty())
            continue;
        QString kind = r[key].toString();
        if (kind != "ACE" && kind != "NME")
            continue;
        const auto parent = named(r, kind == "ACE" ? "N" : "C");
        for (auto neighbor : d.neighbors(parent)) {
            auto capId = residueForAtom(d, neighbor);
            if (capId.isEmpty() || capId == rid)
                continue;
            auto cap = residue(d, capId);
            if (cap["code"] != kind)
                continue;
            Ids removed;
            for (auto id : cap["atoms"].toObject())
                removed << id.toString();
            d.remove(removed);
        }
    }
    if (!n.isEmpty()) {
        if (!QStringList{"neutral", "charged", "internal"}.contains(n))
            fail(tr("N末端状態が不正です。"));
        QString id = named(r, "N");
        int count = n == "charged" ? 3 : n == "neutral" ? 2 : 1;
        if (r["code"] == "PRO")
            --count;
        auto hs = hydrogens(d, id);
        while (hs.size() > count)
            d.remove({hs.takeLast()});
        while (hydrogens(d, id).size() < count)
            addH(d, rid, id, "HN" + QString::number(hydrogens(d, id).size() + 1));
        d.atom(id).charge = n == "charged" ? 1 : 0;
        r = residue(d, rid);
        r["n_terminal"] = n;
        updateResidue(d, r);
    }
    if (!c.isEmpty()) {
        if (!QStringList{"neutral", "charged", "internal"}.contains(c))
            fail(tr("C末端状態が不正です。"));
        r = residue(d, rid);
        QString carbon = named(r, "C"), oxt = r["atoms"].toObject()["OXT"].toString();
        if (c == "internal") {
            if (d.contains(oxt)) {
                auto ids = hydrogens(d, oxt);
                ids << oxt;
                d.remove(ids);
            }
        } else {
            if (!d.contains(oxt)) {
                Vec origin = d.atom(carbon).position;
                Vec dir = -unit(d.atom(named(r, "CA")).position - origin) -
                          unit(d.atom(named(r, "O")).position - origin);
                oxt = addNamedAtom(d, rid, carbon, "OXT", "O", origin + 1.33 * unit(dir));
            }
            d.atom(oxt).charge = c == "charged" ? -1 : 0;
            if (c == "charged")
                d.remove(hydrogens(d, oxt));
            else if (hydrogens(d, oxt).isEmpty())
                addH(d, rid, oxt, "HOXT1");
        }
        r = residue(d, rid);
        r["c_terminal"] = c;
        updateResidue(d, r);
    }
}
static void orientAmideH(Document &d, const QString &rid, const QString &carbon);
void addCap(Document &d, const QString &rid, const QString &kind) {
    auto r = residue(d, rid);
    bool ace = kind == "ACE";
    if (!ace && kind != "NME")
        fail(tr("対応するキャップはACEとNMEです。"));
    auto state = r[ace ? "n_terminal" : "c_terminal"].toString();
    if (state == kind)
        return;
    if (state != "neutral" && state != "charged")
        fail(tr("空いている末端へキャップを付けてください。"));
    QString parent = named(r, ace ? "N" : "C"), leaving;
    if (ace) {
        setTerminals(d, rid, "neutral", {});
        auto hs = hydrogens(d, parent);
        if (hs.isEmpty())
            fail(tr("N末端に置換可能なHがありません。"));
        leaving = hs.last();
        Vec c = d.atom(named(r, "C")).position, ca = d.atom(named(r, "CA")).position,
            n = d.atom(parent).position;
        if (d.atom(leaving).fixed)
            fail(tr("固定水素にキャップを付けられません。"));
        d.atom(leaving).position = internalPoint(c, ca, n, 1.01, 121, r["chirality"] == "D" ? 135 : -135);
    } else {
        setTerminals(d, rid, {}, "neutral");
        r = residue(d, rid);
        QString o = named(r, "OXT");
        Vec position = d.atom(o).position;
        auto removed = hydrogens(d, o);
        removed << o;
        d.remove(removed);
        Atom temporary;
        temporary.element = "H";
        temporary.position = position;
        d.atoms << temporary;
        d.setBond(parent, temporary.id, "single");
        leaving = temporary.id;
    }
    auto part = attachPart(d, leaving, findTemplate(ace ? "substituent:acetyl" : "cap:NME"));
    QString connection = part.connections.first(), methyl, oxygen;
    QJsonObject names;
    int h = 0;
    for (auto id : part.atoms) {
        auto &a = d.atom(id);
        if (id == connection)
            a.name = ace ? "C" : "N";
        else if (a.element == "C") {
            a.name = "CH3";
            methyl = id;
        } else if (a.element == "O") {
            a.name = "O";
            oxygen = id;
        } else
            a.name = "H" + QString::number(++h);
        names[a.name] = id;
    }
    setDimension(d, {parent, connection}, 1.33);
    if (!methyl.isEmpty())
        setDimension(d, {named(r, "CA"), parent, connection, methyl}, 180);
    if (!ace) {
        Vec n = d.atom(connection).position;
        for (auto id : hydrogens(d, connection))
            d.atom(id).position =
                n + 1.01 * unit(-unit(d.atom(parent).position - n) - unit(d.atom(methyl).position - n));
    } else
        orientAmideH(d, rid, connection); // The N–H left of the amino group, into the new amide plane.
    QString capId = newId();
    QJsonObject cap{{"id", capId},
                    {"code", kind},
                    {"atoms", names},
                    {"chain", r["chain"]},
                    {"number", r["number"].toInt() + (ace ? -1 : 1)},
                    {"insertion_code", ""},
                    {"chirality", ""},
                    {"state", "default"},
                    {"n_terminal", "internal"},
                    {"c_terminal", "internal"},
                    {"matched", true},
                    {"modified", false},
                    {"metadata", QJsonObject()}};
    updateResidue(d, cap);
    r = residue(d, rid);
    r[ace ? "n_terminal" : "c_terminal"] = kind;
    updateResidue(d, r);
    auto chains = d.data["chains"].toObject();
    auto seq = idsFromJson(chains[r["chain"].toString()]);
    int index = seq.indexOf(rid);
    seq.insert(index + (ace ? 0 : 1), capId);
    chains[r["chain"].toString()] = idsToJson(seq);
    d.data["chains"] = chains;
}
static void orientAmideH(Document &d, const QString &rid, const QString &carbon) {
    auto r = residue(d, rid);
    QString nitrogen = named(r, "N");
    Vec n = d.atom(nitrogen).position;
    Vec dir = -unit(d.atom(named(r, "CA")).position - n) - unit(d.atom(carbon).position - n);
    for (auto id : hydrogens(d, nitrogen)) {
        if (d.atom(id).fixed)
            fail(tr("固定水素を動かせません。"));
        d.atom(id).position = n + 1.01 * unit(dir);
    }
}
QString appendResidue(Document &d, const QString &code, const QString &previous, const QString &state,
                      const QString &chirality, const QString &nstate, const QString &cstate,
                      const Vec &position) {
    double sign = chirality == "D" ? -1 : 1;
    QString result;
    if (previous.isEmpty()) {
        Vec n(0, 0, 0), ca(1.46, 0, 0),
            c = ca + Vec(1.52 * std::cos(69 * M_PI / 180), 1.52 * std::sin(69 * M_PI / 180), 0);
        Vec o = internalPoint(n, ca, c, 1.23, 120, -45 * sign);
        result = placeAmino(
            d, code, state, chirality,
            {{"N", n + position}, {"CA", ca + position}, {"C", c + position}, {"O", o + position}}, position);
        setTerminals(d, result, {}, "internal");
        setTerminals(d, result, nstate, cstate);
        return result;
    }
    auto prev = residue(d, previous);
    if (!QStringList{"neutral", "charged"}.contains(prev["c_terminal"].toString()))
        fail(tr("空いているC末端を選択してください。"));
    Vec ca = d.atom(named(prev, "CA")).position, c = d.atom(named(prev, "C")).position,
        o = d.atom(named(prev, "O")).position;
    Vec nn = c + unit(-unit(ca - c) - unit(o - c)) * 1.33, nca = internalPoint(ca, c, nn, 1.46, 121, 180),
        nc = internalPoint(c, nn, nca, 1.52, 111, -135 * sign),
        no = internalPoint(nn, nca, nc, 1.23, 120, -45 * sign);
    result = placeAmino(d, code, state, chirality, {{"N", nn}, {"CA", nca}, {"C", nc}, {"O", no}},
                        Vec::Zero(), prev["chain"].toString(), prev["number"].toInt() + 1);
    setTerminals(d, previous, {}, "internal");
    setTerminals(d, result, "internal", "internal");
    setTerminals(d, result, {}, cstate);
    d.setBond(named(prev, "C"), named(residue(d, result), "N"), "single");
    orientAmideH(d, result, named(prev, "C"));
    return result;
}
QString prependResidue(Document &d, const QString &code, const QString &following, const QString &state,
                       const QString &chirality, const QString &nstate) {
    auto next = residue(d, following);
    if (!QStringList{"neutral", "charged"}.contains(next["n_terminal"].toString()))
        fail(tr("空いているN末端を選択してください。"));
    Vec n = d.atom(named(next, "N")).position, ca = d.atom(named(next, "CA")).position,
        c = d.atom(named(next, "C")).position;
    double sign = chirality == "D" ? -1 : 1;
    Vec nc = internalPoint(c, ca, n, 1.33, 121, next["chirality"] == "D" ? 135 : -135),
        nca = internalPoint(ca, n, nc, 1.52, 116, 180), nn = internalPoint(n, nc, nca, 1.46, 111, 135 * sign),
        no = nc + 1.23 * unit(-unit(nca - nc) - unit(n - nc));
    QString rid = placeAmino(d, code, state, chirality, {{"N", nn}, {"CA", nca}, {"C", nc}, {"O", no}},
                             Vec::Zero(), next["chain"].toString(), next["number"].toInt() - 1);
    auto chains = d.data["chains"].toObject();
    auto seq = idsFromJson(chains[next["chain"].toString()]);
    seq.removeAll(rid);
    seq.prepend(rid);
    chains[next["chain"].toString()] = idsToJson(seq);
    d.data["chains"] = chains;
    setTerminals(d, rid, nstate, "internal");
    setTerminals(d, following, "internal", {});
    d.setBond(named(residue(d, rid), "C"), named(next, "N"), "single");
    orientAmideH(d, following, named(residue(d, rid), "C"));
    return rid;
}
QString addTerminalResidue(Document &d, const QString &rid, bool nTerminus, const QString &code,
                           const QString &state, const QString &chirality, const QString &n,
                           const QString &c) {
    QString added;
    followingCharge(d, [&] {
        added = nTerminus ? prependResidue(d, code, rid, state, chirality, n)
                          : appendResidue(d, code, rid, state, chirality, n, c);
    });
    return added;
}
void buildPeptide(Document &d, const QStringList &codes, const QString &n, const QString &c,
                  const QString &chirality, const QString &state) {
    if (codes.isEmpty() || codes.size() > 1000)
        fail(tr("配列長は1〜1000残基にしてください。"));
    QString prev, first;
    QString nt = n == "ACE" ? "neutral" : n, ct = c == "NME" ? "neutral" : c;
    followingCharge(d, [&] {
        for (const auto &code : codes) {
            if (codes.size() == 1) {
                prev = placeAmino(d, code, state, chirality, {}, Vec::Zero());
                setTerminals(d, prev, nt, ct);
            } else
                prev = appendResidue(d, code, prev, state, chirality, nt, ct);
            if (first.isEmpty())
                first = prev;
        }
        if (n == "ACE")
            addCap(d, first, "ACE");
        if (c == "NME")
            addCap(d, prev, "NME");
    });
}
void setChainTerminals(Document &d, const QString &rid, const QString &n, const QString &c) {
    const auto chain = idsFromJson(d.data["chains"].toObject()[residue(d, rid)["chain"].toString()]);
    if (chain.isEmpty())
        fail(tr("鎖情報がありません。"));
    followingCharge(d, [&] {
        setTerminals(d, chain.first(), n, {});
        setTerminals(d, chain.last(), {}, c);
    });
}
static void replaceResidue(Document &d, const QString &rid, const QString &code, const QString &state,
                           const QString &chirality) {
    auto old = residue(d, rid);
    QMap<QString, Vec> targets;
    for (auto name : {"N", "CA", "C", "O"})
        targets[name] = d.atom(named(old, name)).position;
    auto snapshot = d;
    QString fresh = placeAmino(d, code, state, chirality, targets, Vec::Zero());
    const auto n = old["n_terminal"].toString(), c = old["c_terminal"].toString();
    setTerminals(d, fresh, n == "ACE" ? "internal" : n, c == "NME" ? "internal" : c);
    auto replacement = residue(d, fresh);
    auto freshNames = replacement["atoms"].toObject(), oldNames = old["atoms"].toObject();
    QMap<QString, QString> preserved;
    for (auto it = freshNames.begin(); it != freshNames.end(); ++it)
        if (oldNames.contains(it.key()))
            preserved[it.value().toString()] = oldNames[it.key()].toString();
    Ids oldIds;
    for (auto id : oldNames)
        oldIds << id.toString();
    d.remove(oldIds);
    for (auto &a : d.atoms)
        if (preserved.contains(a.id))
            a.id = preserved[a.id];
    for (auto &b : d.bonds) {
        if (preserved.contains(b.a))
            b.a = preserved[b.a];
        if (preserved.contains(b.b))
            b.b = preserved[b.b];
    }
    for (auto it = freshNames.begin(); it != freshNames.end(); ++it)
        if (preserved.contains(it.value().toString()))
            it.value() = preserved[it.value().toString()];
    for (const auto &b : snapshot.bonds)
        if (oldIds.contains(b.a) != oldIds.contains(b.b)) {
            if (d.contains(b.a) && d.contains(b.b))
                d.setBond(b.a, b.b, b.kind);
            else
                fail(tr("置換で失われる側鎖に外部結合があります。"));
        }
    auto residues = d.data["residues"].toArray();
    for (int i = residues.size() - 1; i >= 0; --i)
        if (residues[i].toObject()["id"] == fresh)
            residues.removeAt(i);
    d.data["residues"] = residues;
    replacement["id"] = rid;
    replacement["chain"] = old["chain"];
    replacement["number"] = old["number"];
    replacement["n_terminal"] = n;
    replacement["c_terminal"] = c;
    replacement["atoms"] = freshNames;
    updateResidue(d, replacement);
    d.data["chains"] = snapshot.data["chains"];
    auto parts = d.data["parts"].toArray();
    for (int i = 0; i < parts.size(); ++i) {
        auto p = parts[i].toObject();
        auto aa = idsFromJson(p["atoms"]);
        for (auto &id : aa)
            if (preserved.contains(id))
                id = preserved[id];
        p["atoms"] = idsToJson(aa);
        auto cc = idsFromJson(p["connections"]);
        for (auto &id : cc)
            if (preserved.contains(id))
                id = preserved[id];
        p["connections"] = idsToJson(cc);
        parts[i] = p;
    }
    d.data["parts"] = parts;
    // Inside a chain the amide hydrogen goes into the peptide plane, as when a residue is added: the
    // template's is one of a free amino group and may point at the previous residue's carbonyl.
    const auto nitrogen = named(residue(d, rid), "N");
    for (const auto &neighbor : d.neighbors(nitrogen))
        if (d.atom(neighbor).element == "C" && residueForAtom(d, neighbor) != rid) {
            orientAmideH(d, rid, neighbor);
            break;
        }
}
void mutateResidue(Document &d, const QString &rid, const QString &code, const QString &state,
                   const QString &chirality) {
    followingCharge(d, [&] { replaceResidue(d, rid, code, state, chirality); });
}
void deleteResidue(Document &d, const QString &rid) {
    auto r = residue(d, rid);
    if (r["code"] == "ACE" || r["code"] == "NME") {
        const bool ace = r["code"] == "ACE";
        auto connection = named(r, ace ? "C" : "N");
        for (auto neighbor : d.neighbors(connection)) {
            auto parent = residueForAtom(d, neighbor);
            if (!parent.isEmpty() && parent != rid) {
                setTerminals(d, parent, ace ? "neutral" : "", ace ? "" : "neutral");
                return;
            }
        }
    }
    if (r["n_terminal"] == "ACE")
        setTerminals(d, rid, "neutral", {});
    if (r["c_terminal"] == "NME")
        setTerminals(d, rid, {}, "neutral");
    r = residue(d, rid);
    auto chain = r["chain"].toString();
    auto sequence = idsFromJson(d.data["chains"].toObject()[chain]);
    int index = sequence.indexOf(rid);
    Ids ids;
    for (auto id : r["atoms"].toObject())
        ids << id.toString();
    d.remove(ids);
    if (index > 0)
        setTerminals(d, sequence[index - 1], {}, "neutral");
    if (index + 1 < sequence.size()) {
        QString nc = newId();
        auto chains = d.data["chains"].toObject();
        chains[chain] = idsToJson(sequence.mid(0, index));
        chains[nc] = idsToJson(sequence.mid(index + 1));
        d.data["chains"] = chains;
        for (auto id : sequence.mid(index + 1)) {
            auto rr = residue(d, id);
            rr["chain"] = nc;
            updateResidue(d, rr);
        }
        setTerminals(d, sequence[index + 1], "neutral", {});
    }
}
void setResidueTorsion(Document &d, const QString &rid, const QString &kind, double angle) {
    auto r = residue(d, rid);
    auto seq = idsFromJson(d.data["chains"].toObject()[r["chain"].toString()]);
    int i = seq.indexOf(rid);
    Ids ids;
    if (kind == "phi") {
        if (i <= 0)
            fail(tr("N末端にphiは定義できません。"));
        ids = {named(residue(d, seq[i - 1]), "C"), named(r, "N"), named(r, "CA"), named(r, "C")};
    } else if (kind == "psi" || kind == "omega") {
        if (i + 1 >= seq.size())
            fail(tr("C末端にこの二面角は定義できません。"));
        auto next = residue(d, seq[i + 1]);
        ids = kind == "psi" ? Ids{named(r, "N"), named(r, "CA"), named(r, "C"), named(next, "N")}
                            : Ids{named(r, "CA"), named(r, "C"), named(next, "N"), named(next, "CA")};
    } else {
        QStringList names{"N", "CA", "CB"};
        auto map = r["atoms"].toObject();
        for (auto candidates :
             {QStringList{"CG", "CG1", "OG", "OG1", "SG"}, QStringList{"CD", "CD1", "OD1", "ND1", "SD"},
              QStringList{"CE", "CE1", "NE"}, QStringList{"NZ", "CZ"}, QStringList{"NH1"}}) {
            for (auto name : candidates)
                if (map.contains(name)) {
                    names << name;
                    break;
                }
        }
        bool ok = false;
        int chi = kind.mid(3).toInt(&ok);
        if (!ok || chi < 1 || chi + 3 > names.size())
            fail(tr("この残基に指定のchi二面角はありません。"));
        for (auto name : names.mid(chi - 1, 4))
            ids << named(r, name);
    }
    setDimension(d, ids, angle);
}
void registerResidue(Document &d, const Ids &ids, const QString &code) {
    if (ids.isEmpty())
        fail(tr("残基の原子を選択してください。"));
    QJsonObject names;
    for (auto id : ids) {
        auto a = d.atom(id);
        QString name = a.name.isEmpty() ? a.element + QString::number(d.index(id) + 1) : a.name;
        if (names.contains(name))
            fail(tr("原子名が重複しています。"));
        names[name] = id;
    }
    QString rid = newId(), chain = newId();
    updateResidue(d, {{"id", rid},
                      {"code", code},
                      {"atoms", names},
                      {"chain", chain},
                      {"number", 1},
                      {"insertion_code", ""},
                      {"chirality", "L"},
                      {"state", "default"},
                      {"n_terminal", "neutral"},
                      {"c_terminal", "neutral"},
                      {"matched", false},
                      {"modified", false},
                      {"metadata", QJsonObject()}});
    auto chains = d.data["chains"].toObject();
    chains[chain] = QJsonArray{rid};
    d.data["chains"] = chains;
}
void reconnectResidues(Document &d, const QString &left, const QString &right) {
    auto a = residue(d, left), b = residue(d, right);
    if (a["chain"] == b["chain"] || a["c_terminal"] == "internal" || b["n_terminal"] == "internal")
        fail(tr("異なる鎖の空いたC末端、N末端を選んでください。"));
    double distance = (d.atom(named(a, "C")).position - d.atom(named(b, "N")).position).norm();
    if (distance < 1.1 || distance > 1.7)
        fail(tr("C–N距離が %1 Å です。1.1〜1.7 Åに整えてから連結してください（C末端のCとN末端のNを順に選び、"
                "距離に 1.33 を入れて「寸法を適用」すると、別々の鎖なので後に選んだ側の鎖が動きます）。")
                 .arg(distance, 0, 'f', 2));
    setTerminals(d, left, {}, "internal");
    setTerminals(d, right, "internal", {});
    d.setBond(named(a, "C"), named(b, "N"), "single");
    auto chains = d.data["chains"].toObject();
    auto seq = idsFromJson(chains[a["chain"].toString()]);
    auto tail = idsFromJson(chains[b["chain"].toString()]);
    seq += tail;
    chains.remove(b["chain"].toString());
    chains[a["chain"].toString()] = idsToJson(seq);
    d.data["chains"] = chains;
    for (auto id : tail) {
        auto rr = residue(d, id);
        rr["chain"] = a["chain"];
        updateResidue(d, rr);
    }
    orientAmideH(d, right, named(a, "C"));
}
QStringList residueDiagnostics(const Document &d, const QString &rid) {
    auto r = residue(d, rid);
    QStringList result;
    QJsonObject t;
    try {
        t = aminoTemplate(r["code"].toString(), r["state"].toString("default"), r["chirality"].toString("L"));
    } catch (const std::exception &) {
        return {tr("テンプレートがない残基です。手動で確認してください。")};
    }
    auto names = r["atoms"].toObject();
    for (auto v : t["atoms"].toArray()) {
        auto a = v.toObject();
        QString name = a["name"].toString();
        if (a["element"] == "H" || (name == "OXT" && r["c_terminal"] == "internal"))
            continue;
        if (!names.contains(name) || !d.contains(names[name].toString()))
            result << tr("欠損原子: %1").arg(name);
        else if (d.atom(names[name].toString()).element != a["element"].toString())
            result << tr("原子名と元素が一致しません: %1").arg(name);
    }
    for (auto name : {"N", "CA", "C"})
        if (!names.contains(name))
            result << tr("主鎖%1がないため自動補完できません。").arg(name);
    if (result.isEmpty())
        result << tr("主鎖・側鎖の重原子が揃っています。末端状態・水素・電荷を確認してください。");
    return result;
}
void repairResidue(Document &d, const QString &rid) {
    auto r = residue(d, rid);
    QMap<QString, Vec> targets;
    for (auto name : {"N", "CA", "C"})
        targets[name] = d.atom(named(r, name)).position;
    if (r["atoms"].toObject().contains("O"))
        targets["O"] = d.atom(named(r, "O")).position;
    Document temp;
    auto fresh = placeAmino(temp, r["code"].toString(), r["state"].toString("default"),
                            r["chirality"].toString("L"), targets, Vec::Zero());
    auto n = r["n_terminal"].toString("neutral"), c = r["c_terminal"].toString("neutral");
    setTerminals(temp, fresh, n == "ACE" ? "internal" : n, c == "NME" ? "internal" : c);
    QMap<QString, QString> mapping;
    auto names = r["atoms"].toObject();
    Ids heavy;
    for (const auto &a : temp.atoms)
        if (a.element != "H") {
            QString old = names[a.name].toString();
            if (d.contains(old)) {
                if (d.atom(old).element != a.element)
                    fail(tr("原子名と元素の不一致を先に修正してください: %1").arg(a.name));
                mapping[a.id] = old;
            } else {
                auto copy = a;
                copy.id = newId();
                d.atoms << copy;
                names[a.name] = copy.id;
                mapping[a.id] = copy.id;
            }
            heavy << mapping[a.id];
        }
    for (const auto &b : temp.bonds)
        if (mapping.contains(b.a) && mapping.contains(b.b))
            d.setBond(mapping[b.a], mapping[b.b], b.kind);
    r["atoms"] = names;
    r["matched"] = true;
    r["modified"] = false;
    updateResidue(d, r);
    completeHydrogens(d, heavy);
}
} // namespace mf
