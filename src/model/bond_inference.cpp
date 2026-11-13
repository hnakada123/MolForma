// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/document.h"
#include <QHash>

namespace mf {
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
                        if (!a.ghost && !b.ghost && bondDistance(dist, radius(i), radius(j)) &&
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
