// SPDX-License-Identifier: Apache-2.0
// Derived from KAGAMI tools/kagami_gui (Copyright 2026 Hiroshi Nakada).
// MolForma: translated and adapted to native Qt6/C++ document and editing APIs.
#include "model/geometry.h"
#include <Eigen/Eigenvalues>
#include <Eigen/SVD>
#include <cmath>
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
void setDimension(Document &d, const Ids &ids, double value) {
    if (!std::isfinite(value))
        fail("寸法は有限値を指定してください。");
    double current = measure(d, ids);
    if (ids.size() == 2) {
        if (value <= 0)
            fail("距離は正の値にしてください。");
        transform(d, d.bondSide(ids[0], ids[1]), Mat::Identity(),
                  unit(d.atom(ids[1]).position - d.atom(ids[0]).position) * (value - current));
    } else if (ids.size() == 3) {
        if (value <= 0 || value >= 180)
            fail("角度は0°より大きく180°未満にしてください。");
        Vec o = d.atom(ids[1]).position;
        transform(d, d.bondSide(ids[1], ids[2]),
                  rotation((d.atom(ids[2]).position - o).cross(d.atom(ids[0]).position - o), current - value),
                  Vec::Zero(), o);
    } else {
        Vec a = d.atom(ids[1]).position, b = d.atom(ids[2]).position;
        transform(d, d.bondSide(ids[1], ids[2]), rotation(b - a, value - current), Vec::Zero(), b);
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
} // namespace mf
