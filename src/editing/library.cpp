// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/library.h"
#include "model/i18n.h"
namespace mf {
const QJsonArray &templates() {
    elements();
    static QJsonArray t = parseJson(readFile(":/data/library.json"))["templates"].toArray();
    return t;
}
QJsonObject findTemplate(const QString &id) {
    for (auto v : templates())
        if (v.toObject()["id"] == id)
            return v.toObject();
    fail(tr("部品が見つかりません: %1").arg(id));
}
QString templateName(const QJsonObject &t) {
    const auto name = t[english() ? "name_en" : "name_ja"].toString();
    return name.isEmpty() ? t["name_ja"].toString() : name;
}
void validateTemplate(const QJsonObject &t) {
    auto aa = t["atoms"].toArray();
    if (aa.isEmpty() || t["id"].toString().isEmpty())
        fail(tr("部品に原子と名前が必要です。"));
    auto check = [&](QJsonValue v) {
        if (!v.isDouble() || v.toDouble() != v.toInt() || v.toInt() < 0 || v.toInt() >= aa.size())
            fail(tr("部品の原子参照が不正です。"));
    };
    for (auto a : aa)
        Atom::fromJson(a.toObject());
    for (auto v : t["bonds"].toArray()) {
        auto b = v.toObject();
        check(b["a"]);
        check(b["b"]);
    }
    for (auto v : t["connections"].toArray())
        check(v);
    if (!t["leaving"].isNull() && !t["leaving"].isUndefined())
        check(t["leaving"]);
}
PlacedPart instantiate(Document &d, const QJsonObject &t, const QVector<Vec> &positions,
                       const QSet<int> &omit) {
    validateTemplate(t);
    auto aa = t["atoms"].toArray();
    if (!positions.isEmpty() && positions.size() != aa.size())
        fail(tr("部品の座標数が不正です。"));
    PlacedPart p;
    p.id = newId();
    for (int i = 0; i < aa.size(); ++i)
        if (!omit.contains(i)) {
            Atom a = Atom::fromJson(aa[i].toObject());
            a.extra = QJsonObject{
                {"metadata", QJsonObject{{"chiral_tag", aa[i].toObject()["chiral_tag"].toInt()}}}};
            a.id = newId();
            if (!positions.isEmpty())
                a.position = positions[i];
            d.atoms << a;
            p.atoms << a.id;
            p.mapping[i] = a.id;
        }
    for (auto v : t["bonds"].toArray()) {
        auto b = v.toObject();
        int a = b["a"].toInt(), c = b["b"].toInt();
        if (p.mapping.contains(a) && p.mapping.contains(c))
            d.bonds.append({p.mapping[a], p.mapping[c], b["kind"].toString("single"), b["stereo"].toString(),
                            "template"});
    }
    for (auto v : t["connections"].toArray())
        if (p.mapping.contains(v.toInt()))
            p.connections << p.mapping[v.toInt()];
    QJsonObject meta{{"name_ja", t["name_ja"]}, {"name_en", t["name_en"]}, {"category", t["category"]}};
    if (t["id"].toString().startsWith("my:"))
        meta["template_data"] = t;
    auto parts = d.data["parts"].toArray();
    parts.append(QJsonObject{{"id", p.id},
                             {"template", t["id"]},
                             {"atoms", idsToJson(p.atoms)},
                             {"connections", idsToJson(p.connections)},
                             {"metadata", meta}});
    d.data["parts"] = parts;
    d.invalidateAssignments();
    return p;
}
PlacedPart placeTemplate(Document &d, const QJsonObject &t, const Vec &position) {
    QVector<Vec> pp;
    Vec center = Vec::Zero();
    for (auto a : t["atoms"].toArray()) {
        pp << vectorFromJson(a.toObject()["position"]);
        center += pp.last();
    }
    if (pp.isEmpty())
        fail(tr("空の部品です。"));
    center /= pp.size();
    for (auto &p : pp)
        p += position - center;
    return instantiate(d, t, pp);
}
QJsonObject makePart(const Document &d, const Ids &ids, const QString &name, const QString &category) {
    const bool ligand = category == "ligand", skeleton = category == "skeleton";
    if (!ligand && !skeleton && category != "substituent")
        fail(tr("部品の種類が不正です。"));
    if (ids.isEmpty() || name.trimmed().isEmpty())
        fail(tr("原子を接続点から順に選び、部品名を入力してください。"));
    QSet<QString> selected(ids.begin(), ids.end());
    int boundary = 0;
    for (const auto &b : d.bonds)
        if (selected.contains(b.a) != selected.contains(b.b)) {
            ++boundary;
            if (skeleton)
                fail(
                    tr("骨格は他の原子と結合していない分子全体を選択してください（選択単位「連結分子」）。"));
            if ((selected.contains(b.a) ? b.a : b.b) != ids.first())
                fail(tr("最初に接続点の原子を選択してください。"));
        }
    if (!skeleton && (boundary > 1 || (!ligand && boundary != 1)))
        fail(tr("置換基の外部結合は1本、配位子は0または1本にしてください。"));
    Document sub;
    for (const auto &id : ids)
        sub.atoms << d.atom(id);
    QJsonArray bonds, atoms;
    for (const auto &b : d.bonds)
        if (selected.contains(b.a) && selected.contains(b.b)) {
            sub.bonds << b;
            bonds.append(QJsonObject{
                {"a", ids.indexOf(b.a)}, {"b", ids.indexOf(b.b)}, {"kind", b.kind}, {"stereo", b.stereo}});
        }
    if (!skeleton && sub.component(ids.first()).size() != ids.size())
        fail(tr("部品の原子は互いに結合している必要があります。"));
    for (const auto &id : ids) {
        auto a = d.atom(id).toJson();
        a.remove("id");
        atoms.append(a);
    }
    return {{"id", "my:" + name},     {"name_ja", name},
            {"name_en", name},        {"aliases", QJsonArray{name}},
            {"category", category},   {"atoms", atoms},
            {"bonds", bonds},         {"connections", skeleton ? QJsonArray() : QJsonArray{0}},
            {"leaving", QJsonValue()}};
}
} // namespace mf
