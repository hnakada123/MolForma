// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_path.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
ResultPathPoint pathPoint(const QJsonObject &p, ResultPathPoint::Role role, const char *number) {
    ResultPathPoint point;
    point.role = role;
    point.number = p[number].toInt();
    point.frame = p["frame"].toInt(-1);
    point.energy = p["energy"].toDouble(qQNaN());
    return point;
}
} // namespace
int ResultPath::highest(PathEnds ends, bool structures) const {
    const int skipped = ends == PathEnds::Excluded ? 1 : 0;
    int best = -1;
    for (int i = skipped; i + skipped < points.size(); ++i) {
        const auto &p = points[i];
        if (std::isnan(p.energy) || (structures && p.frame < 0))
            continue;
        if (best < 0 || p.energy > points[best].energy)
            best = i;
    }
    return best;
}
ResultPath resultPath(const ResultData &data, ResultPathKind kind) {
    using Role = ResultPathPoint::Role;
    ResultPath path;
    if (kind == ResultPathKind::Scan) {
        const auto scan = data.properties.value("scan").toArray();
        if (scan.isEmpty())
            return path;
        path.kind = kind;
        path.plane = scan[0].toObject().contains("value2");
        double lowest = INFINITY;
        for (const auto &value : scan) {
            const auto p = value.toObject();
            auto point = pathPoint(p, Role::Point, "point");
            point.x = p["value"].toDouble();
            point.coordinate = p["coordinate"].toString();
            if (path.plane) {
                point.y = p["value2"].toDouble();
                point.coordinate2 = p["coordinate2"].toString();
            }
            path.points << point;
            lowest = std::min(lowest, point.energy);
        }
        if (std::isfinite(lowest))
            path.reference = lowest;
    } else if (kind == ResultPathKind::Neb) {
        const auto neb = data.properties.value("neb").toObject();
        if (neb.isEmpty())
            return path;
        path.kind = kind;
        const auto list = neb["points"].toArray();
        path.lengths = !list.isEmpty() && list[0].toObject().contains("distance");
        for (int i = 0; i < list.size(); ++i) {
            const auto p = list[i].toObject();
            auto point = pathPoint(p,
                                   i == 0                 ? Role::Start
                                   : i + 1 == list.size() ? Role::End
                                                          : Role::Point,
                                   "bead");
            point.x = path.lengths ? p["distance"].toDouble() : p["bead"].toDouble();
            path.points << point;
        }
        if (!path.points.isEmpty())
            path.reference = path.points.first().energy;
    } else if (kind == ResultPathKind::Irc) {
        const auto irc = data.properties.value("irc").toObject();
        if (irc.isEmpty())
            return path;
        path.kind = kind;
        QVector<ResultPathPoint> backward, forward;
        for (const auto &value : irc["points"].toArray()) {
            const auto p = value.toObject();
            const bool ahead = p["direction"] == "forward";
            auto point = pathPoint(p, ahead ? Role::Forward : Role::Backward, "point");
            point.x = (ahead ? 1 : -1) * p["s"].toDouble();
            (ahead ? forward : backward) << point;
        }
        std::reverse(backward.begin(), backward.end());
        path.points = backward;
        if (irc.contains("ts_energy")) {
            ResultPathPoint point;
            point.role = Role::TransitionState;
            point.frame = irc["ts_frame"].toInt(-1);
            point.energy = path.reference = irc["ts_energy"].toDouble();
            path.points << point;
        }
        path.points += forward;
        if (!std::isfinite(path.reference) && !path.points.isEmpty())
            path.reference = path.points.first().energy;
    }
    return path;
}
ResultPath resultPath(const ResultData &data) {
    for (const auto kind : {ResultPathKind::Scan, ResultPathKind::Neb, ResultPathKind::Irc})
        if (auto path = resultPath(data, kind); path.kind != ResultPathKind::None)
            return path;
    return {};
}
} // namespace mf
