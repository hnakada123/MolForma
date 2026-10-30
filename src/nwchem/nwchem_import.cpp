// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_import.h"
#include "editing/chemistry.h"
#include "model/i18n.h"
#include "model/numbers.h"
#include "model/units.h"
#include "nwchem/nwchem.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace mf {
namespace {
// Statements of an NWChem input: comments removed, ";" separated, "\" continuations joined.
QStringList statements(const QString &text) {
    QStringList result;
    QString pending;
    for (auto line : text.split('\n')) {
        line.remove('\r');
        bool quoted = false;
        QString current;
        for (int i = 0; i < line.size(); ++i) {
            const QChar c = line[i];
            if (c == '"')
                quoted = !quoted;
            if (!quoted && c == '#')
                break;
            if (!quoted && c == ';') {
                result << (pending + current).trimmed();
                pending.clear();
                current.clear();
                continue;
            }
            current += c;
        }
        if (current.trimmed().endsWith('\\')) {
            pending += current.trimmed().chopped(1) + ' ';
            continue;
        }
        result << (pending + current).trimmed();
        pending.clear();
    }
    if (!pending.isEmpty())
        result << pending.trimmed();
    result.removeAll(QString());
    return result;
}
QStringList words(const QString &line) {
    // Quoted strings stay one word, without the quotes.
    QStringList result;
    static const QRegularExpression word(R"re("([^"]*)"|(\S+))re");
    for (auto it = word.globalMatch(line); it.hasNext();) {
        const auto m = it.next();
        result << (m.capturedLength(1) || m.captured(0) == "\"\"" ? m.captured(1) : m.captured(2));
    }
    return result;
}
bool number(const QString &text, double &value) {
    const auto n = fortranNumber(text);
    value = n.value_or(0);
    return n.has_value();
}
// NWChem atom tags: the element symbol followed by any label ("C1", "H_a"), "bq" for ghosts.
QString tagElement(QString tag, bool &ghost) {
    ghost = false;
    if (tag.compare("bq", Qt::CaseInsensitive) == 0 || tag.compare("x", Qt::CaseInsensitive) == 0)
        return {}; // Point charges and dummy centers carry no element.
    if (tag.startsWith("bq", Qt::CaseInsensitive) && tag.size() > 2) {
        ghost = true;
        tag = tag.mid(2);
    }
    const auto valid = [](const QString &symbol) {
        try {
            element(symbol);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    };
    if (tag.size() >= 2 && tag[1].isLetter()) {
        const auto two = tag.left(1).toUpper() + tag.mid(1, 1).toLower();
        if (valid(two))
            return two;
    }
    const auto one = tag.left(1).toUpper();
    return valid(one) ? one : QString();
}
const QStringList blockNames{"geometry", "basis",    "ecp",   "so",      "scf",          "dft",     "mp2",
                             "ccsd",     "tce",      "tddft", "cosmo",   "relativistic", "driver",  "freq",
                             "vib",      "property", "esp",   "bsse",    "constraints",  "dplot",   "python",
                             "nwpw",     "qmmm",     "md",    "prepare", "mepgs",        "stepper", "hessian",
                             "bq",       "smd",      "neb",   "string",  "fragment",     "rt_tddft"};
} // namespace
QStringList nwchemStatements(const QString &text) {
    return statements(text);
}
NwchemImport importNwchemInput(const QString &text) {
    NwchemImport result;
    auto &d = result.document;
    auto &notes = result.notes;
    auto settings = nwchemDefaults();
    const auto lines = statements(text);
    QStringList tasks;
    bool haveGeometry = false;
    int multiplicity = 0;
    QJsonObject overrides;
    QSet<int> fixedAtoms;
    int fixedBudget = 1000000;   // Atom numbers expanded from ranges, however often they repeat.
    QMap<int, int> atomOfCenter; // 1-based NWChem center number → MolForma atom index.
    // The SCF thresholds of the scf block (thresh) and of the dft block (convergence energy, density
    // and gradient), read once the method is known.
    QStringList scfThresholds, dftThresholds;
    for (int i = 0; i < lines.size(); ++i) {
        auto w = words(lines[i]);
        const auto keyword = w.value(0).toLower();
        // Collect a block (with nested sub-blocks of GEOMETRY) up to its END.
        QStringList body;
        if (blockNames.contains(keyword)) {
            int depth = 1;
            for (++i; i < lines.size(); ++i) {
                const auto first = words(lines[i]).value(0).toLower();
                if (keyword == "geometry" && (first == "zmatrix" || first == "zcoord" || first == "system"))
                    ++depth;
                if (first == "end" && --depth == 0)
                    break;
                body << lines[i];
            }
        }
        // Method blocks after the first task belong to later steps (e.g. a follow-up single point).
        if (!tasks.isEmpty() &&
            QStringList{"basis", "ecp", "scf", "dft", "mp2", "ccsd", "cosmo", "relativistic", "tddft"}
                .contains(keyword)) {
            notes << tr("最初のtaskより後の計算条件（後続の計算）は取り込みません。");
            continue;
        }
        if (keyword == "start" || keyword == "restart") {
            if (w.size() > 1)
                settings["start"] = w[1];
        } else if (keyword == "title") {
            settings["title"] = w.mid(1).join(' ').left(120);
        } else if (keyword == "charge") {
            double q;
            if (number(w.value(1), q) && q == std::floor(q) && std::abs(q) <= 1000)
                d.charge = int(q);
        } else if (keyword == "memory") {
            // memory [total] <n> <unit> [heap <n> <unit>] [stack ...] [global ...]: MolForma keeps the
            // total in MB; without "total", heap + stack + global.
            double total = 0, parts = 0;
            for (int k = 1; k + 1 < w.size(); ++k) {
                double n;
                if (!number(w[k], n))
                    continue;
                const auto unit = w[k + 1].toLower();
                const double mb = unit == "gb" ? n * 1024 : unit == "mb" ? n : unit == "kb" ? n / 1024 : -1;
                if (mb <= 0)
                    continue;
                if (QStringList{"heap", "stack", "global"}.contains(w[k - 1].toLower()))
                    parts += mb;
                else
                    total = mb;
            }
            if (total > 0 || parts > 0) // Out of range values stay out of range, as validation reports.
                settings["memory_mb"] = int(std::round(std::min(total > 0 ? total : parts, 1e9)));
        } else if (keyword == "geometry") {
            if (haveGeometry) {
                notes << tr("2番目以降のGEOMETRYは取り込みません。");
                continue;
            }
            haveGeometry = true;
            double scale = 1;
            for (int k = 1; k + 1 < w.size(); ++k)
                if (w[k].toLower() == "units") {
                    const auto unit = w[k + 1].toLower();
                    scale = unit.startsWith("a") && unit != "au" && unit != "atomic" ? 1
                            : unit == "nm" || unit == "nanometers"                   ? 10
                            : unit == "pm" || unit == "picometers"                   ? 0.01
                                                                                     : BohrToAngstrom;
                }
            settings["geometry_units"] = scale == BohrToAngstrom ? "bohr" : "angstrom";
            int depth = 0;
            int centers = 0; // NWChem numbers every center, including skipped point charges.
            for (const auto &line : body) {
                const auto row = words(line);
                const auto first = row.value(0).toLower();
                if (first == "zmatrix" || first == "zcoord" || first == "system") {
                    if (first == "zmatrix")
                        fail(tr(
                            "Z-matrix形式のGEOMETRYは取り込めません。Cartesian座標の入力を使ってください。"));
                    notes << tr("GEOMETRYの%1は取り込みません。").arg(first);
                    ++depth;
                    continue;
                }
                if (first == "end") {
                    --depth;
                    continue;
                }
                if (depth > 0 || first == "symmetry" || first == "load")
                    continue;
                double x, y, z;
                if (row.size() < 4 || !number(row[1], x) || !number(row[2], y) || !number(row[3], z))
                    continue;
                bool ghost;
                const auto symbol = tagElement(row[0], ghost);
                ++centers;
                if (symbol.isEmpty()) {
                    notes << tr("元素を判定できない中心を取り込みません: %1").arg(row[0]);
                    continue;
                }
                atomOfCenter[centers] = d.atoms.size();
                Atom a;
                a.element = symbol;
                a.ghost = ghost;
                a.position = Vec(x, y, z) * scale;
                for (int k = 4; k + 1 < row.size(); ++k)
                    if (row[k].toLower() == "mass") {
                        double m;
                        if (number(row[k + 1], m) && m > 0)
                            a.mass = m;
                    }
                d.atoms << a;
            }
        } else if (keyword == "basis") {
            QString name =
                w.size() > 1 &&
                        !QStringList{"spherical", "cartesian", "print", "noprint"}.contains(w[1].toLower())
                    ? w[1].toLower()
                    : QString("ao basis");
            if (name != "ao basis") {
                if (name == "cd basis")
                    settings["ri_j"] = true;
                else
                    notes << tr("名前付きの基底「%1」は取り込みません。").arg(name);
                continue;
            }
            for (const auto &option : w.mid(1))
                if (option.toLower() == "spherical" || option.toLower() == "cartesian")
                    settings["basis_functions"] = option.toLower();
            const auto known = [](const QString &basis) {
                for (const auto &c : nwchemChoices("basis"))
                    if (c.value == basis)
                        return true;
                return false;
            };
            for (const auto &line : body) {
                const auto row = words(line);
                if (row.size() < 3 || row[1].toLower() != "library")
                    continue;
                const auto library = row.last().toLower();
                if (row[0].startsWith("bq", Qt::CaseInsensitive))
                    continue; // Ghost entries follow the element's basis.
                if (!known(library)) {
                    notes << tr("基底「%1」（%2）はMolFormaの一覧にないため取り込みません。")
                                 .arg(library, row[0]);
                    continue;
                }
                if (row[0] == "*")
                    settings["basis"] = library;
                else {
                    bool ghost;
                    const auto symbol = tagElement(row[0], ghost);
                    if (!symbol.isEmpty())
                        overrides[symbol] = library;
                }
            }
        } else if (keyword == "scf") {
            for (const auto &line : body) {
                const auto row = words(line);
                const auto option = row.value(0).toLower();
                const QStringList spins{"singlet", "doublet", "triplet", "quartet",
                                        "quintet", "sextet",  "septet",  "octet"};
                if (option == "rhf" || option == "uhf" || option == "rohf")
                    settings["reference"] = option;
                else if (spins.contains(option))
                    multiplicity = spins.indexOf(option) + 1;
                else if (option == "nopen")
                    multiplicity = std::clamp(row.value(1).toInt(), 0, 1000) + 1;
                else if (option == "maxiter")
                    settings["scf_maxiter"] = row.value(1).toInt();
                else if (option == "thresh")
                    scfThresholds << row.value(1);
            }
        } else if (keyword == "dft") {
            for (const auto &line : body) {
                const auto row = words(line);
                const auto option = row.value(0).toLower();
                if (option == "xc") {
                    const auto xc = row.mid(1).join(' ').toLower().simplified();
                    QString match = "custom";
                    for (const auto &c : nwchemChoices("functional"))
                        if (nwchemFunctionalXc(c.value).toLower() == xc)
                            match = c.value;
                    settings["functional"] = match;
                    if (match == "custom")
                        settings["xc_custom"] = row.mid(1).join(' ');
                } else if (option == "mult")
                    multiplicity = row.value(1).toInt();
                else if (option == "odft")
                    settings["reference"] = "uhf";
                else if (option == "iterations")
                    settings["scf_maxiter"] = row.value(1).toInt();
                else if (option == "grid") {
                    for (const auto &value : row.mid(1))
                        if (QStringList{"coarse", "medium", "fine", "xfine", "huge"}.contains(
                                value.toLower()))
                            settings["grid"] = value.toLower();
                } else if (option == "convergence") {
                    for (int k = 1; k + 1 < row.size(); ++k)
                        if (QStringList{"energy", "density", "gradient"}.contains(row[k].toLower()))
                            dftThresholds << row[k + 1];
                } else if (option == "disp")
                    for (int k = 1; k + 1 < row.size(); ++k)
                        if (row[k].toLower() == "vdw")
                            settings["dispersion"] = row[k + 1] == "3"   ? "d3"
                                                     : row[k + 1] == "4" ? "d3bj"
                                                                         : "none";
            }
            if (settings["functional"] == "cam-b3lyp" &&
                !body.join('\n').contains("cam", Qt::CaseInsensitive))
                notes << tr(
                    "CAM-B3LYPの交換項に対応するcamパラメータがありません。汎関数を確認してください。");
        } else if (keyword == "mp2" || keyword == "ccsd") {
            // "freeze atomic" (or "freeze core atomic") is the frozen-core choice; "freeze 0" is none.
            for (const auto &line : body) {
                const auto row = words(line);
                if (keyword == "ccsd" && row.value(0).toLower() == "maxiter")
                    settings["cc_maxiter"] = row.value(1).toInt();
                if (row.value(0).toLower() != "freeze")
                    continue;
                double n;
                if (row.mid(1).join(' ').toLower().contains("atomic"))
                    settings["freeze_core"] = true;
                else if (number(row.last(), n) && n > 0)
                    notes << tr("凍結軌道数（freeze %1）は取り込みません。「2 計算条件」の詳細設定"
                                "（凍結する占有軌道数）で指定してください。")
                                 .arg(n);
            }
        } else if (keyword == "tddft") {
            settings["tddft"] = true;
            for (const auto &line : body) {
                const auto row = words(line);
                const auto option = row.value(0).toLower();
                if (option == "nroots")
                    settings["tddft_roots"] = row.value(1).toInt();
                else if (option == "cis")
                    settings["tddft_tda"] = true;
                else if (option == "notriplet" || option == "singlet")
                    settings["tddft_triplets"] = false;
            }
            if (!body.join('\n').contains("notriplet", Qt::CaseInsensitive))
                settings["tddft_triplets"] = true;
        } else if (keyword == "cosmo") {
            settings["solvent"] = "cosmo";
            for (const auto &line : body) {
                const auto row = words(line);
                const auto option = row.value(0).toLower();
                double value;
                if (option == "dielec" && number(row.value(1), value))
                    settings["dielectric"] = value;
                else if (option == "solvent")
                    settings["solvent_name"] = row.value(1).toLower();
                else if (option == "do_cosmo_smd" && row.value(1).toLower().startsWith('t'))
                    settings["solvent"] = "smd";
            }
        } else if (keyword == "relativistic") {
            const auto block = body.join('\n').toLower();
            settings["relativistic"] = block.contains("zora")            ? "zora"
                                       : block.contains("douglas-kroll") ? "dkh"
                                       : block.contains("x2c")           ? "x2c"
                                                                         : "none";
        } else if (keyword == "driver") {
            for (const auto &line : body) {
                const auto row = words(line);
                const auto option = row.value(0).toLower();
                if (option == "maxiter")
                    settings["opt_maxiter"] = row.value(1).toInt();
                else if (option == "tight" || option == "loose")
                    settings["opt_convergence"] = option;
            }
        } else if (keyword == "property") {
            for (const auto &line : body) {
                const auto option = words(line).value(0).toLower();
                if (option == "mulliken")
                    settings["prop_mulliken"] = true;
                else if (option == "shielding")
                    settings["prop_nmr"] = true;
                else if (option == "aoresponse")
                    settings["prop_polarizability"] = true;
            }
        } else if (keyword == "constraints") {
            for (const auto &line : body) {
                const auto row = words(line);
                if (row.value(0).toLower() == "fix" && row.value(1).toLower() == "atom") {
                    for (const auto &item : row.mid(2)) {
                        // Atom numbers and ranges such as 1:5.
                        const int first = item.section(':', 0, 0).toInt(),
                                  last = item.section(':', -1).toInt();
                        for (int n = first; n >= 1 && n <= last && n <= 100000 && fixedBudget > 0;
                             ++n, --fixedBudget)
                            fixedAtoms.insert(n);
                    }
                } else if (!QStringList{"clear", ""}.contains(row.value(0).toLower()))
                    notes << tr("constraintsの指示を取り込みません: %1").arg(line.left(60));
            }
        } else if (keyword == "bsse") {
            settings["bsse"] = true;
            notes << tr("BSSEの単量体はMolFormaが結合から決め直します。");
        } else if (keyword == "mepgs") {
            for (const auto &line : body) {
                const auto row = words(line);
                double value;
                if (row.value(0).toLower() == "maxmep")
                    settings["irc_points"] = row.value(1).toInt();
                else if (row.value(0).toLower() == "maxiter")
                    settings["opt_maxiter"] = row.value(1).toInt();
                else if (row.value(0).toLower() == "stride" && number(row.value(1), value))
                    settings["irc_stride"] = value;
                else if (row.value(0).toLower() == "forward" || row.value(0).toLower() == "backward")
                    settings["irc_direction"] = row.value(0).toLower();
            }
        } else if (keyword == "task") {
            tasks << w.mid(1).join(' ').toLower();
        } else if (keyword == "ecp") {
            // MolForma chooses the ECP that belongs to the library basis.
        } else if (blockNames.contains(keyword))
            notes << tr("%1ブロックは取り込みません。").arg(keyword);
        else if (!QStringList{"echo", "print", "noprint", "scratch_dir", "permanent_dir"}.contains(keyword))
            notes << tr("取り込まない指示: %1").arg(lines[i].left(80));
    }
    if (d.atoms.isEmpty())
        fail(tr("NWChem入力にCartesian座標のGEOMETRYがありません。"));
    // Tasks: theory and operation; optimization followed by frequencies becomes one choice.
    QStringList operations;
    QString theory;
    for (const auto &task : tasks) {
        const auto parts = task.split(' ', Qt::SkipEmptyParts);
        const auto first = parts.value(0), operation = parts.value(1, "energy");
        if (first == "esp") {
            settings["prop_esp"] = true;
            continue;
        }
        if (first == "tddft") {
            settings["tddft"] = true;
            theory = "dft";
            continue;
        }
        if (!QStringList{"scf", "dft", "mp2", "ccsd", "ccsd(t)"}.contains(first)) {
            notes << tr("取り込まないtask: %1").arg(task);
            continue;
        }
        if (theory.isEmpty())
            theory = first;
        else if (theory != first && operation != "property")
            notes << tr("手法の異なるtask（%1）は取り込みません。").arg(task);
        if (operation != "property")
            operations << (operation.startsWith("freq") ? "frequencies" : operation);
        if (operation.startsWith("freq") && parts.contains("numerical"))
            settings["numerical_frequencies"] = true;
    }
    if (!theory.isEmpty())
        settings["method"] = theory == "scf" ? "hf" : theory;
    // SCF convergence: the level whose thresholds the method's block sets; NWChem's own without any.
    if (const auto &values = settings["method"] == "dft" ? dftThresholds : scfThresholds; values.isEmpty())
        settings["scf_convergence"] = "default";
    else {
        QString level;
        for (const auto *candidate : {"normal", "tight", "verytight"}) {
            const double target = nwchemScfThreshold({{"scf_convergence", candidate}}).toDouble();
            if (std::all_of(values.begin(), values.end(), [&](const QString &value) {
                    double n;
                    return number(value, n) && std::abs(n / target - 1) < 1e-9;
                }))
                level = candidate;
        }
        if (level.isEmpty())
            notes << tr("SCFの収束閾値（%1）は取り込みません。「2 計算条件」の詳細設定で指定してください。")
                         .arg(values.join(' '));
        else
            settings["scf_convergence"] = level;
    }
    if (operations.contains("mepgs"))
        settings["task"] = "irc";
    else if (operations.contains("saddle"))
        settings["task"] = operations.contains("frequencies") ? "saddle_frequencies" : "saddle";
    else if (operations.contains("optimize"))
        settings["task"] = operations.contains("frequencies") ? "optimize_frequencies" : "optimize";
    else if (operations.contains("frequencies"))
        settings["task"] = "frequencies";
    else if (operations.contains("gradient"))
        settings["task"] = "gradient";
    if (!overrides.isEmpty())
        settings["basis_overrides"] = overrides;
    if (multiplicity > 0)
        d.multiplicity = multiplicity;
    // Store only values that differ from the defaults, as the calculation tab does.
    QJsonObject stored;
    const auto defaults = nwchemDefaults();
    for (auto it = settings.begin(); it != settings.end(); ++it)
        if (defaults[it.key()] != it.value())
            stored[it.key()] = it.value();
    d.data["nwchem"] = stored;
    // Bond orders from the geometry, as for XYZ files (ghosts and masses survive the round trip).
    auto shaped = xyzStructure(d.xyz());
    d.atoms = shaped.atoms;
    d.bonds = shaped.bonds;
    for (int n : fixedAtoms)
        if (atomOfCenter.contains(n))
            d.atoms[atomOfCenter[n]].fixed = true;
    notes.removeDuplicates();
    return result;
}
} // namespace mf
