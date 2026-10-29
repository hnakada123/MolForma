// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/topology.h"
#include "model/i18n.h"
#include <algorithm>

namespace mf {
Topology::Topology(const Document &d) {
    const int n = d.atoms.size();
    indices.reserve(n);
    for (int i = 0; i < n; ++i)
        if (!indices.contains(d.atoms[i].id)) // As Document::index: the first atom of a repeated ID.
            indices.insert(d.atoms[i].id, i);
    QVector<std::pair<int, int>> ends(d.bonds.size(), {-1, -1});
    offsets.fill(0, n + 1);
    for (int k = 0; k < d.bonds.size(); ++k) {
        const int a = index(d.bonds[k].a), b = index(d.bonds[k].b);
        if (a < 0 || b < 0)
            continue;
        ends[k] = {a, b};
        ++offsets[a + 1];
        ++offsets[b + 1];
    }
    for (int i = 0; i < n; ++i)
        offsets[i + 1] += offsets[i];
    adjacent.resize(offsets[n]);
    bondIndices.resize(offsets[n]);
    auto next = offsets;
    for (int k = 0; k < ends.size(); ++k) {
        const auto [a, b] = ends[k];
        if (a < 0)
            continue;
        adjacent[next[a]] = b;
        bondIndices[next[a]++] = k;
        adjacent[next[b]] = a;
        bondIndices[next[b]++] = k;
    }
}
bool Topology::bonded(int i, int j) const {
    const auto list = neighbors(i);
    return std::find(list.begin(), list.end(), j) != list.end();
}
Atom &Topology::atom(Document &d, const QString &id) const {
    const int i = index(id);
    if (i < 0)
        fail(tr("原子が見つかりません: %1").arg(id));
    return d.atoms[i];
}
const Atom &Topology::atom(const Document &d, const QString &id) const {
    const int i = index(id);
    if (i < 0)
        fail(tr("原子が見つかりません: %1").arg(id));
    return d.atoms[i];
}
Ids Topology::neighbors(const Document &d, const QString &id, bool includeDative) const {
    Ids out;
    const int i = index(id);
    if (i < 0)
        return out;
    const auto list = neighbors(i), through = bonds(i);
    for (size_t k = 0; k < list.size(); ++k)
        if (includeDative || d.bonds[through[k]].kind != "dative")
            out << d.atoms[list[k]].id;
    return out;
}
const Bond *Topology::bond(const Document &d, const QString &a, const QString &b) const {
    const int i = index(a), j = index(b);
    if (i < 0 || j < 0)
        return nullptr;
    const auto list = neighbors(i), through = bonds(i);
    for (size_t k = 0; k < list.size(); ++k)
        if (list[k] == j)
            return &d.bonds[through[k]];
    return nullptr;
}
QVector<int> Topology::component(int root, int cutA, int cutB) const {
    QVector<int> order{root};
    QVector<bool> seen(size());
    seen[root] = true;
    for (qsizetype k = 0; k < order.size(); ++k) {
        const int i = order[k];
        for (int n : neighbors(i))
            if (!seen[n] && !((i == cutA && n == cutB) || (i == cutB && n == cutA))) {
                seen[n] = true;
                order << n;
            }
    }
    return order;
}
QVector<int> Topology::molecules() const {
    QVector<int> molecule(size(), -1);
    int count = 0;
    for (int start = 0; start < size(); ++start) {
        if (molecule[start] >= 0)
            continue;
        QVector<int> todo{start};
        molecule[start] = count;
        while (!todo.isEmpty()) {
            const int i = todo.takeLast();
            for (int n : neighbors(i))
                if (molecule[n] < 0) {
                    molecule[n] = count;
                    todo << n;
                }
        }
        ++count;
    }
    return molecule;
}
} // namespace mf
