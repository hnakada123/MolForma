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
Residue residue(const Document &d, const QString &id) {
    if (const auto *r = d.findResidue(id))
        return *r;
    fail(tr("残基を選択してください。"));
}
bool polymerResidue(const Residue &r) {
    return (r.atoms.contains("N") && r.atoms.contains("CA") && r.atoms.contains("C")) ||
           r.atoms.contains("C1'");
}
QString residueForAtom(const Document &d, const QString &id) {
    for (const auto &r : d.residues)
        for (const auto &a : r.atoms)
            if (a == id)
                return r.id;
    return {};
}
QHash<QString, QString> atomResidues(const Document &d) {
    QHash<QString, QString> residues;
    for (const auto &r : d.residues)
        for (const auto &a : r.atoms)
            if (!residues.contains(a))
                residues.insert(a, r.id);
    return residues;
}
QHash<QString, QString> atomPlaces(const Document &d) {
    QHash<QString, QString> places;
    for (const auto &r : d.residues) {
        const auto code = QString("%1 %2").arg(r.code).arg(r.number);
        for (auto it = r.atoms.begin(); it != r.atoms.end(); ++it)
            if (!places.contains(it.value()))
                places.insert(it.value(), code + " · " + it.key());
    }
    return places;
}
QString atomPlace(const Document &d, const QString &atom) {
    const auto rid = residueForAtom(d, atom);
    if (rid.isEmpty())
        return {};
    const auto r = residue(d, rid);
    for (auto it = r.atoms.begin(); it != r.atoms.end(); ++it)
        if (it.value() == atom)
            return QString("%1 %2 · %3").arg(r.code).arg(r.number).arg(it.key());
    return {};
}
void updateResidue(Document &d, const Residue &r) {
    for (auto &row : d.residues)
        if (row.id == r.id) {
            row = r;
            return;
        }
    d.residues << r;
}
static QString named(const Residue &r, const QString &name) {
    auto id = r.atoms.value(name);
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
    Residue r;
    for (auto it = names.begin(); it != names.end(); ++it)
        r.atoms[it.key()] = part.mapping[it.value()];
    r.code = code;
    r.chain = chain.isEmpty() ? newId() : chain;
    r.number = number;
    r.chirality = code == "GLY" ? "" : chirality;
    r.state = state;
    updateResidue(d, r);
    d.chains[r.chain] << r.id;
    return r.id;
}
static QString addNamedAtom(Document &d, const QString &rid, const QString &parent, const QString &name,
                            const QString &symbol, const Vec &pos) {
    Atom a;
    a.element = symbol;
    a.name = name;
    a.position = pos;
    d.atoms << a;
    d.setBond(parent, a.id, "single");
    auto r = residue(d, rid);
    QString unique = name;
    int i = 1;
    while (r.atoms.contains(unique))
        unique = name + QString::number(++i);
    d.atom(a.id).name = unique;
    r.atoms[unique] = a.id;
    updateResidue(d, r);
    for (auto &p : d.parts)
        if (p.atoms.contains(parent)) {
            p.atoms << a.id;
            break;
        }
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
    for (bool nTerminus : {true, false}) {
        if ((nTerminus ? n : c).isEmpty())
            continue;
        QString kind = nTerminus ? r.nTerminal : r.cTerminal;
        if (kind != "ACE" && kind != "NME")
            continue;
        const auto parent = named(r, kind == "ACE" ? "N" : "C");
        for (auto neighbor : d.neighbors(parent)) {
            auto capId = residueForAtom(d, neighbor);
            if (capId.isEmpty() || capId == rid)
                continue;
            auto cap = residue(d, capId);
            if (cap.code != kind)
                continue;
            d.remove(cap.atoms.values());
        }
    }
    if (!n.isEmpty()) {
        if (!QStringList{"neutral", "charged", "internal"}.contains(n))
            fail(tr("N末端状態が不正です。"));
        QString id = named(r, "N");
        int count = n == "charged" ? 3 : n == "neutral" ? 2 : 1;
        if (r.code == "PRO")
            --count;
        auto hs = hydrogens(d, id);
        while (hs.size() > count)
            d.remove({hs.takeLast()});
        while (hydrogens(d, id).size() < count)
            addH(d, rid, id, "HN" + QString::number(hydrogens(d, id).size() + 1));
        d.atom(id).charge = n == "charged" ? 1 : 0;
        r = residue(d, rid);
        r.nTerminal = n;
        updateResidue(d, r);
    }
    if (!c.isEmpty()) {
        if (!QStringList{"neutral", "charged", "internal"}.contains(c))
            fail(tr("C末端状態が不正です。"));
        r = residue(d, rid);
        QString carbon = named(r, "C"), oxt = r.atoms.value("OXT");
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
        r.cTerminal = c;
        updateResidue(d, r);
    }
}
static void orientAmideH(Document &d, const QString &rid, const QString &carbon);
void addCap(Document &d, const QString &rid, const QString &kind) {
    auto r = residue(d, rid);
    bool ace = kind == "ACE";
    if (!ace && kind != "NME")
        fail(tr("対応するキャップはACEとNMEです。"));
    auto state = ace ? r.nTerminal : r.cTerminal;
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
        d.atom(leaving).position = internalPoint(c, ca, n, 1.01, 121, r.chirality == "D" ? 135 : -135);
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
    QMap<QString, QString> names;
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
    Residue cap;
    cap.code = kind;
    cap.atoms = names;
    cap.chain = r.chain;
    cap.number = r.number + (ace ? -1 : 1);
    cap.chirality = "";
    cap.nTerminal = cap.cTerminal = "internal";
    updateResidue(d, cap);
    r = residue(d, rid);
    (ace ? r.nTerminal : r.cTerminal) = kind;
    updateResidue(d, r);
    auto &seq = d.chains[r.chain];
    const auto at = seq.indexOf(rid);
    if (at < 0)
        fail(tr("鎖の残基参照が不正です。"));
    seq.insert(at + (ace ? 0 : 1), cap.id);
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
    if (!QStringList{"neutral", "charged"}.contains(prev.cTerminal))
        fail(tr("空いているC末端を選択してください。"));
    Vec ca = d.atom(named(prev, "CA")).position, c = d.atom(named(prev, "C")).position,
        o = d.atom(named(prev, "O")).position;
    Vec nn = c + unit(-unit(ca - c) - unit(o - c)) * 1.33, nca = internalPoint(ca, c, nn, 1.46, 121, 180),
        nc = internalPoint(c, nn, nca, 1.52, 111, -135 * sign),
        no = internalPoint(nn, nca, nc, 1.23, 120, -45 * sign);
    result = placeAmino(d, code, state, chirality, {{"N", nn}, {"CA", nca}, {"C", nc}, {"O", no}},
                        Vec::Zero(), prev.chain, prev.number + 1);
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
    if (!QStringList{"neutral", "charged"}.contains(next.nTerminal))
        fail(tr("空いているN末端を選択してください。"));
    Vec n = d.atom(named(next, "N")).position, ca = d.atom(named(next, "CA")).position,
        c = d.atom(named(next, "C")).position;
    double sign = chirality == "D" ? -1 : 1;
    Vec nc = internalPoint(c, ca, n, 1.33, 121, next.chirality == "D" ? 135 : -135),
        nca = internalPoint(ca, n, nc, 1.52, 116, 180), nn = internalPoint(n, nc, nca, 1.46, 111, 135 * sign),
        no = nc + 1.23 * unit(-unit(nca - nc) - unit(n - nc));
    QString rid = placeAmino(d, code, state, chirality, {{"N", nn}, {"CA", nca}, {"C", nc}, {"O", no}},
                             Vec::Zero(), next.chain, next.number - 1);
    auto &seq = d.chains[next.chain];
    seq.removeAll(rid);
    seq.prepend(rid);
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
        // The chain grows in a working document that keeps only the newest residue and the bond to it:
        // the residues before it are finished and move to d in order, so a residue costs the same
        // however long the chain is. Adding a residue reads only the previous one and its C side.
        Document work;
        const auto finish = [&](const QString &kept) {
            QSet<QString> keep;
            if (const auto *r = work.findResidue(kept))
                keep = QSet<QString>(r->atoms.begin(), r->atoms.end());
            const auto moves = [&](const QString &id) { return !keep.contains(id); };
            for (const auto &a : std::as_const(work.atoms))
                if (moves(a.id))
                    d.atoms << a;
            work.atoms.removeIf([&](const Atom &a) { return moves(a.id); });
            const auto finished = [&](const Bond &b) { return moves(b.a) && moves(b.b); };
            for (const auto &b : std::as_const(work.bonds))
                if (finished(b))
                    d.bonds << b;
            work.bonds.removeIf(finished);
            const auto done = [&](const Part &p) {
                return std::all_of(p.atoms.begin(), p.atoms.end(), moves);
            };
            for (const auto &p : std::as_const(work.parts))
                if (done(p))
                    d.parts << p;
            work.parts.removeIf(done);
            for (const auto &r : std::as_const(work.residues))
                if (r.id != kept) {
                    d.residues << r;
                    d.chains[r.chain] << r.id;
                }
            work.residues.removeIf([&](const Residue &r) { return r.id != kept; });
            for (auto &sequence : work.chains)
                sequence.removeIf([&](const QString &id) { return id != kept; });
            work.chains.removeIf([](const auto &it) { return it.value().isEmpty(); });
        };
        for (const auto &code : codes) {
            if (codes.size() == 1) {
                prev = placeAmino(work, code, state, chirality, {}, Vec::Zero());
                setTerminals(work, prev, nt, ct);
            } else
                prev = appendResidue(work, code, prev, state, chirality, nt, ct);
            if (first.isEmpty())
                first = prev;
            finish(prev);
        }
        finish({});
        if (n == "ACE")
            addCap(d, first, "ACE");
        if (c == "NME")
            addCap(d, prev, "NME");
    });
}
void setChainTerminals(Document &d, const QString &rid, const QString &n, const QString &c) {
    const auto chain = d.chains.value(residue(d, rid).chain);
    if (chain.isEmpty())
        fail(tr("鎖情報がありません。"));
    followingCharge(d, [&] {
        setTerminals(d, chain.first(), n, {});
        setTerminals(d, chain.last(), {}, c);
    });
}
static void replaceResidue(Document &d, const QString &rid, const QString &code, const QString &state,
                           const QString &chirality) {
    const auto old = residue(d, rid);
    const auto oldIds = old.atoms.values();
    for (const auto &id : oldIds)
        if (d.atom(id).fixed)
            fail(tr("固定原子を削除できません。"));
    QMap<QString, Vec> targets;
    for (auto name : {"N", "CA", "C", "O"})
        targets[name] = d.atom(named(old, name)).position;
    const auto snapshot = d;
    QString fresh = placeAmino(d, code, state, chirality, targets, Vec::Zero());
    const auto freshPart = d.parts.last().id;
    setTerminals(d, fresh, old.nTerminal == "ACE" ? "internal" : old.nTerminal,
                 old.cTerminal == "NME" ? "internal" : old.cTerminal);
    // The new atoms with an old atom's name take its ID, so that bonds out of the residue, constraints
    // and coordination sites keep referring to them; the old atoms without one are removed.
    auto replacement = residue(d, fresh);
    QHash<QString, QString> preserved;
    for (auto it = replacement.atoms.begin(); it != replacement.atoms.end(); ++it)
        if (old.atoms.contains(it.key()))
            preserved[it.value()] = old.atoms[it.key()];
    const auto kept = QSet<QString>(preserved.begin(), preserved.end());
    Ids lost;
    for (const auto &id : oldIds)
        if (!kept.contains(id))
            lost << id;
    d.remove(lost);
    d.atoms.removeIf([&](const Atom &a) { return kept.contains(a.id); });
    d.bonds.removeIf([&](const Bond &b) { return kept.contains(b.a) || kept.contains(b.b); });
    d.renameIds(preserved);
    for (const auto &b : snapshot.bonds)
        if (oldIds.contains(b.a) != oldIds.contains(b.b)) {
            if (d.contains(b.a) && d.contains(b.b))
                d.setBond(b.a, b.b, b.kind);
            else
                fail(tr("置換で失われる側鎖に外部結合があります。"));
        }
    // The old residue's place takes the new one, in its chain; the new atoms leave the old parts.
    replacement = residue(d, fresh);
    d.residues.removeIf([&](const Residue &r) { return r.id == fresh; });
    d.chains.remove(replacement.chain);
    replacement.id = rid;
    replacement.chain = old.chain;
    replacement.number = old.number;
    replacement.nTerminal = old.nTerminal;
    replacement.cTerminal = old.cTerminal;
    updateResidue(d, replacement);
    for (auto &p : d.parts)
        if (p.id != freshPart) {
            p.atoms.removeIf([&](const QString &id) { return oldIds.contains(id); });
            p.connections.removeIf([&](const QString &id) { return oldIds.contains(id); });
        }
    d.parts.removeIf([](const Part &p) { return p.atoms.isEmpty(); });
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
    if (r.code == "ACE" || r.code == "NME") {
        const bool ace = r.code == "ACE";
        auto connection = named(r, ace ? "C" : "N");
        for (auto neighbor : d.neighbors(connection)) {
            auto parent = residueForAtom(d, neighbor);
            if (!parent.isEmpty() && parent != rid) {
                setTerminals(d, parent, ace ? "neutral" : "", ace ? "" : "neutral");
                return;
            }
        }
    }
    if (r.nTerminal == "ACE")
        setTerminals(d, rid, "neutral", {});
    if (r.cTerminal == "NME")
        setTerminals(d, rid, {}, "neutral");
    r = residue(d, rid);
    const auto chain = r.chain;
    const auto sequence = d.chains.value(chain);
    int index = sequence.indexOf(rid);
    if (!chain.isEmpty() && index < 0)
        fail(tr("鎖の残基参照が不正です。"));
    d.remove(r.atoms.values());
    if (index > 0)
        setTerminals(d, sequence[index - 1], {}, "neutral");
    if (index + 1 < sequence.size()) {
        QString nc = newId();
        d.chains[chain] = sequence.mid(0, index);
        d.chains[nc] = sequence.mid(index + 1);
        for (auto id : sequence.mid(index + 1)) {
            auto rr = residue(d, id);
            rr.chain = nc;
            updateResidue(d, rr);
        }
        setTerminals(d, sequence[index + 1], "neutral", {});
    }
}
void setResidueTorsion(Document &d, const QString &rid, const QString &kind, double angle) {
    auto r = residue(d, rid);
    auto seq = d.chains.value(r.chain);
    int i = seq.indexOf(rid);
    if (!r.chain.isEmpty() && i < 0)
        fail(tr("鎖の残基参照が不正です。"));
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
        const auto &map = r.atoms;
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
    Residue r;
    for (auto id : ids) {
        auto a = d.atom(id);
        QString name = a.name.isEmpty() ? a.element + QString::number(d.index(id) + 1) : a.name;
        if (r.atoms.contains(name))
            fail(tr("原子名が重複しています。"));
        r.atoms[name] = id;
    }
    r.code = code;
    r.chain = newId();
    r.number = 1;
    r.matched = false;
    updateResidue(d, r);
    d.chains[r.chain] = {r.id};
}
void reconnectResidues(Document &d, const QString &left, const QString &right) {
    auto a = residue(d, left), b = residue(d, right);
    if (a.chain == b.chain || a.cTerminal == "internal" || b.nTerminal == "internal")
        fail(tr("異なる鎖の空いたC末端、N末端を選んでください。"));
    double distance = (d.atom(named(a, "C")).position - d.atom(named(b, "N")).position).norm();
    if (distance < 1.1 || distance > 1.7)
        fail(tr("C–N距離が %1 Å です。1.1〜1.7 Åに整えてから連結してください（C末端のCとN末端のNを順に選び、"
                "距離に 1.33 を入れて「寸法を適用」すると、別々の鎖なので後に選んだ側の鎖が動きます）。")
                 .arg(distance, 0, 'f', 2));
    setTerminals(d, left, {}, "internal");
    setTerminals(d, right, "internal", {});
    d.setBond(named(a, "C"), named(b, "N"), "single");
    const auto tail = d.chains.take(b.chain);
    d.chains[a.chain] += tail;
    for (auto id : tail) {
        auto rr = residue(d, id);
        rr.chain = a.chain;
        updateResidue(d, rr);
    }
    orientAmideH(d, right, named(a, "C"));
}
QStringList residueDiagnostics(const Document &d, const QString &rid) {
    auto r = residue(d, rid);
    QStringList result;
    QJsonObject t;
    try {
        t = aminoTemplate(r.code, r.state, r.chirality);
    } catch (const std::exception &) {
        return {tr("テンプレートがない残基です。手動で確認してください。")};
    }
    const auto &names = r.atoms;
    for (auto v : t["atoms"].toArray()) {
        auto a = v.toObject();
        QString name = a["name"].toString();
        if (a["element"] == "H" || (name == "OXT" && r.cTerminal == "internal"))
            continue;
        if (!names.contains(name) || !d.contains(names[name]))
            result << tr("欠損原子: %1").arg(name);
        else if (d.atom(names[name]).element != a["element"].toString())
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
    if (r.atoms.contains("O"))
        targets["O"] = d.atom(named(r, "O")).position;
    Document temp;
    auto fresh = placeAmino(temp, r.code, r.state, r.chirality, targets, Vec::Zero());
    const auto n = r.nTerminal, c = r.cTerminal;
    setTerminals(temp, fresh, n == "ACE" ? "internal" : n, c == "NME" ? "internal" : c);
    QMap<QString, QString> mapping;
    auto &names = r.atoms;
    Ids heavy;
    for (const auto &a : temp.atoms)
        if (a.element != "H") {
            QString old = names.value(a.name);
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
    r.matched = true;
    r.modified = false;
    updateResidue(d, r);
    completeHydrogens(d, heavy);
}
} // namespace mf
