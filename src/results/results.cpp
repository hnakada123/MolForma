// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/results.h"
#include "foundation/i18n.h"
#include "model/numbers.h"
#include "results/nwchem_log.h"
#include "results/result_cache.h"
#include "results/result_fields.h"
#include "results/result_parsing.h"
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace mf {
using namespace result_detail;
ResultData readResult(const QString &path, double iso, int dataset, int orbital, const QString &normalization,
                      int orbitalDivisions, const QString &cacheFolder) {
    if (!std::isfinite(iso) || iso <= 0 || iso > 1e6)
        fail(tr("等値は0より大きい有限の値で指定してください。"));
    const auto suffix = QFileInfo(path).suffix().toLower();
    const ResultCache cache(cacheFolder, path);
    // A CUBE or MOLDEN file read before in this session: only its surface is made again.
    if (cache && orbitalDivisions >= 16 && orbitalDivisions <= 100) {
        const auto again = [&](ResultData r) {
            r.isovalue = iso;
            r.dataset = dataset;
            r.orbitalDivisions = orbitalDivisions;
            r.normalizationRequest = normalization;
            return r;
        };
        if (suffix == "cube" || suffix == "cub") {
            if (const auto hit = cache.cube(dataset)) {
                auto r = again(hit->first);
                cubeSurface(r, hit->second);
                return r;
            }
        } else if (const auto hit = cache.molden()) {
            auto r = again(hit->first);
            moldenSurface(r, hit->second, orbital, cache);
            return r;
        }
    }
    const auto bytes = boundedRead(path);
    const auto text = QString::fromUtf8(bytes);
    ResultData r;
    r.source = QFileInfo(path).absoluteFilePath();
    r.isovalue = iso;
    r.dataset = dataset;
    if (orbitalDivisions < 16 || orbitalDivisions > 100)
        fail(tr("MOLDENの格子分割数は16〜100を指定してください。"));
    r.orbitalDivisions = orbitalDivisions;
    r.normalizationRequest = normalization;
    r.log = QString::fromUtf8(bytes.right(128 * 1024));
    if (bytes.size() > 128 * 1024)
        r.log.prepend(tr("…末尾128 KiBを表示しています。\n"));
    QStringList lines; // CUBE files are read as a stream of words instead.
    if (suffix != "cube" && suffix != "cub") {
        lines = text.split('\n');
        for (auto &line : lines)
            if (line.endsWith('\r'))
                line.chop(1);
    }
    r.status = tr("ファイルを読み込みました（計算の成否は判定しません）");
    if (suffix == "cube" || suffix == "cub") {
        r.format = "CUBE";
        const auto values = cubeValues(r, text);
        if (!r.frames.isEmpty())
            cache.store(dataset, r, values);
        cubeSurface(r, values);
    } else if (QStringView(text).trimmed().startsWith(u"[Molden Format]", Qt::CaseInsensitive)) {
        r.format = "MOLDEN";
        const auto data = moldenData(r, lines);
        if (!r.frames.isEmpty())
            cache.store(r, data);
        moldenSurface(r, data, orbital, cache);
    } else if (suffix == "normal") {
        r.format = "Raman";
        if (!text.contains("normal") || !text.contains("Raman Scattering Plot") ||
            !text.contains("S [Ang**4/amu]"))
            fail(tr("通常ラマンの出力ファイルを認識できません。"));
        QJsonArray peaks;
        bool table = false;
        QSet<int> seen;
        for (const auto &line : lines) {
            if (line.contains("S [Ang**4/amu]")) {
                table = true;
                continue;
            }
            if (line.contains("Excitation wavelength"))
                r.properties["raman_wavelength_nm"] = logNumber(words(line).value(2));
            const auto tokens = words(line);
            if (!table || tokens.isEmpty() || line.trimmed().startsWith('-'))
                continue;
            if (tokens.size() == 2)
                break; // The broadened cross section that follows is not activity.
            bool ok;
            const int index = tokens.value(0).toInt(&ok);
            const auto frequency = fortranNumber(tokens.value(1)), activity = fortranNumber(tokens.value(2));
            if (!ok || index <= 0 || seen.contains(index) || !frequency || !activity || tokens.size() != 3 ||
                peaks.size() >= 4096)
                fail(tr("通常ラマンのピーク表が不正です。"));
            seen.insert(index);
            peaks.append(
                QJsonObject{{"raman_index", index}, {"frequency", *frequency}, {"activity", *activity}});
        }
        if (peaks.isEmpty())
            fail(tr("通常ラマンのピーク表がありません。"));
        r.properties["raman"] = peaks;
    } else if (suffix == "xyz") {
        r.format = "XYZ";
        xyz(r, lines);
    } else {
        r.format = "NWChem";
        NwchemLog log(&r);
        for (const auto &line : std::as_const(lines))
            log.add(line);
        log.finish();
    }
    if (r.format != "NWChem" && r.format != "Raman" && r.frames.isEmpty())
        fail(tr("表示可能な原子座標がありません。"));
    return r;
}
Document resultModeGeometry(const ResultData &r, int mode, double phase, double amplitude) {
    if (mode < 0 || mode >= r.modes.size() || !std::isfinite(phase) || !std::isfinite(amplitude) ||
        amplitude < 0 || amplitude > 2)
        fail(tr("振動モード・表示振幅が不正です。"));
    const auto &v = r.modes[mode].displacement;
    auto d = r.vibrationGeometry;
    if (v.size() != d.atoms.size() || v.isEmpty())
        fail(tr("この振動モードには変位データがありません。"));
    double scale = 0;
    for (const auto &p : v)
        scale = std::max(scale, p.norm());
    if (scale > 0)
        for (int i = 0; i < d.atoms.size(); ++i)
            d.atoms[i].position += v[i] * (std::sin(phase) * amplitude / scale);
    return d;
}
QString resultOutcome(const ResultData &data) {
    switch (data.outcome) {
    case ResultOutcome::Normal:
        return "normal";
    case ResultOutcome::Incomplete:
        return "incomplete";
    case ResultOutcome::Error:
        return "error";
    case ResultOutcome::NotApplicable:
        return {};
    }
    return {};
}
QVector<bool> resultRigidModes(const ResultData &data) {
    QVector<bool> rigid(data.modes.size(), false);
    const auto &atoms = data.vibrationGeometry.atoms;
    if (atoms.isEmpty() || data.modes.size() != 3 * atoms.size())
        return rigid;
    // Linear: every atom within 1e-3 Å of the line through the first two separated atoms.
    bool linear = true;
    Vec axis = Vec::Zero();
    for (const auto &a : atoms)
        if ((a.position - atoms[0].position).norm() > 1e-3) {
            axis = (a.position - atoms[0].position).normalized();
            break;
        }
    for (const auto &a : atoms) {
        const Vec d = a.position - atoms[0].position;
        if ((d - d.dot(axis) * axis).norm() > 1e-3)
            linear = false;
    }
    const int count = atoms.size() == 1 ? 3 : linear ? 5 : 6;
    QVector<int> order(data.modes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return std::abs(data.modes[a].frequency) < std::abs(data.modes[b].frequency);
    });
    for (int i = 0; i < count && i < order.size(); ++i)
        rigid[order[i]] = true;
    return rigid;
}

} // namespace mf
