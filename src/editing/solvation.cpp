// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/solvation.h"
#include "editing/library.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include <QHash>
#include <algorithm>
#include <cmath>
#include <limits>
#include <openbabel/atom.h>
#include <optional>

namespace mf {
namespace {
constexpr double gridSize = 3.6; // At least the largest contact distance.
bool metal(const QString &symbol) {
    OpenBabel::OBAtom atom;
    atom.SetAtomicNum(element(symbol).number);
    return atom.IsMetal();
}
// Evenly spread unit vectors (a Fibonacci sphere).
Vec spread(int i, int n) {
    const double z = 1 - (2 * i + 1.0) / n, r = std::sqrt(std::max(0.0, 1 - z * z));
    const double phi = i * M_PI * (3 - std::sqrt(5.0));
    return Vec(r * std::cos(phi), r * std::sin(phi), z);
}
// Atoms looked up by place: the atoms within gridSize of a point are in its cell and the 26 around it.
class Grid {
  public:
    void add(const Vec &p, int index) {
        cells[key(p)] << index;
    }
    template <typename F> void near(const Vec &p, F &&visit) const {
        const auto c = cell(p);
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z)
                    for (int i : cells.value(pack(c[0] + x, c[1] + y, c[2] + z)))
                        visit(i);
    }

  private:
    QHash<qint64, QVector<int>> cells;
    static std::array<qint64, 3> cell(const Vec &p) {
        return {qint64(std::floor(p.x() / gridSize)), qint64(std::floor(p.y() / gridSize)),
                qint64(std::floor(p.z() / gridSize))};
    }
    static qint64 pack(qint64 x, qint64 y, qint64 z) {
        return ((x + 1000000) << 42) | ((y + 1000000) << 21) | (z + 1000000);
    }
    static qint64 key(const Vec &p) {
        const auto c = cell(p);
        return pack(c[0], c[1], c[2]);
    }
};
struct Site {
    Vec position;
    bool hydrogen = false;
    double contact = 0; // How close the solvent's donor atom comes (Å).
    int priority = 0;   // Ions first, then polar atoms.
    bool anion = false;
};
struct Candidate {
    Vec point, normal; // Where the donor atom goes, and the way out from the atom it touches.
    int priority = 0;
    bool flip = false; // Its hydrogens face the atom (an anion).
    int site = -1;     // The atom of the solute it touches; -1 around solvent already placed.
};
constexpr double solventContact = 2.55; // The least distance between heavy atoms of two solvent molecules.
// Candidates taken by priority (ions first), each the farthest from the points taken so far; the nearest
// distances are kept up to date as points are taken.
class Picker {
  public:
    explicit Picker(const QVector<Candidate> &candidates)
        : pool(candidates), nearest(candidates.size(), std::numeric_limits<double>::infinity()),
          used(candidates.size(), false) {}
    void take(const Vec &p) {
        for (int i = 0; i < pool.size(); ++i)
            if (!used[i])
                nearest[i] = std::min(nearest[i], (pool[i].point - p).norm());
    }
    // The next one no closer than least to a point taken. Once the farthest of a priority is closer, all of
    // that priority are, and they are left.
    std::optional<Candidate> next(double least) {
        for (;;) {
            int best = -1;
            for (int i = 0; i < pool.size(); ++i)
                if (!used[i] &&
                    (best < 0 || pool[i].priority > pool[best].priority ||
                     (pool[i].priority == pool[best].priority && nearest[i] > nearest[best] + 1e-9)))
                    best = i;
            if (best < 0)
                return std::nullopt;
            if (nearest[best] >= least - 1e-9) {
                used[best] = true;
                return pool[best];
            }
            for (int i = 0; i < pool.size(); ++i)
                used[i] = used[i] || pool[i].priority == pool[best].priority;
        }
    }

  private:
    QVector<Candidate> pool;
    QVector<double> nearest;
    QVector<bool> used;
};
} // namespace
QStringList solventTemplates() {
    return {"skeleton:water", "skeleton:methanol", "ligand:acetonitrile", "skeleton:dmso",
            "skeleton:ammonia"};
}
int solvate(Document &d, const QJsonObject &t, const SolvationOptions &o) {
    validateTemplate(t);
    if (o.count < 1 || o.count > 1000)
        fail(tr("溶媒分子の数は1〜1000で指定してください。"));
    if (d.atoms.isEmpty())
        fail(tr("分子が空です。"));
    QVector<Vec> shape;
    QVector<QString> kinds;
    for (const auto &value : t["atoms"].toArray()) {
        shape << vectorFromJson(value.toObject()["position"]);
        kinds << value.toObject()["element"].toString();
    }
    // The donor atom: the most electronegative heavy atom (O of water and DMSO, N of acetonitrile).
    static const QHash<QString, int> donors{{"O", 5}, {"N", 4}, {"F", 3}, {"S", 2}, {"Cl", 2}, {"C", 1}};
    int donor = -1;
    for (int i = 0; i < kinds.size(); ++i)
        if (kinds[i] != "H" && (donor < 0 || donors.value(kinds[i]) > donors.value(kinds[donor])))
            donor = i;
    if (donor < 0)
        fail(tr("溶媒に水素以外の原子がありません。"));
    Vec centroid = Vec::Zero();
    for (const auto &p : shape)
        centroid += p;
    centroid /= shape.size();
    const Vec outward =
        (centroid - shape[donor]).norm() > 1e-6 ? unit(centroid - shape[donor]) : Vec::UnitZ();
    // Toward an anion a protic solvent (water, methanol, ammonia) points one bond of its donor to hydrogen,
    // as in a hydrogen bond; others turn their other end to it.
    std::optional<Vec> protic;
    for (int i = 0; i < kinds.size(); ++i)
        if (kinds[i] == "H" && (shape[i] - shape[donor]).norm() < 1.25 && !protic)
            protic = unit(shape[i] - shape[donor]);
    const double donorRadius = element(kinds[donor]).radius;
    // The solute: every atom keeps its distance; the chosen ones (or all) are those the solvent gathers at.
    QVector<Site> sites;
    Grid grid;
    const QSet<QString> around(o.around.begin(), o.around.end());
    QVector<int> centers;
    Vec middle = Vec::Zero();
    for (const auto &a : d.atoms) {
        Site s;
        s.position = a.position;
        s.hydrogen = a.element == "H";
        if (metal(a.element)) {
            s.contact = element(a.element).radius + donorRadius + .15;
            s.priority = 3;
        } else if (a.charge < 0) {
            s.contact = 3.1;
            s.priority = 3;
            s.anion = true;
        } else if (a.element == "N" || a.element == "O" || a.element == "F") {
            s.contact = 2.85;
            s.priority = 2;
        } else
            s.contact = s.hydrogen ? 2.4 : 3.4;
        s.priority = s.priority ? s.priority : 1;
        grid.add(s.position, int(sites.size()));
        if (around.isEmpty() || around.contains(a.id)) {
            centers << int(sites.size());
            middle += a.position;
        }
        sites << s;
    }
    if (centers.isEmpty())
        fail(tr("溶媒和する原子を選択してください。"));
    middle /= centers.size();
    // Hydrogens gather solvent only where they are all there is (H2, a hydride).
    QVector<int> heavy;
    for (int i : centers)
        if (!sites[i].hydrogen)
            heavy << i;
    if (!heavy.isEmpty())
        centers = heavy;
    // Not closer to any atom of the solute than its contact (the donor atom), 2.6 Å (other heavy atoms) or
    // 1.75 Å (hydrogens: hydrogen bonds come as close as 1.8 Å).
    const auto clearOfSolute = [&](const Vec &p, bool hydrogen, bool isDonor) {
        bool clear = true;
        grid.near(p, [&](int i) {
            const double r = (sites[i].position - p).norm();
            const double least = hydrogen || sites[i].hydrogen ? (hydrogen && sites[i].hydrogen ? 1.8 : 1.75)
                                 : isDonor                     ? .95 * sites[i].contact
                                                               : std::max(2.6, .9 * sites[i].contact);
            clear = clear && r >= least - 1e-9;
        });
        return clear;
    };
    QVector<Vec> placed;          // The solvent's atoms so far.
    QVector<bool> placedHydrogen; //
    QVector<Vec> placedDonors;    // Their donor atoms.
    Grid placedGrid;
    const auto clearOfSolvent = [&](const Vec &p, bool hydrogen) {
        bool clear = true;
        placedGrid.near(p, [&](int i) {
            const double least = hydrogen || placedHydrogen[i] ? 1.6 : solventContact;
            clear = clear && (placed[i] - p).norm() >= least - 1e-9;
        });
        return clear;
    };
    // The molecule with its donor atom at the point and the rest along the normal (or against it, for an
    // anion), turned about it to the first orientation that touches nothing.
    const auto place = [&](const Candidate &c) -> std::optional<QVector<Vec>> {
        const Vec w = c.flip ? -c.normal : c.normal;
        const Mat align = alignVectors(c.flip && protic ? *protic : outward, w);
        for (int step = 0; step < 12; ++step) {
            const Mat r = rotation(w, 30.0 * step) * align;
            QVector<Vec> positions;
            bool clear = true;
            for (int i = 0; i < shape.size() && clear; ++i) {
                positions << c.point + r * (shape[i] - shape[donor]);
                clear = clearOfSolute(positions.last(), kinds[i] == "H", i == donor) &&
                        clearOfSolvent(positions.last(), kinds[i] == "H");
            }
            if (clear)
                return positions;
        }
        return std::nullopt;
    };
    // The first shell: points at contact around the chosen atoms, fewer per atom for large molecules.
    QVector<Candidate> candidates;
    const int points = std::clamp(4000 / int(centers.size()), 12, 96);
    for (int i : centers)
        for (int k = 0; k < points; ++k) {
            const Vec normal = spread(k, points);
            const Vec p = sites[i].position + sites[i].contact * normal;
            bool open = true;
            grid.near(p, [&](int j) {
                if (j != i && !sites[j].hydrogen && (sites[j].position - p).norm() < .95 * sites[j].contact)
                    open = false;
            });
            if (open)
                candidates << Candidate{p, normal, sites[i].priority, sites[i].anion, i};
        }
    // The first shell planned by its donor atoms: picked one by one, then spread evenly (they push one
    // another apart over the contact sphere of the atom each touches; picking alone leaves them uneven,
    // five around a sodium ion instead of an octahedron).
    QVector<Candidate> plan;
    Picker planning(candidates);
    while (plan.size() < o.count)
        if (const auto c = planning.next(solventContact)) {
            plan << *c;
            planning.take(c->point);
        } else
            break;
    double step = .3;
    for (int iteration = 0; iteration < 300 && plan.size() > 1; ++iteration, step *= .98) {
        QVector<Vec> force(plan.size(), Vec::Zero());
        for (int i = 0; i < plan.size(); ++i)
            for (int j = i + 1; j < plan.size(); ++j) {
                const Vec r = plan[i].point - plan[j].point;
                const double distance = r.norm();
                if (distance < 1e-9 || distance > 12)
                    continue;
                const Vec f = r / std::pow(distance, 8); // The gradient of 1/r^6.
                force[i] += f;
                force[j] -= f;
            }
        double largest = 0;
        for (int i = 0; i < plan.size(); ++i) {
            force[i] -= force[i].dot(plan[i].normal) * plan[i].normal;
            largest = std::max(largest, force[i].norm());
        }
        if (largest < 1e-15)
            break;
        for (int i = 0; i < plan.size(); ++i) {
            const auto &site = sites[plan[i].site];
            const Vec normal = unit(plan[i].point + step / largest * force[i] - site.position);
            const Vec p = site.position + site.contact * normal;
            if (clearOfSolute(p, false, true)) {
                plan[i].point = p;
                plan[i].normal = normal;
            }
        }
    }
    int count = 0;
    Picker rest(candidates); // The first shell where planned molecules did not fit.
    const auto add = [&](const QVector<Vec> &positions) {
        for (int i = 0; i < positions.size(); ++i) {
            placedGrid.add(positions[i], int(placed.size()));
            placed << positions[i];
            placedHydrogen << (kinds[i] == "H");
        }
        placedDonors << positions[donor];
        rest.take(positions[donor]);
        instantiate(d, t, positions);
        ++count;
    };
    for (const auto &c : std::as_const(plan))
        if (const auto positions = place(c))
            add(*positions);
    while (count < o.count)
        if (const auto c = rest.next(solventContact)) {
            if (const auto positions = place(*c))
                add(*positions);
        } else
            break;
    // Then layer by layer outward, around the solvent placed so far, the points nearest the solute first.
    for (int layer = 1; count < o.count && layer < 6; ++layer) {
        candidates.clear();
        for (const auto &centre : std::as_const(placedDonors))
            for (int k = 0; k < 24; ++k) {
                const Vec normal = spread(k, 24), p = centre + 2.8 * normal;
                candidates << Candidate{p, normal, 0, false, -1};
            }
        std::stable_sort(candidates.begin(), candidates.end(), [&](const Candidate &a, const Candidate &b) {
            return (a.point - middle).norm() < (b.point - middle).norm();
        });
        for (int i = 0; i < candidates.size() && count < o.count; ++i)
            if (const auto positions = place(candidates[i]))
                add(*positions);
    }
    if (!count)
        fail(tr("溶媒分子を置ける場所がありません。"));
    return count;
}
} // namespace mf
