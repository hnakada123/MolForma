// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/i18n.h"
#include "model/memo.h"
#include "nwchem/input_builder.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_catalog.h"
#include "nwchem/nwchem_geometry.h"
#include "nwchem/nwchem_output.h"
#include "nwchem/nwchem_paths.h"
#include "nwchem/nwchem_plan.h"
#include <QLocale>
#include <algorithm>
namespace mf {
using namespace nwchem_detail;
namespace {
QString shortest(double n) {
    return QString::number(n, 'g', QLocale::FloatingPointShortest);
}
QStringList sorted(const QSet<QString> &set) {
    auto list = set.values();
    std::sort(list.begin(), list.end());
    return list;
}
void geometryBlocks(NwchemInputBuilder &out, const Document &d, CoordinateUnit unit,
                    const NwchemGeometryOptions &options, bool cartesianOverride) {
    const auto part = nwchemGeometryFragment(d, unit, options);
    NwchemInputBuilder body;
    const auto end = part.geometry.indexOf('\n') + 1;
    body.sourced(part.geometry.left(end), {"driver_cartesian"}, cartesianOverride);
    body.append(part.geometry.mid(end));
    out.block("geometry", body.result, {}, "driver", {SettingTarget::Molecule},
              tr("geometry（%n 原子）", nullptr, d.atoms.size()));
    out.block("constraints", part.constraints, {}, {}, {SettingTarget::Molecule});
}
void basisBlocks(NwchemInputBuilder &out, const Document &d, const QJsonObject &s, bool allGhosts) {
    const NwchemCalculation calc(s);
    const auto basis = s["basis"].toString();
    const bool spherical = s["basis_functions"] == "spherical" ||
                           (s["basis_functions"] == "auto" && !cartesianByConvention(basis));
    const auto p = pasted(s);
    QSet<QString> real, ghosts, ecps;
    for (const auto &a : d.atoms) {
        (a.ghost ? ghosts : real).insert(a.element);
        if (!a.ghost && coreElectrons(s, p, a.element, element(a.element).number))
            ecps.insert(a.element);
    }
    if (allGhosts)
        ghosts.unite(real);
    const auto type = spherical ? QString(" spherical\n") : QString(" cartesian\n");
    QString text = "\nbasis" + type;
    // A pasted basis replaces the library's for its element (NWChem would add its shells to a library
    // basis named for the same element, as "*" does).
    const bool perElement = std::any_of(real.begin(), real.end(), [&](const auto &symbol) {
        return elementBasis(s, symbol) != basis || p.basis.lines.contains(symbol);
    });
    // The pasted lines of an element, for a ghost of it with the tag "bq<element>".
    const auto pastedLines = [&](const QString &symbol, const QString &tag) {
        QString lines;
        for (const auto &line : p.basis.lines[symbol])
            lines += line.startsWith("  " + symbol + ' ') ? "  " + tag + line.mid(2 + symbol.size()) + '\n'
                                                          : line + '\n';
        return lines;
    };
    if (perElement)
        for (const auto &symbol : sorted(real))
            if (p.basis.lines.contains(symbol))
                text += pastedLines(symbol, symbol);
            else
                text += QString("  %1 library \"%2\"\n").arg(symbol, elementBasis(s, symbol));
    else
        text += "  * library \"" + basis + "\"\n";
    for (const auto &symbol : sorted(ghosts))
        if (p.basis.lines.contains(symbol))
            text += pastedLines(symbol, "bq" + symbol);
        else
            text += QString("  bq%1 library %1 \"%2\"\n").arg(symbol, elementBasis(s, symbol));
    text += "end\n";
    out.block("basis", text, p.basis.lines.isEmpty() ? QString("basis") : QString("basis_custom"), {},
              {SettingTarget::Calculation}, perElement ? QString() : "basis \"" + basis + '"');
    if (calc.dft() && s["ri_j"].toBool()) {
        text = "\nbasis \"cd basis\"" + type + "  * library \"weigend coulomb fitting\"\n";
        for (const auto &symbol : sorted(ghosts))
            text += QString("  bq%1 library %1 \"weigend coulomb fitting\"\n").arg(symbol);
        out.block("basis", text + "end\n", "ri_j");
    }
    if (!ecps.isEmpty()) {
        text = "\necp\n";
        for (const auto &symbol : sorted(ecps))
            if (p.ecp.lines.contains(symbol))
                text += p.ecp.lines[symbol].join('\n') + '\n';
            else
                text +=
                    QString("  %1 library \"%2\"\n").arg(symbol, libraryBasis(elementBasis(s, symbol))->ecp);
        out.block("ecp", text + "end\n", p.ecp.lines.isEmpty() ? QString("basis") : QString("ecp_custom"));
    }
}
void methodBlocks(NwchemInputBuilder &out, const Document &d, const QJsonObject &s,
                  const NwchemValidation &check) {
    const auto advanced = nwchemAdvanced(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    auto integer = [&](const char *key) { return QString::number(s[key].toInt()); };
    const auto method = str("method");
    const NwchemCalculation calc(s);
    const bool own = str("scf_convergence") == "default";
    const QString threshold = nwchemScfThreshold(s);
    NwchemInputBuilder body;
    if (calc.dft()) {
        const auto *f = functional(str("functional"));
        body.append("\ndft\n  xc " +
                    (QString(f->value) == "custom" ? str("xc_custom").simplified() : QString(f->xc)) + '\n');
        if (*f->extra)
            body.append(QString("  ") + f->extra + '\n');
        body.append("  mult " + QString::number(d.multiplicity) + '\n');
        if (check.reference == "uhf")
            body.append("  odft\n");
        QString convergence;
        QStringList keys;
        bool overridden = false;
        for (const auto &[key, keyword] :
             {std::pair{"dft_energy", "energy"}, std::pair{"dft_density", "density"},
              std::pair{"dft_gradient", "gradient"}})
            if (!own || advanced.contains(key)) {
                convergence += QString(" ") + keyword + ' ' + nwchemAdvancedNumber(d, key, threshold);
                keys << key;
                overridden |= advanced.contains(key);
            }
        body.append("  iterations " + integer("scf_maxiter") + '\n');
        if (!convergence.isEmpty())
            body.sourced("  convergence" + convergence + '\n', keys, overridden);
        body.sourced("  grid " + str("grid") + (advanced["dft_grid_nodisk"].toBool() ? " nodisk" : "") + '\n',
                     {"dft_grid_nodisk"}, advanced["dft_grid_nodisk"].toBool());
        body.append(nwchemAdvancedLines(d, "dft"));
        body.append(nwchemAdvancedLines(d, "vectors"));
        if (str("dispersion") != "none")
            body.append(str("dispersion") == "d3" ? "  disp vdw 3\n" : "  disp vdw 4\n");
        body.append("end\n");
        out.block("dft", body.result, "method", "dft");
    } else {
        body.append("\nscf\n  " + check.reference + "\n  nopen " + QString::number(d.multiplicity - 1) +
                    "\n  maxiter " + integer("scf_maxiter") + '\n');
        if (!own || advanced.contains("scf_thresh"))
            body.sourced("  thresh " + nwchemAdvancedNumber(d, "scf_thresh", threshold) + '\n',
                         {"scf_thresh"}, advanced.contains("scf_thresh"));
        body.append(nwchemAdvancedLines(d, "scf"));
        body.append(nwchemAdvancedLines(d, "vectors"));
        body.append("end\n");
        out.block("scf", body.result, "method", "scf");
        if (calc.correlated()) {
            NwchemInputBuilder correlation;
            correlation.append(calc.method == NwchemMethod::Mp2 ? "\nmp2\n" : "\nccsd\n");
            correlation.sourced(
                "  freeze " +
                    nwchemAdvancedNumber(d, "corr_freeze", s["freeze_core"].toBool() ? "atomic" : "0") + '\n',
                {"corr_freeze"}, advanced.contains("corr_freeze"));
            if (calc.method != NwchemMethod::Mp2) {
                correlation.append("  maxiter " + integer("cc_maxiter") + '\n');
                correlation.sourced("  thresh " + nwchemAdvancedNumber(d, "cc_thresh", "1e-7") + '\n',
                                    {"cc_thresh"}, advanced.contains("cc_thresh"));
            }
            correlation.append(nwchemAdvancedLines(d, "correlation"));
            correlation.append("end\n");
            out.block(calc.method == NwchemMethod::Mp2 ? "mp2" : "ccsd", correlation.result, "method",
                      "correlation");
        }
    }
}
void extraInput(NwchemInputBuilder &out, const QString &text, const QString &key) {
    if (text.trimmed().isEmpty())
        return;
    NwchemInputBuilder body;
    body.sourced("\n# Additional input (not checked by MolForma)\n" + text.trimmed() + '\n', {key}, false,
                 true);
    // Free-form input is one opaque region, even if it contains nested blocks or repeated directives.
    out.block("additional", body.result, key, {}, {SettingTarget::Calculation},
              text.trimmed().section('\n', 0, 0));
}
// Takes a block and its lines out of a generated input; the lines after it move up.
void removeBlock(NwchemGeneratedInput &input, int index) {
    const auto block = input.outline.blocks[index];
    auto lines = input.text.split('\n');
    int first = block.line, count = block.lines;
    // With the block goes one of the empty lines around it, when it stood between two.
    if (first > 0 && lines.value(first - 1).isEmpty() && lines.value(first + count).isEmpty()) {
        --first;
        ++count;
    }
    lines.remove(first, count);
    input.text = lines.join('\n');
    input.outline.blocks.remove(index);
    for (auto &other : input.outline.blocks)
        if (other.line >= first + count)
            other.line -= count;
    const auto shifted = [&](const QSet<int> &set) {
        QSet<int> result;
        for (const int line : set)
            if (line < first)
                result.insert(line);
            else if (line >= first + count)
                result.insert(line - count);
        return result;
    };
    input.outline.advanced = shifted(input.outline.advanced);
    input.outline.extra = shifted(input.outline.extra);
    for (auto &set : input.outline.settings)
        set = shifted(set);
}
// The orbitals of each fragment computed alone, then the whole molecule's initial orbitals from them.
// The DFT/SCF settings written before stay; each fragment's charge and multiplicity replace the
// molecule's, which come back for the tasks after it.
void fragmentGuess(NwchemInputBuilder &out, const Document &d, const QJsonObject &s,
                   const NwchemValidation &check, const NwchemTaskPlan &plan) {
    const auto fragments = nwchemFragments(d);
    if (fragments.isEmpty())
        return;
    const bool dft = NwchemCalculation(s).dft();
    const auto start = s["start"].toString();
    const auto block = [&](int multiplicity, const QString &vectors) {
        if (dft)
            return "dft\n  mult " + QString::number(multiplicity) + '\n' +
                   (check.reference == "uhf" ? "  odft\n" : "") + "  vectors " + vectors + "\nend\n";
        return "scf\n  " + check.reference + "\n  nopen " + QString::number(multiplicity - 1) +
               "\n  vectors " + vectors + "\nend\n";
    };
    QStringList files;
    out.append("\n# Initial orbitals from the fragments\n");
    for (int k = 0; k < fragments.size(); ++k) {
        const auto file = QString("%1_frag%2.movecs").arg(start).arg(k + 1);
        files << file;
        out.block("set",
                  QString(k ? "\nset geometry frag%1\ncharge %2\n" : "set geometry frag%1\ncharge %2\n")
                      .arg(k + 1)
                      .arg(fragments[k].charge),
                  "fragment_guess");
        out.block(dft ? "dft" : "scf", block(fragments[k].multiplicity, "input atomic output " + file),
                  "fragment_guess");
        out.block("task", plan.fragments[k] + '\n', "fragment_guess");
    }
    const auto output = nwchemAdvanced(d)["vectors_output"].toString();
    out.block("set", "\nset geometry geometry\ncharge " + QString::number(d.charge) + '\n', "fragment_guess");
    out.block(dft ? "dft" : "scf",
              block(d.multiplicity, "input fragment " + files.join(' ') +
                                        (output.isEmpty() ? QString() : " output \"" + output + '"')),
              "fragment_guess");
}
} // namespace
static NwchemGeneratedInput generation(const Document &d);
NwchemGeneratedInput generateNwchemInput(const Document &d) {
    thread_local Memo<Document, NwchemGeneratedInput> memo;
    return memo.get(d, [&] { return generation(d); });
}
static NwchemGeneratedInput generation(const Document &d) {
    const auto check = validateNwchem(d);
    if (!check.errors.isEmpty())
        throw ValidationFailure(check);
    const auto s = nwchemSettings(d), advanced = nwchemAdvanced(d);
    const auto plan = nwchemTaskPlan(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    auto integer = [&](const char *key) { return QString::number(s[key].toInt()); };
    const auto method = str("method");
    const NwchemCalculation calc(s);
    const auto unit = str("geometry_units") == "bohr" ? CoordinateUnit::Bohr : CoordinateUnit::Angstrom;
    const double scale = coordinateScale(unit);
    const bool scan = calc.task == NwchemTask::Scan, relaxedScan = scan && s["scan_relaxed"].toBool();
    NwchemInputBuilder out;
    const auto restart = nwchemRestart(s);
    out.append("# Generated by MolForma for NWChem\n");
    if (restart.active()) // NWChem keeps the geometry, constraints and orbitals of the database.
        out.block("restart", "restart " + str("start") + '\n', "restart");
    else
        out.block("start", "start " + str("start") + '\n', "start");
    if (!str("title").isEmpty())
        out.block("title", "title \"" + str("title") + "\"\n", "title");
    out.block("memory", "memory total " + integer("memory_mb") + " mb\n", "memory_mb", {},
              {SettingTarget::Execution});
    out.append(nwchemAdvancedLines(d, "files"));
    out.block("charge", "charge " + QString::number(d.charge) + '\n', "charge");
    if (!restart.active())
        out.append("\n");
    if (!scan && !restart.active()) {
        Document geometry = d;
        if (calc.optimization())
            for (auto &a : geometry.atoms)
                a.fixed |= a.ghost;
        NwchemGeometryOptions options;
        if (calc.optimization())
            options.zcoord = constraintLines(d, s, scale);
        options.autoz = nwchemAutoz(d);
        geometryBlocks(out, geometry, unit, options,
                       advanced["driver_cartesian"].toBool() && calc.optimization());
        // The fragments of an initial guess, each a geometry of its own.
        const auto fragments = nwchemFragments(d);
        for (int k = 0; k < fragments.size(); ++k) {
            Document piece;
            for (int i : fragments[k].atoms) {
                piece.atoms << d.atoms[i];
                piece.atoms.last().fixed = false;
            }
            NwchemGeometryOptions options;
            options.name = QString("frag%1").arg(k + 1);
            options.constraints = false;
            geometryBlocks(out, piece, unit, options, false);
            out.result.outline.blocks.last().setting = "fragment_guess"; // Without constraints: the geometry.
        }
        if (calc.task == NwchemTask::Neb) { // The end of the path, as NWChem's NEB names it.
            NwchemGeometryOptions end;
            end.name = "endgeom";
            end.constraints = false;
            geometryBlocks(out, *nwchemNebEnd(d).document, unit, end, false);
        }
    }
    basisBlocks(out, d, s, s["bsse"].toBool());
    if (s["include_point_charges"].toBool() && !d.pointCharges.isEmpty()) {
        QString text = "\nbq units angstrom\n";
        for (const auto &q : d.pointCharges)
            text += QString("  %1 %2 %3 %4\n")
                        .arg(shortest(q.position.x()), shortest(q.position.y()), shortest(q.position.z()),
                             shortest(q.charge));
        out.block("bq", text + "end\n", "include_point_charges", {},
                  {SettingTarget::Molecule, SettingTarget::Calculation});
    }
    if (const auto relativistic = str("relativistic"); relativistic != "none")
        out.block("relativistic",
                  "\nrelativistic\n" +
                      QString(relativistic == "zora"  ? "  zora on\n"
                              : relativistic == "dkh" ? "  douglas-kroll dkh\n"
                                                      : "  x2c on\n") +
                      "end\n",
                  "relativistic");
    methodBlocks(out, d, s, check);
    if (str("solvent") != "none") {
        NwchemInputBuilder body;
        body.append("\ncosmo\n");
        if (str("solvent") == "smd")
            body.append("  do_cosmo_smd true\n");
        body.append(str("solvent_name") == "dielectric"
                        ? "  dielec " + shortest(s["dielectric"].toDouble()) + '\n'
                        : "  solvent " + str("solvent_name") + '\n');
        body.append(nwchemAdvancedLines(d, "cosmo"));
        body.append("end\n");
        out.block("cosmo", body.result, "solvent", "cosmo");
    }
    if (calc.dft() && s["tddft"].toBool()) {
        QString text = "\ntddft\n  nroots " + integer("tddft_roots") + '\n';
        if (check.reference == "rhf" && !s["tddft_triplets"].toBool())
            text += "  notriplet\n";
        if (s["tddft_tda"].toBool())
            text += "  cis\n";
        if (s["tddft_cdspectrum"].toBool())
            text += "  cdspectrum\n";
        NwchemInputBuilder body;
        body.append(text);
        // An excited state's optimization: its CI vectors and the gradient of that state.
        if (calc.task == NwchemTask::Optimize) {
            const auto root = QString::number(s["tddft_root"].toInt());
            body.sourced("  civecs\n  target " + root + "\n  grad\n    root " + root + "\n  end\n",
                         {"tddft_root"});
        }
        // The CUBE of an excited state: the CI vectors for its transition density, and a gradient of that
        // state for its density matrices.
        const auto advanced = nwchemAdvanced(d);
        if (advanced["cube_output"].toBool() && nwchemCubeExcited(advanced["cube_type"].toString())) {
            body.sourced("  civecs\n", {"cube_output", "cube_type"}, true);
            if (nwchemCubeGradient(d)) {
                const auto root = QString::number(nwchemCubeRoot(d));
                body.sourced("  target " + root + "\n  grad\n    root " + root + "\n  end\n",
                             {"cube_output", "cube_type", "cube_root"}, true);
            }
        }
        body.append("end\n");
        out.block("tddft", body.result, "tddft", "visualization");
    }
    if (calc.driver()) {
        NwchemInputBuilder body;
        body.append("\ndriver\n  maxiter " + integer("opt_maxiter") + "\n  " + str("opt_convergence") + '\n');
        if (restart.mode == "hessian") // Not the Hessian the DRIVER kept from an optimization.
            body.sourced("  clear\n  inhess 2\n", {"restart"});
        body.append(nwchemAdvancedLines(d, "driver"));
        body.append("end\n");
        out.block("driver", body.result, "opt_maxiter", "driver");
    }
    if (calc.frequencies()) {
        const auto lines = nwchemAdvancedLines(d, "frequency");
        const bool reuse = restart.mode == "reuse";
        if (!lines.text.isEmpty() || reuse) {
            NwchemInputBuilder body;
            if (reuse) {
                // The masses of the geometry block, which a restart leaves out: by atom number.
                QString text = "\nfreq\n  reuse\n";
                for (int i = 0; i < d.atoms.size(); ++i)
                    if (const double mass = nwchemAtomMass(d.atoms[i]); mass > 0)
                        text += QString("  mass %1 %2\n")
                                    .arg(i + 1)
                                    .arg(QString::number(mass, 'g', QLocale::FloatingPointShortest));
                body.sourced(text, {"restart"});
            } else
                body.sourced("\nfreq\n", {"frequency"}, true);
            body.append(lines);
            body.sourced("end\n", {"frequency"}, !reuse);
            out.block("freq", body.result, reuse ? "restart" : QString(), "frequency");
        }
        if (!reuse && !check.numericalFrequencies && advanced.contains("hessian_thresh"))
            out.advancedBlock("hessian",
                              "\nhessian\n  thresh " + nwchemAdvancedNumber(d, "hessian_thresh", "1e-6") +
                                  "\nend\n",
                              {"hessian_thresh"}, "frequency");
    }
    if (calc.task == NwchemTask::Neb)
        // print_shift 1 prints the path into the log after each step, where the result viewer reads it.
        out.block("neb",
                  "\nneb\n  nbeads " + integer("neb_beads") + "\n  kbeads " +
                      shortest(s["neb_spring"].toDouble()) + "\n  maxiter " + integer("neb_maxiter") +
                      "\n  stepsize " + shortest(s["neb_stepsize"].toDouble()) + "\n  print_shift 1\nend\n",
                  "neb_beads");
    if (calc.task == NwchemTask::Irc) {
        const auto direction = s["irc_direction"].toString();
        out.block("mepgs",
                  "\nmepgs\n  maxmep " + integer("irc_points") + "\n  maxiter " + integer("opt_maxiter") +
                      "\n  stride " + shortest(s["irc_stride"].toDouble()) +
                      (direction == "both" ? QString() : "\n  " + direction) + "\nend\n",
                  "irc_points");
    }
    if (s["bsse"].toBool()) {
        QString text = "\nbsse\n";
        const auto monomers = bsseMonomers(d);
        for (int i = 0; i < monomers.size(); ++i) {
            QStringList numbers, lines;
            for (int atom : monomers[i])
                numbers << QString::number(atom + 1);
            for (int k = 0; k < numbers.size(); k += 16)
                lines << numbers.mid(k, 16).join(' ');
            text += QString("  mon m%1 %2\n  charge %3\n  mult 1\n")
                        .arg(i + 1)
                        .arg(lines.join(" \\\n    "))
                        .arg(monomerCharge(d, monomers[i]));
        }
        out.block("bsse", text + "end\n", "bsse");
    }
    extraInput(out, str("extra_input"), "extra_input");
    if (!restart.active())
        fragmentGuess(out, d, s, check, plan);
    out.append("\n");
    auto taskLine = [&](const QString &value) { out.block("task", value, "task"); };
    if (scan) {
        // A grid of the second coordinate's points for each point of the first, named in the order
        // they are computed.
        const auto ids = idsFromJson(s["scan_atoms"]), second = idsFromJson(s["scan2_atoms"]);
        const bool plane = twoDimensionalScan(s);
        const int points = s["scan_steps"].toInt(), points2 = plane ? s["scan2_steps"].toInt() : 1;
        const auto named = [&](const Ids &atoms, double value) {
            return QString("%1 = %2").arg(zcoordLine(d, atoms, value, 1, "").section(' ', 0, atoms.size()),
                                          shortest(value));
        };
        for (int step = 0; step < points; ++step)
            for (int step2 = 0; step2 < points2; ++step2) {
                const int point = step * points2 + step2 + 1;
                const double value = scanValue(s, step), value2 = scanValue(s, step2, 2);
                NwchemGeometryOptions options;
                options.name = QString("scan%1").arg(point);
                options.constraints = false;
                if (relaxedScan) {
                    options.zcoord = constraintLines(d, s, scale)
                                     << zcoordLine(d, ids, value, scale, "mfscan");
                    if (plane)
                        options.zcoord << zcoordLine(d, second, value2, scale, "mfscan2");
                }
                out.block(
                    "title",
                    QString("\ntitle \"MolForma scan %1/%2 %3\"\n")
                        .arg(point)
                        .arg(points * points2)
                        .arg(plane ? named(ids, value) + " and " + named(second, value2) : named(ids, value)),
                    "scan_steps");
                geometryBlocks(out, scanGeometry(d, s, step, step2), unit, options, false);
                out.block("set", "set geometry " + options.name + '\n', "scan_steps");
                taskLine(plan.main[point - 1] + '\n');
            }
    } else {
        for (int i = 0; i < plan.main.size(); ++i) {
            if (i == plan.frequency) {
                NwchemInputBuilder body;
                body.sourced(plan.main[i] + '\n', {"numerical_frequencies"});
                out.block("task", body.result, "task");
            } else
                taskLine(plan.main[i] + '\n');
        }
    }
    Document last = d;
    auto lastCheck = check;
    if (s["followup"].toBool()) {
        last = followupDocument(d);
        const auto ns = nwchemSettings(last);
        lastCheck = validateNwchem(last);
        out.append("\n# Follow-up single point at the optimized structure\n");
        out.block("unset", "unset scf:*\n", "followup");
        out.block("unset", "unset dft:*\n", "followup");
        if (const NwchemCalculation next(ns); next.correlated())
            out.block("unset",
                      next.method == NwchemMethod::Mp2 ? QString("unset mp2:*\n") : QString("unset ccsd:*\n"),
                      "followup");
        NwchemInputBuilder next;
        basisBlocks(next, last, ns, false);
        methodBlocks(next, last, ns, lastCheck);
        for (auto &block : next.result.outline.blocks) {
            if (block.setting == "basis")
                block.setting = "followup_basis";
            else if (block.setting == "method")
                block.setting = "followup_method";
        }
        out.append(next.result);
        out.block("task", "\n" + plan.followup.first() + '\n', "followup");
    }
    {
        if (s["prop_mulliken"].toBool() || s["prop_nmr"].toBool() || s["prop_polarizability"].toBool() ||
            s["prop_epr"].toBool()) {
            QString text = "\nproperty\n";
            if (s["prop_mulliken"].toBool())
                text += "  mulliken\n";
            if (s["prop_nmr"].toBool())
                text += "  shielding\n";
            if (s["prop_epr"].toBool())
                text += "  gshift\n  hyperfine\n";
            if (s["prop_polarizability"].toBool())
                text += "  aoresponse 1 0.0\n";
            out.block("property", text + "end\n", "prop_mulliken");
            out.block("task", plan.properties.first() + '\n', "prop_mulliken");
        }
        if (s["prop_esp"].toBool()) {
            out.block("esp", "\nesp\n  restrain\nend\n", "prop_esp");
            out.block("task", plan.esp.first() + '\n', "prop_esp");
        }
    }
    int gridTask = 0;
    for (const auto &kind : {QString("esp"), QString("efield")})
        if (s["prop_" + kind + "_grid"].toBool()) {
            out.block("unset", "\nunset prop:*\n", "prop_" + kind + "_grid");
            out.block("property",
                      "property\n  " + kind + "\n  grid pad " +
                          shortest(s["property_grid_padding"].toDouble()) + " ngrid " +
                          integer("property_grid_points") + " output " + str("start") + "_" + kind +
                          "_grid.cube\nend\n",
                      "prop_" + kind + "_grid");
            out.block("task", plan.grids[gridTask++] + '\n', "prop_" + kind + "_grid");
        }
    out.append(nwchemOutputLines(last, lastCheck));
    if (s["prop_raman"].toBool()) {
        const double frequency = 45.5633525316 / s["raman_wavelength_nm"].toDouble(); // hc/Eh in nm.
        out.block("unset", "\nunset prop:*\n", "prop_raman");
        out.block("property", "property\n  response 1 " + QString::number(frequency, 'g', 14) + "\nend\n",
                  "raman_wavelength_nm");
        out.block("raman", "raman\n  normal\n  lorentzian\nend\n", "prop_raman");
        out.block("task", plan.raman.first() + '\n', "prop_raman");
    }
    if (s["prop_ord"].toBool()) {
        const int points = s["ord_points"].toInt();
        for (int i = 0; i < points; ++i) {
            const double wavelength =
                s["ord_start_nm"].toDouble() +
                (s["ord_end_nm"].toDouble() - s["ord_start_nm"].toDouble()) * i / (points - 1);
            out.block("unset", "\nunset prop:*\n", "prop_ord");
            out.block("property",
                      "property\n  response 1 " + QString::number(45.5633525316 / wavelength, 'g', 14) +
                          "\n  giao\nend\n",
                      "prop_ord");
            out.block("task", plan.ord[i] + '\n', "prop_ord");
        }
    }
    // NMR spin–spin couplings last: NWChem computes them for UKS only, which the DFT block keeps for the
    // tasks after it.
    if (s["prop_spinspin"].toBool()) {
        NwchemInputBuilder body;
        body.append("\nunset prop:*\n");
        if (lastCheck.reference == "rhf")
            body.append("dft\n  odft\nend\n");
        QString line = "  spinspin";
        if (s["spinspin_pairs"] == "hfp") {
            const auto pairs = nwchemSpinSpinPairs(d);
            QStringList numbers{QString::number(pairs.size())};
            for (const auto &[a, b] : pairs)
                numbers << QString::number(a + 1) << QString::number(b + 1);
            QStringList lines;
            for (int k = 0; k < numbers.size(); k += 16)
                lines << numbers.mid(k, 16).join(' ');
            line += ' ' + lines.join(" \\\n    ");
        }
        body.append("property\n" + line + "\nend\n");
        out.block("property", body.result, "prop_spinspin");
        out.block("task", plan.spinspin.first() + '\n', "prop_spinspin");
    }
    extraInput(out, str("extra_tasks"), "extra_tasks");
    // A continued job starts at the task the other job did not finish (validateNwchem checked the match).
    for (qsizetype done = 0, i = 0; done < restart.done.size() && i < out.result.outline.blocks.size();)
        if (out.result.outline.blocks[i].keyword == "task") {
            removeBlock(out.result, int(i));
            ++done;
        } else
            ++i;
    return out.result;
}
QString nwchemInput(const Document &d) {
    return generateNwchemInput(d).text;
}
} // namespace mf
