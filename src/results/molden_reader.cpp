// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/geometry.h"
#include "model/i18n.h"
#include "model/units.h"
#include "results/molden_orbitals.h"
#include "results/result_cache.h"
#include "results/result_fields.h"
#include "results/result_parsing.h"
#include "results/results.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace mf::result_detail {
// Reads a MOLDEN file into the result, all but the orbital surface (moldenSurface).
MoldenData moldenData(ResultData &r, const QStringList &lines) {
    QMap<QString, QStringList> blocks;
    QMap<QString, QString> headers;
    QString current;
    const QRegularExpression section("^\\s*\\[([^]]+)\\](.*)$");
    for (const auto &line : lines) {
        // Only a line that begins with '[' can be a section header: the others skip the expression.
        const auto trimmed = QStringView(line).trimmed();
        auto match = trimmed.startsWith('[') ? section.match(line) : QRegularExpressionMatch();
        if (match.hasMatch()) {
            current = match.captured(1).trimmed().toUpper();
            if (blocks.contains(current))
                fail(tr("MOLDENのセクションが重複しています: %1").arg(current));
            blocks[current] = {};
            headers[current] = match.captured(2).trimmed().toUpper();
        } else if (!current.isEmpty() && !trimmed.isEmpty())
            blocks[current] << line;
    }
    if (!blocks.contains("MOLDEN FORMAT"))
        fail(tr("MOLDENヘッダーがありません。"));
    if (blocks.contains("ATOMS")) {
        const auto units = headers["ATOMS"];
        if (units != "ANGS" && units != "AU" && units != "(ANGS)" && units != "(AU)")
            fail(tr("MOLDEN [Atoms] の単位はAngsまたはAUを指定してください。"));
        const double scale = units.contains(u"ANGS") ? 1 : BohrToAngstrom;
        Document d;
        for (const auto &line : blocks["ATOMS"]) {
            auto row = words(line);
            if (row.size() != 6 || integer(row[1], 1, maxAtoms) != d.atoms.size() + 1)
                fail(tr("MOLDEN原子表の形式・番号が不正です。"));
            d.atoms << atom(symbol(integer(row[2], 1, elements().last().number)), point(row, 3, scale));
        }
        addFrame(r, d, tr("MOLDEN原子座標"));
        r.orbitalFrame = 0;
    }
    if (blocks.contains("GEOMETRIES")) {
        if (headers["GEOMETRIES"] != "XYZ")
            fail(tr("MOLDENの構造列はXYZ形式に対応します。"));
        // XYZ titles may be blank; use original lines to preserve them.
        QStringList geometry;
        bool active = false;
        for (const auto &line : lines) {
            if (line.trimmed().startsWith('[')) {
                if (active)
                    break;
                active = line.contains(u"[GEOMETRIES]", Qt::CaseInsensitive);
            } else if (active)
                geometry << line;
        }
        xyz(r, geometry);
    }
    ResultOrbital orbital;
    QVector<QVector<double>> coefficients;
    QVector<double> mo;
    QSet<int> seenCoefficients;
    qint64 totalCoefficients = 0;
    QSet<QString> fields;
    QHash<QString, int> perSpin;
    auto finishOrbital = [&] {
        if (fields.isEmpty())
            return;
        if (!fields.contains("ENE") || !fields.contains("OCCUP") || !fields.contains("SPIN"))
            fail(tr("MOLDENの軌道情報が不足しています。"));
        if (orbital.occupation < 0 || orbital.occupation > 2 ||
            (orbital.spin != "Alpha" && orbital.spin != "Beta"))
            fail(tr("MOLDENの軌道占有数・スピンが不正です。"));
        if (r.orbitals.size() >= 20000)
            fail(tr("MOLDENの軌道は2万件まで読み込めます。"));
        orbital.number = ++perSpin[orbital.spin];
        r.orbitals << orbital;
        totalCoefficients += mo.size();
        if (totalCoefficients > 4000000)
            fail(tr("MOLDENのMO係数は400万個までです。"));
        coefficients << mo;
        mo.clear();
        seenCoefficients.clear();
        orbital = {};
        fields.clear();
    };
    for (const auto &line : std::as_const(blocks["MO"])) {
        // "Key= value" with one '='; anything else is a coefficient line.
        const qsizetype equals = line.indexOf('=');
        if (equals < 0 || line.indexOf('=', equals + 1) >= 0) {
            const auto row = words(line);
            if (row.size() != 2 || fields.isEmpty())
                fail(tr("MOLDENのMO係数の行が不正です。"));
            const int ao = integer(row[0], 1, 4000) - 1;
            if (seenCoefficients.contains(ao))
                fail(tr("MOLDENのMO係数番号が重複しています。"));
            seenCoefficients.insert(ao);
            if (mo.size() <= ao)
                mo.resize(ao + 1);
            mo[ao] = number(row[1]);
            continue;
        }
        const QString key = line.left(equals).trimmed().toUpper(), value = line.mid(equals + 1).trimmed();
        if (key == "SYM" || fields.contains(key))
            finishOrbital();
        fields.insert(key);
        if (key == "SYM")
            orbital.symmetry = value;
        else if (key == "ENE")
            orbital.energy = number(value);
        else if (key == "OCCUP")
            orbital.occupation = number(value);
        else if (key == "SPIN")
            orbital.spin = value.left(1).toUpper() + value.mid(1).toLower();
    }
    finishOrbital();
    if (blocks.contains("FR-COORD")) {
        Document d;
        for (const auto &line : blocks["FR-COORD"]) {
            auto row = words(line);
            if (row.size() != 4)
                fail(tr("MOLDENの振動基準座標が不正です。"));
            d.atoms << atom(row[0], point(row, 1, BohrToAngstrom));
        }
        addFrame(r, d, tr("MOLDEN振動基準座標"));
        r.vibrationGeometry = r.frames.last().molecule;
    }
    for (const auto &line : blocks["FREQ"]) {
        if (r.modes.size() >= 6000)
            fail(tr("振動モード数が大きすぎます。"));
        r.modes.append({number(line.trimmed()), std::numeric_limits<double>::quiet_NaN(), {}});
    }
    const auto norm = blocks["FR-NORM-COORD"];
    QSet<int> seen;
    for (int i = 0; i < norm.size();) {
        auto row = words(norm[i++]);
        if (row.size() != 2 || row[0].toLower() != "vibration" || r.vibrationGeometry.atoms.isEmpty())
            fail(tr("MOLDEN振動モードのヘッダー・基準座標が不正です。"));
        const int mode = integer(row[1], 1, r.modes.size()) - 1;
        if (seen.contains(mode))
            fail(tr("振動モードの番号が重複しています。"));
        seen.insert(mode);
        for (int a = 0; a < r.vibrationGeometry.atoms.size(); ++a) {
            if (i >= norm.size())
                fail(tr("MOLDEN振動モードが途中で切れています。"));
            r.modes[mode].displacement << point(words(norm[i++]), 0, BohrToAngstrom);
        }
    }
    const auto intensities = blocks["INT"];
    if (!intensities.isEmpty() && intensities.size() != r.modes.size())
        fail(tr("MOLDENの振動数とIR強度の件数が一致しません。"));
    for (int i = 0; i < intensities.size(); ++i)
        r.modes[i].intensity = number(words(intensities[i]).first());
    bool energy = false;
    for (const auto &line : blocks["GEOCONV"]) {
        if (line.trimmed().toLower() == "energy") {
            energy = true;
            continue;
        }
        if (line.trimmed().front().isLetter()) {
            energy = false;
            continue;
        }
        if (energy)
            r.energies << ResultEnergy{"GEOCONV", number(line.trimmed()), 0};
    }
    blocks.remove("MO");
    return {blocks, coefficients};
}
// The surface of an orbital (the requested one, or the frontier orbital) at r.isovalue.
void moldenSurface(ResultData &r, const MoldenData &m, int requestedOrbital, const ResultCache &cache) {
    if (!r.orbitals.isEmpty()) {
        r.orbitalBasis = m.blocks.contains("GTO") && r.orbitalFrame >= 0;
        try {
            if (!r.orbitalBasis)
                fail(tr("軌道の形状表示には[Atoms]・[GTO]・[MO]が必要です。"));
            const auto &atoms = r.frames[r.orbitalFrame].molecule;
            int selected = requestedOrbital;
            if (selected < 0) {
                selected = frontierOrbital(r.orbitals, "Alpha");
                if (selected < 0)
                    selected = frontierOrbital(r.orbitals, "Beta");
                if (selected < 0)
                    selected = 0;
            }
            Vec lo = atoms.atoms.first().position, hi = lo;
            for (const auto &atom : atoms.atoms) {
                lo = lo.cwiseMin(atom.position);
                hi = hi.cwiseMax(atom.position);
            }
            // The same grid as MolForma's CUBE output: 3 Å around the atoms, equal spacing.
            const auto box = uniformGrid(lo.array() - 3., hi.array() + 3., r.orbitalDivisions);
            const Vec step = Vec::Constant(box.spacing);
            for (int axis = 0; axis < 3; ++axis)
                r.grid[axis] = box.divisions[axis] + 1;
            // An orbital evaluated before on this grid (for another isovalue) is not evaluated again.
            const auto key =
                QString("orbital %1 %2 %3").arg(selected).arg(r.normalizationRequest).arg(r.orbitalDivisions);
            auto evaluated = cache.orbital(key);
            if (!evaluated) {
                const auto wavefunction =
                    moldenWavefunction(m.blocks, atoms, m.coefficients, r.normalizationRequest);
                evaluated = {GridValues{moldenOrbitalGrid(wavefunction, selected, box.origin, step, r.grid),
                                        box.origin, step.asDiagonal()},
                             wavefunction.normalization};
                cache.store(key, evaluated->first, evaluated->second);
            }
            const auto &values = evaluated->first.values;
            r.minimum = *std::min_element(values.cbegin(), values.cend());
            r.maximum = *std::max_element(values.cbegin(), values.cend());
            mesh(r, values, evaluated->first.origin, evaluated->first.axes);
            r.selectedOrbital = selected;
            r.orbitalNormalization = evaluated->second;
            if (r.surface.isEmpty())
                r.warnings << tr("指定した等値の面がありません。「軌道」の等値を小さくしてください。");
            r.warnings << tr(
                "MOLDENの分子軌道を表示しています。赤が正、青が負です。全体の位相の符号は任意です。");
        } catch (const std::exception &e) {
            r.surface.clear();
            r.grid = {};
            if (requestedOrbital >= 0)
                throw;
            r.orbitalError = QString::fromUtf8(e.what());
            r.warnings << tr("軌道形状を表示できません: %1").arg(r.orbitalError);
        }
    }
}

} // namespace mf::result_detail
