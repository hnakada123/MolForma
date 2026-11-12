// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/i18n.h"
#include "results/result_fields.h"
#include "results/result_parsing.h"
#include "results/results.h"
#include <Eigen/Geometry>
#include <QHash>
#include <QVarLengthArray>
#include <algorithm>
#include <cmath>

namespace mf::result_detail {
// Marching cubes. Each cube's surface comes from where the level crosses its edges: on every face
// the crossings pair into segments (ambiguous faces by the asymptotic decider, so neighbours agree),
// the segments close into loops, and each loop is fanned into triangles. A crossing is computed once
// per grid edge, so adjacent triangles share exact vertices and the surface has no cracks.
void mesh(ResultData &r, const QVector<double> &values, const Vec &origin, const Mat &axes) {
    static const int corners[8][3]{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                                   {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    // Faces as corner cycles, counterclockwise seen from outside the cube.
    static const int faces[6][4]{{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                 {2, 3, 7, 6}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    const auto &g = r.grid;
    const auto index = [&](int a, int b, int c) { return (qint64(a) * g[1] + b) * g[2] + c; };
    // Gradient by central differences (one-sided on the faces of the grid), in Å⁻¹.
    const Mat toWorld = axes.inverse().transpose();
    const auto gradient = [&](const std::array<int, 3> &i) {
        Vec d;
        for (int axis = 0; axis < 3; ++axis) {
            auto lo = i, hi = i;
            lo[axis] = std::max(0, i[axis] - 1);
            hi[axis] = std::min(g[axis] - 1, i[axis] + 1);
            d[axis] = (values[index(hi[0], hi[1], hi[2])] - values[index(lo[0], lo[1], lo[2])]) /
                      std::max(1, hi[axis] - lo[axis]);
        }
        return Vec(toWorld * d);
    };
    for (bool positive : {true, false}) {
        // f >= 0 inside the lobe: v - level for the positive surface, -(v + level) for the negative.
        const double level = r.isovalue;
        const auto f = [&](qint64 i) { return positive ? values[i] - level : -(values[i] + level); };
        QVector<Vec> points, normals;
        QHash<qint64, int> cuts; // Grid edge (lower point * 3 + axis) → vertex.
        const auto vertex = [&](const std::array<int, 3> &a, const std::array<int, 3> &b) {
            int axis = 0;
            while (a[axis] == b[axis])
                ++axis;
            const auto &low = a[axis] < b[axis] ? a : b, &high = a[axis] < b[axis] ? b : a;
            const qint64 key = index(low[0], low[1], low[2]) * 3 + axis;
            if (const auto it = cuts.constFind(key); it != cuts.cend())
                return *it;
            const double fa = f(index(low[0], low[1], low[2])), fb = f(index(high[0], high[1], high[2]));
            const double t = fa / (fa - fb);
            const Vec pa = origin + axes * Vec(low[0], low[1], low[2]),
                      pb = origin + axes * Vec(high[0], high[1], high[2]);
            // Values fall outward from a positive lobe and rise outward from a negative one.
            const Vec ga = gradient(low), gb = gradient(high);
            Vec normal = (ga + (gb - ga) * t) * (positive ? -1 : 1);
            const double length = normal.norm();
            normal = length > 0 && std::isfinite(length) ? Vec(normal / length) : Vec::Zero();
            points << pa + (pb - pa) * t;
            normals << normal;
            cuts.insert(key, int(points.size()) - 1);
            return int(points.size()) - 1;
        };
        for (int x = 0; x < g[0] - 1; ++x)
            for (int y = 0; y < g[1] - 1; ++y)
                for (int z = 0; z < g[2] - 1; ++z) {
                    std::array<std::array<int, 3>, 8> at;
                    std::array<double, 8> v;
                    int inside = 0;
                    for (int i = 0; i < 8; ++i) {
                        at[i] = {x + corners[i][0], y + corners[i][1], z + corners[i][2]};
                        v[i] = f(index(at[i][0], at[i][1], at[i][2]));
                        inside += v[i] >= 0;
                    }
                    if (inside == 0 || inside == 8)
                        continue;
                    // next[edge] = following edge of the loop, edges named by their corners (a * 8 + b).
                    std::array<int, 64> next;
                    next.fill(-1);
                    for (const auto &face : faces) {
                        struct Crossing {
                            int edge;
                            bool exit; // Leaves the inside, going around the face.
                        };
                        std::array<Crossing, 4> crossings;
                        int n = 0;
                        for (int i = 0; i < 4; ++i) {
                            const int a = face[i], b = face[(i + 1) % 4];
                            if ((v[a] >= 0) != (v[b] >= 0))
                                crossings[n++] = {std::min(a, b) * 8 + std::max(a, b), v[a] >= 0};
                        }
                        if (n == 0)
                            continue;
                        // Two inside corners facing each other: the bilinear saddle decides whether
                        // the inside joins across the face (cut off the outside corners) or not.
                        bool joined = true;
                        if (n == 4) {
                            const double a = v[face[0]], b = v[face[1]], c = v[face[2]], d = v[face[3]];
                            const double denominator = a + c - b - d;
                            joined = denominator == 0 || (a * c - b * d) / denominator >= 0;
                        }
                        for (int i = 0; i < n; ++i)
                            if (crossings[i].exit)
                                next[crossings[i].edge] = crossings[(i + (joined ? 1 : n - 1)) % n].edge;
                    }
                    const auto point = [&](int edge) { return vertex(at[edge / 8], at[edge % 8]); };
                    for (int start = 0; start < 64; ++start) {
                        if (next[start] < 0)
                            continue;
                        QVarLengthArray<int, 12> loop;
                        for (int edge = start; next[edge] >= 0;) {
                            loop << point(edge);
                            const int following = next[edge];
                            next[edge] = -1;
                            edge = following;
                        }
                        for (int i = 1; i + 1 < loop.size(); ++i) {
                            const Vec &a = points[loop[0]], &b = points[loop[i + 1]], &c = points[loop[i]];
                            if ((b - a).cross(c - a).squaredNorm() < 1e-24)
                                continue;
                            if (r.surface.size() >= maxTriangles)
                                fail(tr(
                                    "等値面が50万三角形を超えました。等値を大きくするか格子を小さくしてくだ"
                                    "さい。"));
                            r.surface.append({a, b, c, positive, normals[loop[0]], normals[loop[i + 1]],
                                              normals[loop[i]]});
                        }
                    }
                }
    }
}

} // namespace mf::result_detail
