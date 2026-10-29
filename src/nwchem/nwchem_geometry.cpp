// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_geometry.h"
#include "model/i18n.h"
#include "model/memo.h"
#include <QLocale>
#include <cmath>
#include <openbabel/elements.h>

namespace mf {
static NwchemGeometryFragment geometryFragment(const Document &document, CoordinateUnit unit,
                                               const NwchemGeometryOptions &options);
NwchemGeometryFragment nwchemGeometryFragment(const Document &document, CoordinateUnit unit,
                                              const NwchemGeometryOptions &options) {
    // The coordinates table checks it on every change, and the input generation writes the same block.
    thread_local Memo<std::tuple<Document, CoordinateUnit, NwchemGeometryOptions>, NwchemGeometryFragment>
        memo;
    return memo.get({document, unit, options}, [&] { return geometryFragment(document, unit, options); });
}
static NwchemGeometryFragment geometryFragment(const Document &document, CoordinateUnit unit,
                                               const NwchemGeometryOptions &options) {
    auto errors = document.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
    if (document.atoms.isEmpty())
        fail(tr("原子を追加するとNWChem座標を表示します。"));
    // The shortest text that reads back as the same double: 0.758, not 0.75800000000000001.
    auto number = [](double value) { return QString::number(value, 'g', QLocale::FloatingPointShortest); };
    NwchemGeometryFragment result;
    QString &text = result.geometry;
    text += QString("geometry %1units %2 nocenter noautosym%3")
                .arg(options.name.isEmpty() ? QString() : options.name + ' ',
                     unit == CoordinateUnit::Bohr ? "au" : "angstrom",
                     options.zcoord.isEmpty() && !options.autoz ? " noautoz" : "");
    if (unit == CoordinateUnit::Angstrom)
        text += " angstrom_to_au " + number(1. / BohrToAngstrom);
    text += '\n';
    QStringList fixed;
    QSet<QString> ghosts;
    for (int i = 0; i < document.atoms.size(); ++i) {
        const auto &atom = document.atoms[i];
        element(atom.element);
        const QString tag = (atom.ghost ? "bq" : "") + atom.element;
        const Vec p = atom.position / coordinateScale(unit);
        text += QString("  %1 %2 %3 %4").arg(tag, number(p.x()), number(p.y()), number(p.z()));
        double mass = atom.mass;
        if (atom.isotope) {
            mass = OpenBabel::OBElements::GetExactMass(element(atom.element).number, atom.isotope);
            if (!std::isfinite(mass) || mass <= 0)
                fail(tr("原子%1: 同位体の質量が不明です。amuを小数で指定してください。").arg(i + 1));
        }
        if (mass > 0)
            text += " mass " + number(mass);
        text += '\n';
        if (atom.fixed)
            fixed << QString::number(i + 1);
        if (atom.ghost)
            ghosts.insert(atom.element);
    }
    if (!options.zcoord.isEmpty())
        text += "  zcoord\n    " + options.zcoord.join("\n    ") + "\n  end\n";
    text += "end\n";
    if (options.constraints) {
        result.constraints += "\nconstraints\n  clear\n";
        // Keep lines short for NWChem's input parser, even for large structures.
        for (int i = 0; i < fixed.size(); i += 12)
            result.constraints += "  fix atom " + fixed.mid(i, 12).join(' ') + '\n';
        result.constraints += "end\n";
    }
    auto symbols = ghosts.values();
    std::sort(symbols.begin(), symbols.end());
    for (const auto &symbol : symbols)
        result.comments +=
            QString("# Ghost basis required in BASIS: bq%1 library %1 <basis-name>\n").arg(symbol);
    return result;
}
QString nwchemGeometry(const Document &document, CoordinateUnit unit, bool fragmentComments,
                       const NwchemGeometryOptions &options) {
    const auto result = nwchemGeometryFragment(document, unit, options);
    return (fragmentComments
                ? QString("# MolForma: geometry fragment; add basis, charge, method and task separately.\n")
                : QString()) +
           result.geometry + result.constraints + (fragmentComments ? result.comments : QString());
}
} // namespace mf
