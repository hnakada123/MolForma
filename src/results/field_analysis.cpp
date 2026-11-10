// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/field_analysis.h"
#include "model/i18n.h"
#include <Eigen/Eigenvalues>
#include <Eigen/LU>
#include <cmath>
namespace mf {
std::optional<double> sampleField(const ResultData &f, const Vec &p) {
    if (f.values.isEmpty() || !p.allFinite() || std::abs(f.gridAxes.determinant()) < 1e-15)
        return {};
    const Vec q = f.gridAxes.inverse() * (p - f.gridOrigin);
    std::array<int, 3> lo;
    Vec weight;
    for (int k = 0; k < 3; ++k) {
        if (f.grid[k] < 2 || q[k] < -1e-8 || q[k] > f.grid[k] - 1 + 1e-8)
            return {};
        const double x = std::clamp(q[k], 0., double(f.grid[k] - 1));
        lo[k] = std::min(int(std::floor(x)), f.grid[k] - 2);
        weight[k] = x - lo[k];
    }
    double result = 0;
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) {
                const auto i = (qsizetype(lo[0] + x) * f.grid[1] + lo[1] + y) * f.grid[2] + lo[2] + z;
                if (i >= f.values.size())
                    return {};
                result += f.values[i] * (x ? weight[0] : 1 - weight[0]) * (y ? weight[1] : 1 - weight[1]) *
                          (z ? weight[2] : 1 - weight[2]);
            }
    return result;
}
void validateFieldOverlay(const ResultData &surface, const ResultData &field) {
    if (surface.frames.isEmpty() || field.frames.isEmpty() || field.values.isEmpty())
        fail(tr("色付けには原子座標とCUBE格子が必要です。"));
    const auto &a = surface.frames.first().molecule.atoms, &b = field.frames.first().molecule.atoms;
    if (a.size() != b.size())
        fail(tr("二つのCUBEの原子配置・座標系が一致しません。"));
    for (int i = 0; i < a.size(); ++i)
        if (a[i].element != b[i].element || (a[i].position - b[i].position).norm() > 1e-4)
            fail(tr("二つのCUBEの原子配置・座標系が一致しません。"));
    for (const auto &t : surface.surface)
        for (const auto &p : {t.a, t.b, t.c})
            if (!sampleField(field, p))
                fail(tr("色付け用の格子が等値面全体を覆っていません。"));
}
PrincipalTensor polarizabilityAxes(const QJsonArray &a) {
    if (a.size() != 9)
        fail(tr("完全な分極率テンソルが必要です。"));
    Mat m;
    for (int i = 0; i < 9; ++i) {
        if (!a[i].isDouble() || !std::isfinite(a[i].toDouble()))
            fail(tr("完全な分極率テンソルが必要です。"));
        m(i / 3, i % 3) = a[i].toDouble();
    }
    if ((m - m.transpose()).norm() > 1e-4 * std::max(1., m.norm()))
        fail(tr("分極率テンソルが対称でないため主軸を表示できません。"));
    Eigen::SelfAdjointEigenSolver<Mat> solver((m + m.transpose()) / 2);
    if (solver.info() != Eigen::Success || solver.eigenvalues().minCoeff() <= 0)
        fail(tr("楕円体には正の分極率主値が必要です。"));
    return {solver.eigenvalues(), solver.eigenvectors()};
}
} // namespace mf
