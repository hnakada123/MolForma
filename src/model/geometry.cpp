// SPDX-License-Identifier: Apache-2.0
// Derived from KAGAMI tools/kagami_gui (Copyright 2026 Hiroshi Nakada).
// MolForma: translated and adapted to native Qt6/C++ document and editing APIs.
#include "model/geometry.h"
#include <Eigen/Eigenvalues>
#include <Eigen/SVD>
#include <cmath>
#include <numbers>
#include <numeric>
namespace mf {
static constexpr double rad = 3.14159265358979323846 / 180;
Vec unit(const Vec &v) {
    if (v.norm() < 1e-10)
        fail("方向を決められません（重なった原子または直線配置）。");
    return v.normalized();
}
Mat rotation(const Vec &a, double d) {
    return Eigen::AngleAxisd(d * rad, unit(a)).toRotationMatrix();
}
Mat alignVectors(const Vec &a, const Vec &b) {
    return Eigen::Quaterniond::FromTwoVectors(unit(a), unit(b)).toRotationMatrix();
}
std::pair<Mat, Vec> fitPoints(const QVector<Vec> &p, const QVector<Vec> &q) {
    if (p.isEmpty() || p.size() != q.size())
        fail("対応する接続点が必要です。");
    Vec cp = Vec::Zero(), cq = Vec::Zero();
    for (int i = 0; i < p.size(); ++i) {
        cp += p[i];
        cq += q[i];
    }
    cp /= p.size();
    cq /= q.size();
    Mat r = Mat::Identity();
    if (p.size() == 2)
        r = alignVectors(p[1] - p[0], q[1] - q[0]);
    else if (p.size() > 2) {
        Mat h = Mat::Zero();
        for (int i = 0; i < p.size(); ++i)
            h += (p[i] - cp) * (q[i] - cq).transpose();
        Eigen::JacobiSVD<Mat> svd(h, Eigen::ComputeFullU | Eigen::ComputeFullV);
        Mat sign = Mat::Identity();
        sign(2, 2) = (svd.matrixV() * svd.matrixU().transpose()).determinant() < 0 ? -1 : 1;
        r = svd.matrixV() * sign * svd.matrixU().transpose();
    }
    return {r, cq - r * cp};
}
void transform(Document &d, const Ids &ids, const Mat &r, const Vec &shift, const Vec &origin) {
    for (const auto &id : ids) {
        const auto &a = d.atom(id);
        Vec p = origin + r * (a.position - origin) + shift;
        if (a.fixed && (p - a.position).norm() > 1e-7)
            fail("固定原子を動かす操作です。固定を解除してください。");
    }
    for (const auto &id : ids) {
        auto &a = d.atom(id);
        a.position = origin + r * (a.position - origin) + shift;
    }
}
double measure(const Document &d, const Ids &ids) {
    QVector<Vec> p;
    for (const auto &id : ids)
        p << d.atom(id).position;
    if (p.size() == 2)
        return (p[1] - p[0]).norm();
    if (p.size() == 3)
        return std::acos(std::clamp(unit(p[0] - p[1]).dot(unit(p[2] - p[1])), -1., 1.)) / rad;
    if (p.size() == 4) {
        Vec axis = unit(p[2] - p[1]), a = p[0] - p[1], b = p[3] - p[2];
        a -= a.dot(axis) * axis;
        b -= b.dot(axis) * axis;
        unit(a);
        unit(b);
        double angle = std::atan2(axis.cross(a).dot(b), a.dot(b)) / rad;
        return angle <= -180 + 1e-9 ? 180 : angle;
    }
    fail("2〜4原子を順番に選択してください。");
}
// Atoms that move when a measurement changes at `moving`: the far side of the bond, or for
// atoms that are not bonded, the whole molecule containing `moving` (e.g. the second monomer
// of a dimer), so hydrogen-bond and intermolecular distances can be set.
static Ids movingSide(const Document &d, const QString &fixed, const QString &moving) {
    if (d.bond(fixed, moving))
        return d.bondSide(fixed, moving);
    auto side = d.component(moving);
    if (side.contains(fixed))
        fail("同じ分子内で結合していない原子間の寸法は変更できません。結合した原子か、別々の分子の原子を選択"
             "してください。");
    return side;
}
void setDimension(Document &d, const Ids &ids, double value) {
    if (!std::isfinite(value))
        fail("寸法は有限値を指定してください。");
    double current = measure(d, ids);
    if (ids.size() == 2) {
        if (value <= 0)
            fail("距離は正の値にしてください。");
        transform(d, movingSide(d, ids[0], ids[1]), Mat::Identity(),
                  unit(d.atom(ids[1]).position - d.atom(ids[0]).position) * (value - current));
    } else if (ids.size() == 3) {
        if (value <= 0 || value >= 180)
            fail("角度は0°より大きく180°未満にしてください。");
        Vec o = d.atom(ids[1]).position;
        transform(d, movingSide(d, ids[1], ids[2]),
                  rotation((d.atom(ids[2]).position - o).cross(d.atom(ids[0]).position - o), current - value),
                  Vec::Zero(), o);
    } else {
        const auto side = movingSide(d, ids[1], ids[2]);
        if (!side.contains(ids[3]))
            fail("4番目の原子が3番目の原子と同じ分子にないため、二面角を変更できません。");
        Vec a = d.atom(ids[1]).position, b = d.atom(ids[2]).position;
        transform(d, side, rotation(b - a, value - current), Vec::Zero(), b);
    }
}
void orient(Document &d, const Ids &ids) {
    if (ids.isEmpty() || ids.size() > 3)
        fail("原点・+X・XY面の順に1〜3原子を選択してください。");
    Vec o = d.atom(ids[0]).position;
    Mat r = Mat::Identity();
    if (ids.size() > 1)
        r = alignVectors(d.atom(ids[1]).position - o, Vec::UnitX());
    if (ids.size() == 3) {
        Vec p = r * (d.atom(ids[2]).position - o);
        if (std::hypot(p.y(), p.z()) < 1e-9)
            fail("3原子が直線上にあります。");
        r = rotation(Vec::UnitX(), -std::atan2(p.z(), p.y()) / rad) * r;
    }
    transform(d, d.ids(), r, -r * o);
    QJsonArray charges;
    for (auto v : d.data["point_charges"].toArray()) {
        auto p = v.toObject();
        p["position"] = vectorToJson(r * (vectorFromJson(p["position"]) - o));
        charges.append(p);
    }
    d.data["point_charges"] = charges;
}
void centerMass(Document &d, const Ids &selection, bool principal) {
    auto ids = selection.isEmpty() ? d.ids() : selection;
    if (ids.isEmpty())
        fail("分子が空です。");
    Vec center = Vec::Zero();
    double total = 0;
    for (const auto &id : ids) {
        const auto &a = d.atom(id);
        double m = a.mass > 0 ? a.mass : a.isotope ? a.isotope : element(a.element).mass;
        center += m * a.position;
        total += m;
    }
    center /= total;
    Mat r = Mat::Identity();
    if (principal) {
        Mat inertia = Mat::Zero();
        for (const auto &id : ids) {
            const auto &a = d.atom(id);
            Vec p = a.position - center;
            double m = a.mass > 0 ? a.mass : a.isotope ? a.isotope : element(a.element).mass;
            inertia += m * (p.squaredNorm() * Mat::Identity() - p * p.transpose());
        }
        Eigen::SelfAdjointEigenSolver<Mat> e(inertia);
        if (e.info() != Eigen::Success)
            fail("慣性軸を計算できません。");
        r = e.eigenvectors().transpose();
        if (r.determinant() < 0)
            r.row(2) *= -1;
    }
    transform(d, ids, r, -r * center);
}
Vec internalPoint(const Vec &a, const Vec &b, const Vec &c, double length, double angle, double dihedral) {
    Vec bc = unit(c - b), normal = unit((b - a).cross(bc)), tangent = normal.cross(bc);
    return c + length * (-std::cos(angle * rad) * bc +
                         std::sin(angle * rad) *
                             (std::cos(dihedral * rad) * tangent + std::sin(dihedral * rad) * normal));
}
int rotationalSymmetryNumber(const Document &d) {
    QVector<int> z;
    QVector<Vec> p;
    Vec center = Vec::Zero();
    double total = 0;
    for (const auto &a : d.atoms) {
        if (a.ghost)
            continue;
        if (a.isotope || a.mass > 0) // Isotopic substitution can lower the symmetry (HDO).
            return 0;
        const auto &e = element(a.element);
        z << e.number;
        p << a.position;
        center += e.mass * a.position;
        total += e.mass;
    }
    if (p.size() < 2 || p.size() > 60)
        return 0;
    for (auto &r : p)
        r -= center / total;
    constexpr double tolerance = .05;
    // The atom permutation produced by a rotation, or empty if it is not a symmetry operation.
    // Operations are compared by permutation, so nearly parallel candidate axes count once.
    const auto permutation = [&](const Mat &m) {
        QVector<int> image(p.size(), -1);
        for (int i = 0; i < p.size(); ++i) {
            const Vec q = m * p[i];
            for (int j = 0; j < p.size() && image[i] < 0; ++j)
                if (z[j] == z[i] && (p[j] - q).norm() < tolerance)
                    image[i] = j;
            if (image[i] < 0)
                return QVector<int>();
        }
        return image;
    };
    // Linear molecules: sigma is 2 with a center of inversion (D∞h), otherwise 1 (C∞v).
    Vec axis = p[0].norm() > p[1].norm() ? p[0] : p[1];
    bool linear = axis.norm() > tolerance;
    for (const auto &r : p)
        linear = linear && r.cross(axis.normalized()).norm() < tolerance;
    if (linear)
        return permutation(-Mat::Identity()).isEmpty() ? 1 : 2;
    // Every rotation axis passes through the center of mass and through an atom, the midpoint of
    // two equivalent atoms, or is normal to the plane they span with the center.
    QVector<Vec> axes;
    const auto add = [&](const Vec &v) {
        if (v.norm() < tolerance)
            return;
        const Vec u = v.normalized();
        for (const auto &a : axes)
            if (std::abs(a.dot(u)) > 1 - 1e-6)
                return;
        axes << u;
    };
    for (int i = 0; i < p.size(); ++i) {
        add(p[i]);
        for (int j = i + 1; j < p.size(); ++j)
            if (z[i] == z[j] && std::abs(p[i].norm() - p[j].norm()) < tolerance) {
                add(p[i] + p[j]);
                add(p[i].cross(p[j]));
            }
    }
    QVector<int> identity(p.size());
    std::iota(identity.begin(), identity.end(), 0);
    QVector<QVector<int>> group{identity};
    for (const auto &u : axes)
        for (int n = 2; n <= 8; ++n)
            for (int k = 1; k < n; ++k)
                if (auto image =
                        permutation(Eigen::AngleAxisd(2 * std::numbers::pi * k / n, u).toRotationMatrix());
                    !image.isEmpty() && !group.contains(image))
                    group << image;
                else if (k == 1 && image.isEmpty())
                    break;
    // Close the set under composition so tolerance cannot leave a partial group.
    for (int a = 0; a < group.size() && group.size() <= 120; ++a)
        for (int b = 0; b < group.size() && group.size() <= 120; ++b) {
            QVector<int> product(p.size());
            for (int i = 0; i < p.size(); ++i)
                product[i] = group[a][group[b][i]];
            if (!group.contains(product))
                group << product;
        }
    return group.size() <= 120 ? group.size() : 0;
}
Superposition superpose(const Document &moving, const Document &reference) {
    if (moving.atoms.size() != reference.atoms.size() || moving.atoms.isEmpty())
        fail(QString("原子数が異なります（%1 / %2）。同じ原子を同じ順番で含む構造を比較してください。")
                 .arg(moving.atoms.size())
                 .arg(reference.atoms.size()));
    QVector<Vec> from, to;
    for (int i = 0; i < moving.atoms.size(); ++i) {
        if (moving.atoms[i].element != reference.atoms[i].element)
            fail(QString("原子%1の元素が異なります（%2 / %3）。原子の順番が同じ構造を比較してください。")
                     .arg(i + 1)
                     .arg(moving.atoms[i].element, reference.atoms[i].element));
        from << moving.atoms[i].position;
        to << reference.atoms[i].position;
    }
    Superposition s;
    std::tie(s.rotation, s.shift) = fitPoints(from, to);
    double raw = 0, all = 0, heavy = 0;
    int heavyCount = 0;
    for (int i = 0; i < from.size(); ++i) {
        raw += (from[i] - to[i]).squaredNorm();
        const double d = (s.rotation * from[i] + s.shift - to[i]).squaredNorm();
        all += d;
        s.maximum = std::max(s.maximum, std::sqrt(d));
        if (moving.atoms[i].element != "H") {
            heavy += d;
            ++heavyCount;
        }
    }
    s.raw = std::sqrt(raw / from.size());
    s.rmsd = std::sqrt(all / from.size());
    s.heavyRmsd = heavyCount ? std::sqrt(heavy / heavyCount) : 0;
    return s;
}
namespace {
// Rotation axis and order of a proper operation, or the axis of an improper one (the eigenvector
// of eigenvalue -1 for mirrors and rotation-reflections).
std::pair<Vec, int> operationAxis(const Mat &m) {
    const bool proper = m.determinant() > 0;
    const Mat r = proper ? m : Mat(-m);
    Eigen::EigenSolver<Mat> solver(r);
    Vec axis = Vec::UnitZ();
    for (int k = 0; k < 3; ++k)
        if (std::abs(solver.eigenvalues()[k] - std::complex<double>(1, 0)) < 1e-6)
            axis = solver.eigenvectors().col(k).real().normalized();
    const double angle = std::acos(std::clamp((r.trace() - 1) / 2, -1., 1.));
    // For an improper operation -R is a rotation; S_n = σh·C_n has -S_n = C_{2}·... so use the
    // rotation part of m itself: m = σ(axis)·C(θ), with angle θ = π - angle of -m.
    const double theta = proper ? angle : std::numbers::pi - angle;
    const int order = theta < 1e-6 ? 1 : int(std::round(2 * std::numbers::pi / theta));
    return {axis, order};
}
QString groupName(const QVector<SymmetryOperation> &ops, Vec &principal) {
    struct Axis {
        Vec direction;
        int order = 1;
        bool improper = false; // An S_2n (not a mirror or inversion) about it.
    };
    QVector<Axis> axes;
    QVector<Vec> mirrors;
    bool inversion = false;
    const auto axisIndex = [&](const Vec &v) {
        for (int i = 0; i < axes.size(); ++i)
            if (std::abs(axes[i].direction.dot(v)) > 1 - 1e-4)
                return i;
        axes.append({v, 1, false});
        return int(axes.size()) - 1;
    };
    for (const auto &op : ops) {
        const Mat &m = op.matrix;
        if ((m - Mat::Identity()).norm() < 1e-6)
            continue;
        if ((m + Mat::Identity()).norm() < 1e-6) {
            inversion = true;
            continue;
        }
        const auto [axis, order] = operationAxis(m);
        if (m.determinant() > 0) {
            auto &a = axes[axisIndex(axis)];
            a.order = std::max(a.order, order);
        } else if (std::abs(m.trace() - 1) < 1e-6)
            mirrors << axis; // Normal of the mirror plane.
        else
            axes[axisIndex(axis)].improper = true;
    }
    int highest = 1, cubic = 0;
    for (const auto &a : axes) {
        highest = std::max(highest, a.order);
        cubic += a.order >= 3;
    }
    if (cubic > 1) {
        const bool five = std::any_of(axes.begin(), axes.end(), [](const auto &a) { return a.order == 5; });
        const bool four = std::any_of(axes.begin(), axes.end(), [](const auto &a) { return a.order == 4; });
        principal = axes.first().direction;
        if (five)
            return inversion ? "Ih" : "I";
        if (four)
            return inversion ? "Oh" : "O";
        return inversion ? "Th" : mirrors.isEmpty() ? "T" : "Td";
    }
    if (highest < 2) {
        principal = mirrors.isEmpty() ? Vec::UnitZ() : mirrors.first();
        return !mirrors.isEmpty() ? "Cs" : inversion ? "Ci" : "C1";
    }
    // The principal axis: highest order, preferring an S_2n axis, then the most mirrors through it.
    int main = -1, bestScore = -1;
    for (int i = 0; i < axes.size(); ++i) {
        if (axes[i].order != highest)
            continue;
        int score = axes[i].improper ? 1000 : 0;
        for (const auto &normal : mirrors)
            score += std::abs(normal.dot(axes[i].direction)) < 1e-3;
        if (score > bestScore) {
            bestScore = score;
            main = i;
        }
    }
    const Vec z = axes[main].direction;
    principal = z;
    int perpendicular = 0, vertical = 0;
    for (int i = 0; i < axes.size(); ++i)
        perpendicular += i != main && axes[i].order >= 2 && std::abs(axes[i].direction.dot(z)) < 1e-3;
    bool horizontal = false;
    for (const auto &normal : mirrors) {
        horizontal |= std::abs(normal.dot(z)) > 1 - 1e-3;
        vertical += std::abs(normal.dot(z)) < 1e-3;
    }
    const auto n = QString::number(highest);
    if (perpendicular >= highest)
        return "D" + n + (horizontal ? "h" : vertical >= highest ? "d" : "");
    if (horizontal)
        return "C" + n + "h";
    if (vertical >= highest)
        return "C" + n + "v";
    if (axes[main].improper)
        return "S" + QString::number(2 * highest);
    return "C" + n;
}
} // namespace
PointGroup pointGroup(const Document &d, double tolerance) {
    PointGroup group;
    const int n = d.atoms.size();
    if (n == 0 || n > 200 || !(tolerance > 0))
        return group;
    QVector<QString> kind(n);
    QVector<Vec> p(n);
    for (int i = 0; i < n; ++i) {
        const auto &a = d.atoms[i];
        kind[i] = QString("%1%2/%3/%4").arg(a.ghost ? "@" : "", a.element).arg(a.isotope).arg(a.mass);
        group.center += a.position;
    }
    group.center /= n;
    for (int i = 0; i < n; ++i)
        p[i] = d.atoms[i].position - group.center;
    // The atom permutation of an operation, or empty if some atom has no partner within tolerance.
    const auto images = [&](const Mat &m, double &worst) {
        QVector<int> image(n, -1);
        worst = 0;
        for (int i = 0; i < n; ++i) {
            const Vec q = m * p[i];
            double best = tolerance;
            for (int j = 0; j < n; ++j)
                if (kind[j] == kind[i] && (p[j] - q).norm() <= best) {
                    best = (p[j] - q).norm();
                    image[i] = j;
                }
            if (image[i] < 0)
                return QVector<int>();
            worst = std::max(worst, best);
        }
        return image;
    };
    QVector<int> identity(n);
    std::iota(identity.begin(), identity.end(), 0);
    group.operations << SymmetryOperation{Mat::Identity(), identity};
    if (n == 1) {
        group.name = "Kh";
        return group;
    }
    double worst = 0;
    const auto add = [&](const Mat &m) {
        // Within tolerance, products of the found operations only approximate group elements;
        // distinct elements differ by far more (C8 steps: about 1.1 in this norm).
        for (const auto &op : group.operations)
            if ((op.matrix - m).norm() < .3)
                return true;
        double error;
        const auto image = images(m, error);
        if (image.isEmpty())
            return false;
        worst = std::max(worst, error);
        group.operations << SymmetryOperation{m, image};
        return true;
    };
    // Linear molecules: all atoms on one line through the centroid.
    Vec far = p[0];
    for (const auto &v : p)
        if (v.norm() > far.norm())
            far = v;
    bool linear = far.norm() > tolerance;
    for (const auto &v : p)
        linear = linear && v.cross(far.normalized()).norm() < tolerance;
    if (linear) {
        group.axis = far.normalized();
        group.name = add(-Mat::Identity()) ? "D∞h" : "C∞v";
        group.deviation = worst;
        return group;
    }
    // Candidate directions: through atoms, midpoints of equivalent pairs, normals of the planes
    // they span, their differences (mirror normals) and the principal axes of the positions.
    QVector<Vec> directions;
    const auto direction = [&](const Vec &v) {
        if (v.norm() < tolerance)
            return;
        const Vec u = v.normalized();
        for (const auto &a : directions)
            if (std::abs(a.dot(u)) > 1 - 1e-6)
                return;
        directions << u;
    };
    Mat spread = Mat::Zero();
    for (const auto &v : p)
        spread += v * v.transpose();
    Eigen::SelfAdjointEigenSolver<Mat> principal(spread);
    for (int k = 0; k < 3; ++k)
        direction(principal.eigenvectors().col(k));
    for (int i = 0; i < n; ++i) {
        direction(p[i]);
        for (int j = i + 1; j < n; ++j)
            if (kind[i] == kind[j] && std::abs(p[i].norm() - p[j].norm()) < 2 * tolerance) {
                direction(p[i] + p[j]);
                direction(p[i].cross(p[j]));
                direction(p[i] - p[j]);
            }
    }
    add(-Mat::Identity());
    for (const auto &u : directions) {
        add(Mat::Identity() - 2 * u * u.transpose());
        for (int order = 2; order <= 8; ++order) {
            const Mat c = Eigen::AngleAxisd(2 * std::numbers::pi / order, u).toRotationMatrix();
            if (!add(c))
                continue;
            for (int k = 2; k < order; ++k)
                add(Eigen::AngleAxisd(2 * std::numbers::pi * k / order, u).toRotationMatrix());
        }
        for (int order : {4, 6, 8, 10, 12})
            add((Mat::Identity() - 2 * u * u.transpose()) *
                Eigen::AngleAxisd(2 * std::numbers::pi / order, u).toRotationMatrix());
    }
    // Close under composition, so tolerance cannot leave a partial group (Ih has 120 operations).
    for (int a = 0; a < group.operations.size() && group.operations.size() <= 240; ++a)
        for (int b = 0; b < group.operations.size() && group.operations.size() <= 240; ++b)
            add(group.operations[a].matrix * group.operations[b].matrix);
    if (group.operations.size() > 240)
        return PointGroup{};
    group.deviation = worst;
    group.name = groupName(group.operations, group.axis);
    return group;
}
double symmetrize(Document &d, const PointGroup &group) {
    if (group.name.isEmpty() || group.operations.isEmpty())
        fail("点群が決まっていません。");
    const int n = d.atoms.size();
    QVector<Vec> next(n, Vec::Zero());
    for (int i = 0; i < n; ++i) {
        for (const auto &op : group.operations) {
            if (op.image.size() != n)
                fail("点群の判定後に分子が変わりました。もう一度判定してください。");
            next[i] += op.matrix.transpose() * (d.atoms[op.image[i]].position - group.center);
        }
        next[i] /= group.operations.size();
        if (group.name.contains("∞")) // Linear: onto the axis.
            next[i] = next[i].dot(group.axis) * group.axis;
        next[i] += group.center;
    }
    double moved = 0;
    for (int i = 0; i < n; ++i) {
        const double distance = (next[i] - d.atoms[i].position).norm();
        if (d.atoms[i].fixed && distance > 1e-7)
            fail(QString("固定原子%1が動くため対称化できません。固定を解除してください。").arg(i + 1));
        moved = std::max(moved, distance);
    }
    for (int i = 0; i < n; ++i)
        d.atoms[i].position = next[i];
    return moved;
}
} // namespace mf
