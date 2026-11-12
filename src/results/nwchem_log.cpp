// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/geometry.h"
#include "model/i18n.h"
#include "model/numbers.h"
#include "model/units.h"
#include "results/result_parsing.h"
#include "results/results.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace mf {
using namespace result_detail;
namespace {
// "Total SCF energy = -76.02" (DFT, MP2, CCSD, CCSD(T)): the method and the number as written.
std::optional<std::pair<QString, QString>> totalEnergy(const QString &line) {
    static const QRegularExpression total(
        R"(\bTotal\s+(SCF|DFT|MP2|CCSD(?:\(T\))?)\s+energy\s*[:=]?\s*([-+0-9.eEdD]+))",
        QRegularExpression::CaseInsensitiveOption);
    if (!line.contains(u"energy", Qt::CaseInsensitive))
        return std::nullopt;
    const auto m = total.match(line);
    if (!m.hasMatch())
        return std::nullopt;
    return std::pair{m.captured(1).toUpper(), m.captured(2)};
}
} // namespace
bool NwchemEnergySummary::relevant(QByteArrayView line) {
    return line.contains("energy") || line.contains("NWChem Input Module") ||
           line.contains("Frequency Analysis") || line.contains("Raman Analysis") ||
           line.contains("Total times") || line.contains("number of monomers") || line.contains("BSSE error");
}
void NwchemEnergySummary::add(const QString &line) {
    static const QRegularExpression corrected(R"(^\s*Corrected energy\s*=\s*([-+0-9.eEdD]+)\s*$)");
    static const QRegularExpression supermolecular(R"(^\s*Supermolecular energy\s*=\s*([-+0-9.eEdD]+)\s*$)");
    static const QRegularExpression bsseError(R"(^\s*BSSE error\s*=\s*([-+0-9.eEdD]+)\s*$)");
    static const QRegularExpression monomers(R"(^\s*number of monomers\s*=\s*(\d+)\s*$)");
    const auto read = [](const QString &text, double &value) {
        const auto n = resultNumber(text);
        value = n.value_or(0);
        return n.has_value();
    };
    auto &task = tasks.last();
    double value;
    if (line.contains(u"NWChem Input Module")) {
        if (task.energy || task.frequency || task.raman)
            tasks.append(Task{});
    } else if (line.contains(u"Raman Analysis")) {
        task.raman = true;
    } else if (line.contains(u"NWChem Nuclear Hessian and Frequency Analysis"))
        task.frequency = true;
    else if (line.contains(u"Total times  cpu:"))
        ended = true;
    else if (const auto n =
                 line.contains(u"number of monomers") ? monomers.match(line) : QRegularExpressionMatch();
             n.hasMatch())
        task.monomers = std::min(n.captured(1).toInt(), 1000);
    else if (const auto e = line.contains(u"BSSE error") ? bsseError.match(line) : QRegularExpressionMatch();
             e.hasMatch() && read(e.captured(1), value))
        task.nwchemError = value;
    else if (!line.contains(u"energy"))
        return;
    else if (const auto m = totalEnergy(line); m && read(m->second, value)) {
        if (!task.energy) {
            task.first = value;
            task.firstMethod = m->first;
        }
        task.last = value;
        task.lastMethod = m->first;
        task.energy = true;
        if (task.monomers && task.since.size() < 20000)
            task.since << qMakePair(task.lastMethod, value);
    } else if (const auto s = supermolecular.match(line); s.hasMatch() && read(s.captured(1), value))
        task.supermolecular = value;
    else if (const auto c = corrected.match(line); c.hasMatch() && read(c.captured(1), value)) {
        task.corrected = value;
        task.error = task.nwchemError;
        task.bsse = true;
        // NWChem 7.3.1 adds the errors of only the first half of the monomers (task_bsse.F sums
        // "do j = 1, nmon, 2" over the 2·nmon monomer energies). After the supermolecule's energy
        // come those of each monomer in its own basis and in the full one, in the same method.
        const auto start = std::find_if(task.since.begin(), task.since.end(), [&](const auto &e) {
            return std::abs(e.second - task.supermolecular) < 1e-9;
        });
        QVector<double> monomer;
        if (start != task.since.end())
            for (auto it = start + 1; it != task.since.end(); ++it)
                if (it->first == start->first)
                    monomer << it->second;
        task.monomerSum = qQNaN();
        if (task.monomers > 0 && monomer.size() == 2 * task.monomers) {
            task.error = task.monomerSum = 0;
            for (int k = 0; k < task.monomers; ++k) {
                task.error += monomer[2 * k] - monomer[2 * k + 1];
                task.monomerSum += monomer[2 * k];
            }
            task.corrected = task.supermolecular + task.error;
        }
        task.since.clear();
    }
}
QJsonObject NwchemEnergySummary::energy() const {
    for (auto it = tasks.crbegin(); it != tasks.crend(); ++it) {
        if (!it->energy || (it->raman && !it->frequency))
            continue;
        if (it->bsse)
            return {{"hartree", it->corrected}, {"method", it->lastMethod}, {"source", "bsse_corrected"}};
        if (it->frequency)
            return {{"hartree", it->first}, {"method", it->firstMethod}, {"source", "frequency_reference"}};
        return {{"hartree", it->last}, {"method", it->lastMethod}, {"source", "final"}};
    }
    return {};
}
QJsonObject NwchemEnergySummary::bsse() const {
    for (auto it = tasks.crbegin(); it != tasks.crend(); ++it) {
        if (!it->bsse)
            continue;
        QJsonObject o{
            {"error", it->error}, {"supermolecular", it->supermolecular}, {"corrected", it->corrected}};
        if (std::abs(it->error - it->nwchemError) > 1e-9)
            o["nwchem_error"] = it->nwchemError;
        if (std::isfinite(it->monomerSum)) {
            o["interaction"] = it->corrected - it->monomerSum;
            o["interaction_uncorrected"] = it->supermolecular - it->monomerSum;
        }
        return o;
    }
    return {};
}
namespace {
// Display text only; the outcome is stored separately from the translated status.
const char *const IncompleteStatus = QT_TR_NOOP("正常終了マーカーなし（実行中・中断・不完全なログの可能性）"),
                  *const ErrorStatus = QT_TR_NOOP("NWChemエラー: %1"),
                  *const MessageStatus = QT_TR_NOOP("エラー・未収束メッセージあり（ログを確認）"),
                  *const NormalStatus =
                      QT_TR_NOOP("正常終了マーカーあり（各taskの結果は「概要」とログで確認）");
// A DRIVER step ("@ 3 -76.02 -1.2D-05 0.0003 ..."), and the thresholds printed before the steps.
const QRegularExpression &stepLine() {
    static const QRegularExpression pattern(R"(^@\s+(\d+)((?:\s+\S+){7})\s*$)");
    return pattern;
}
const QRegularExpression &thresholdLine() {
    static const QRegularExpression pattern(
        R"(^\s*(?:maximum|rms) (?:gradient|cartesian step) threshold\s+\((gmax|grms|xmax|xrms)\)\s*=\s*(\S+))");
    return pattern;
}
ResultStep resultStep(const QRegularExpressionMatch &m) {
    const auto columns = words(m.captured(2));
    return {m.captured(1).toInt(), logNumber(columns[0]), logNumber(columns[1]), logNumber(columns[2]),
            logNumber(columns[3]), logNumber(columns[4]), logNumber(columns[5]), logNumber(columns[6])};
}
// The largest amplitudes of an excited state, in the order they had.
void keepLargest(QVector<ResultContribution> &list) {
    if (list.size() <= ResultExcitation::maxContributions)
        return;
    QVector<double> sizes;
    for (const auto &c : std::as_const(list))
        sizes << std::abs(c.coefficient);
    std::sort(sizes.begin(), sizes.end(), std::greater<>());
    const double least = sizes[ResultExcitation::maxContributions - 1];
    QVector<ResultContribution> kept;
    for (const auto &c : std::as_const(list))
        if (std::abs(c.coefficient) >= least && kept.size() < ResultExcitation::maxContributions)
            kept << c;
    list = kept;
}
// The bond indices of the Mulliken analysis. NWChem prints the whole matrix, eight columns at a time
// with all the rows of each ("  1- 9   0.00972"). Rows and columns are counted here: the two digits
// printed for them are "**" from the 100th atom on.
class BondIndexTable {
  public:
    // A row of the matrix; false for any other line, which ends the table.
    bool add(const QString &line) {
        static const QRegularExpression entry(R"(\s*(\d+|\*+)-\s*(\d+|\*+)\s*(-?\d+\.\d+))");
        QRegularExpressionMatch entries[8];
        int count = 0;
        qsizetype at = 0;
        for (; count < 8; ++count) {
            entries[count] = entry.match(line, at, QRegularExpression::NormalMatch,
                                         QRegularExpression::AnchorAtOffsetMatchOption);
            if (!entries[count].hasMatch())
                break;
            at = entries[count].capturedEnd();
        }
        if (count == 0 || !QStringView(line).mid(at).trimmed().isEmpty())
            return false;
        // The first row again: the next eight columns.
        if (row > 0 && entries[0].capturedView(1) == u"1") {
            if (columns != 8 || (atoms > 0 && row != atoms))
                return valid = false;
            atoms = row;
            row = 0;
            ++block;
        }
        if (++row == 1)
            columns = count;
        if (count != columns || row > maxAtoms || (atoms > 0 && (row > atoms || 8 * block + count > atoms)))
            return valid = false;
        const auto labelled = [](QStringView label, int number) {
            return label.startsWith('*') || label == QString::number(number);
        };
        for (int k = 0; k < count; ++k) {
            const int column = 8 * block + k + 1;
            const auto value = fortranNumber(entries[k].captured(3));
            if (!labelled(entries[k].capturedView(1), row) || !labelled(entries[k].capturedView(2), column) ||
                !value)
                return valid = false;
            if (row >= column)
                continue; // The matrix is symmetric, with zeros on its diagonal.
            ++pairs;
            const Pair pair{row, column, *value};
            if (std::abs(*value) >= NotableBondIndex && notable.size() < maxBondIndices)
                notable << pair;
            if (!large && all.size() == maxBondIndices) {
                large = true;
                all = {};
            }
            if (!large)
                all << pair;
        }
        return true;
    }
    // The table into the properties, if all of it was read: every pair, or the notable ones of a
    // large molecule. Returns the number of atoms, 0 for a table that is not complete.
    int store(QJsonObject &properties) const {
        const int count = atoms > 0 ? atoms : row;
        if (!valid || count < 1 || row != count || 8 * block + columns != count ||
            pairs != qint64(count) * (count - 1) / 2)
            return 0;
        auto list = large ? notable : all;
        std::sort(list.begin(), list.end(),
                  [](const Pair &p, const Pair &q) { return p.a != q.a ? p.a < q.a : p.b < q.b; });
        QJsonArray indices;
        for (const auto &pair : std::as_const(list))
            indices << QJsonObject{{"a", pair.a}, {"b", pair.b}, {"value", pair.value}};
        if (count > 1)
            properties["bond_indices"] = indices;
        else
            properties.remove("bond_indices"); // An atom has no pairs.
        if (large)
            properties["bond_indices_omitted"] = double(pairs - list.size());
        else
            properties.remove("bond_indices_omitted");
        return count;
    }

  private:
    struct Pair {
        int a, b; // Atom numbers from 1, a < b.
        double value;
    };
    QVector<Pair> all, notable; // `all` while the pairs are at most maxBondIndices.
    bool valid = true, large = false;
    int atoms = 0, row = 0, block = 0, columns = 0; // The row and the columns of the last line.
    qint64 pairs = 0;
};
} // namespace
NwchemLog::NwchemLog(ResultData *data) : result(data) {
    if (result) {
        result->status = tr(IncompleteStatus);
        result->outcome = ResultOutcome::Incomplete;
    }
}
void NwchemLog::add(const QString &line) {
    ++lineNumber;
    // Sections begun by earlier lines see this one first: it may end them.
    for (int i = 0; i < sections.size();)
        if (sections[i](&line))
            ++i;
        else
            sections.removeAt(i);
    energySummary.add(line);
    progress(line);
    if (result)
        read(line);
    recent << line;
    if (recent.size() > 4)
        recent.removeFirst();
}
void NwchemLog::addBytes(QByteArrayView line) {
    // What progress() and the energy summary look for (a DRIVER step begins with '@'; total energies
    // are found case-insensitively).
    const auto energy = [&] {
        static constexpr char word[] = "energy";
        for (qsizetype i = 0; i + 6 <= line.size(); ++i) {
            qsizetype k = 0;
            while (k < 6 && (line[i + k] | 0x20) == word[k])
                ++k;
            if (k == 6)
                return true;
        }
        return false;
    };
    if (!result && !line.startsWith('@') && !line.contains("threshold") &&
        !line.contains("Optimization converged") && !line.contains("NWChem Geometry Optimization") &&
        !line.contains("NWChem Input Module") && !line.contains("Total times") &&
        !line.contains("Frequency Analysis") && !line.contains("number of monomers") &&
        !line.contains("BSSE error") && !energy()) {
        ++lineNumber;
        return;
    }
    add(QString::fromUtf8(line));
}
void NwchemLog::beginOptimization() {
    if (!driver.present || driver.task != started - 1) {
        driver = {};
        driver.task = started - 1;
        driver.present = true;
    }
}
void NwchemLog::progress(const QString &line) {
    if (line.contains(u"NWChem Input Module")) {
        // NWChem reads the input again before each task; repeated banners start no new task.
        if (started == 0 || busy)
            ++started;
        busy = false;
    } else if (line.contains(u"Total times  cpu:"))
        ended = true;
    else if (line.contains(u"NWChem Geometry Optimization")) {
        driver = {};
        beginOptimization();
        busy = true;
    } else if (const auto m = line.startsWith('@') ? stepLine().match(line) : QRegularExpressionMatch();
               m.hasMatch()) {
        const auto s = resultStep(m);
        beginOptimization();
        // NWChem repeats the converged step, including step zero of an already stationary structure.
        const bool repeated = !driver.steps.isEmpty() && driver.steps.last() == s;
        if (s.step == 0 && !repeated) {
            driver.steps.clear();
            driver.converged = false;
        }
        while (!driver.steps.isEmpty() && driver.steps.last().step >= s.step)
            driver.steps.removeLast();
        if (std::isfinite(s.energy) && driver.steps.size() < 20000)
            driver.steps << s;
        busy = true;
    } else if (const auto t =
                   line.contains(u"threshold") ? thresholdLine().match(line) : QRegularExpressionMatch();
               t.hasMatch()) {
        beginOptimization();
        driver.thresholds[t.captured(1)] = logNumber(t.captured(2));
        busy = true;
    } else if (line.contains(u"Optimization converged")) {
        beginOptimization();
        driver.converged = true;
        busy = true;
    } else if (const auto e = totalEnergy(line)) {
        if (const double value = logNumber(e->second); std::isfinite(value)) {
            energy = value;
            energyMethod = e->first;
        }
        busy = true;
    } else if (line.contains(u"Frequency Analysis"))
        busy = true;
}
void NwchemLog::read(const QString &line) {
    auto &r = *result;
    static const QRegularExpression error(
        "failed to converge|convergence failure|not converged|MPI_ABORT|An error occurred|"
        "error termination|task.*failed|failed.*task|Calculation failed|There is an error in|"
        "This error has not yet been assigned|This type of error is most commonly associated|"
        "Received an Error in Communication",
        QRegularExpression::CaseInsensitiveOption);
    // Every message of `error` contains one of these words.
    static constexpr QStringView errorWords[] = {u"fail", u"error", u"converged", u"mpi_abort"};
    static const QRegularExpression code(R"(\s+-?\d+\s*$)");
    static const QRegularExpression orbitalHeader(R"(Final (Alpha |Beta )?Molecular Orbital Analysis)");
    static const QRegularExpression vector(
        R"(^\s*Vector\s+(\d+)\s+Occ=\s*(\S+)\s+E=\s*(\S+)(?:\s+Symmetry=\s*(\S+))?)");
    // Markers written by MolForma's scan input ("title" of each point) and IRC point lines.
    // A point of a two-dimensional scan names both coordinates: "… = 1.5 and torsion 1 2 3 4 = 60".
    static const QRegularExpression scanMark(
        R"(MolForma scan (\d+)/(\d+) ([a-z]+(?: \d+)+) = ([-+.0-9eE]+)(?: and ([a-z]+(?: \d+)+) = ([-+.0-9eE]+))?)");
    static const QRegularExpression spinPair(
        R"(^\s*Atom\s+(\d+):\s+(\d+-[A-Za-z]+)\s+and\s+Atom\s+(\d+):\s+(\d+-[A-Za-z]+))");
    static const QRegularExpression ircPoint(R"(^&\s+(\d+)((?:\s+\S+){6})\s*$)");
    static const QRegularExpression atomRow(R"(^\s*(\d+)\s+([A-Za-z]{1,3}\w*)\s+(\S+)\s+(\S+)\s*$)");
    static const QRegularExpression index("^\\d+$");
    // Lenient numbers for the summaries: one odd line must not reject the whole log.
    const auto value = [](const QString &text) { return logNumber(text); };
    const int i = lineNumber;
    // Each expression below matches only lines with a word or first character checked before it.
    const auto has = [&](QStringView word) { return line.contains(word); };
    const auto unmatched = QRegularExpressionMatch();
    recognized |= line.contains(u"NWChem");
    if (line.contains(u"Spin multiplicity:"))
        r.properties["spin_multiplicity"] = value(words(line.section(':', 1)).value(0));
    if (line.contains(u"ZORA NMR Hyperfine"))
        r.properties["epr_hyperfine"] = QJsonArray();
    if (line.contains(u"Total hyperfine coupling tensor:")) {
        const int atom = shieldingAtom;
        const auto element = shieldingElement;
        shieldingAtom = 0;
        sections << [&r, atom, element, tensor = QJsonArray(), left = 6](const QString *text) mutable {
            if (!text || --left < 0)
                return false;
            const auto tokens = words(*text);
            if (tokens.size() != 6 || !fortranNumber(tokens[0]))
                return true;
            for (int k = 3; k < 6; ++k) {
                const auto n = fortranNumber(tokens[k]);
                if (!n)
                    return false;
                tensor.append(*n);
            }
            if (tensor.size() < 9)
                return true;
            auto rows = r.properties["epr_hyperfine"].toArray();
            const QJsonObject row{{"atom", atom}, {"element", element}, {"tensor_mhz", tensor}};
            bool replaced = false;
            for (int i = 0; i < rows.size(); ++i)
                if (rows[i].toObject()["atom"] == atom) {
                    rows[i] = row;
                    replaced = true;
                    break;
                }
            if (!replaced && atom > 0 && rows.size() < 4096)
                rows.append(row);
            r.properties["epr_hyperfine"] = rows;
            return false;
        };
    }
    if (line.contains(u"Total g shift Tensor")) {
        shieldingAtom = 0; // This isotropic value belongs to g, never to the previous atom's shielding.
        r.properties.remove("epr_gshift_ppt");
        sections << [&r, tensor = QJsonArray()](const QString *text) mutable {
            if (!text)
                return false;
            const auto tokens = words(*text);
            if (tokens.size() != 3)
                return false;
            for (const auto &token : tokens) {
                const auto n = fortranNumber(token);
                if (!n)
                    return false;
                tensor.append(*n);
            }
            if (tensor.size() < 9)
                return true;
            r.properties["epr_gshift_ppt"] = tensor;
            return false;
        };
    }
    if (line.contains(u"Showing rminfo:"))
        r.properties["raman"] = QJsonArray();
    if (line.trimmed().startsWith(u"rminfo(")) {
        static const QRegularExpression raman(
            R"(^\s*rminfo\(\s*(\d+)\)\s*=\(\s*([^,]+),[^,]+,[^,]+,([^\)]+)\))");
        const auto match = raman.match(line);
        if (match.hasMatch()) {
            auto rows = r.properties["raman"].toArray();
            if (rows.size() < 4096)
                rows.append(QJsonObject{{"raman_index", match.captured(1).toInt()},
                                        {"frequency", value(match.captured(2))},
                                        {"activity", value(match.captured(3))}});
            r.properties["raman"] = rows;
        }
    }
    // errquit prints its message, framed by dashes, just above "current input line :".
    if (QStringView(line).trimmed().startsWith(u"current input line") && abort.isEmpty())
        for (int j = recent.size() - 1; j >= 0; --j) {
            const auto text = recent[j].trimmed();
            if (text.isEmpty() || text.startsWith(u"----"))
                continue;
            abort = QString(text).remove(code).left(200);
            fatal = true;
            warn(r, tr("行 %1: NWChemエラー: %2").arg(i - recent.size() + j).arg(abort));
            break;
        }
    // An NEB that reached its step limit ends normally ("@neb NEB calculation not converged"): told apart
    // below, as the optimization of the path rather than an error.
    const bool troubled = !line.startsWith(u"@neb") &&
                          std::any_of(std::begin(errorWords), std::end(errorWords), [&](QStringView word) {
                              return line.contains(word, Qt::CaseInsensitive);
                          });
    if (troubled && error.match(line).hasMatch()) {
        fatal = true;
        if (r.warnings.size() < 30)
            r.warnings << tr("行 %1: %2").arg(i).arg(line.trimmed().left(250));
    }
    if (auto m = has(u"MolForma scan") ? scanMark.match(line) : unmatched;
        m.hasMatch() && m.captured(1).toInt() > scanPoint) {
        scanPoint = m.captured(1).toInt();
        if (scan.size() < 1000)
            scan[scanPoint] = QJsonObject{{"point", scanPoint},
                                          {"points", m.captured(2).toInt()},
                                          {"coordinate", m.captured(3)},
                                          {"value", value(m.captured(4))}};
        if (scan.contains(scanPoint) && m.hasCaptured(5)) {
            scan[scanPoint]["coordinate2"] = m.captured(5);
            scan[scanPoint]["value2"] = value(m.captured(6));
        }
    }
    // NWChem's NEB: the step, the energies of the path after each step, the path itself when the
    // input asks to print it (print_shift), the "@neb" table and the verdict.
    if (line.contains(u"neb") || line.contains(u"XYZ FILE for bead_list")) {
        const auto tokens = words(line);
        if (tokens.size() == 4 && tokens[0] == "neb:" && tokens[1] == "iteration" && tokens[2] == "#")
            nebIteration = int(value(tokens[3]));
        if (line.contains(u"neb: Initial Path Energy") || line.contains(u"neb: Path Energy #")) {
            const int iteration = line.contains(u"Initial") ? 0 : int(value(tokens.value(4)));
            sections << [this, iteration, energies = QVector<double>()](const QString *text) mutable {
                const auto row = text ? words(*text) : QStringList();
                if (row.size() == 3 && row[0] == "neb:" && row[1] == QString::number(energies.size() + 1)) {
                    if (const auto e = resultNumber(row[2]); e && energies.size() < 1000) {
                        energies << *e;
                        return true;
                    }
                } else if (energies.isEmpty() && row.size() == 2 && row[1].startsWith(u"---"))
                    return true;
                if (!energies.isEmpty()) {
                    nebEnergies = energies;
                    nebEnergiesIteration = iteration;
                }
                return false;
            };
        }
        if (line.contains(u"XYZ FILE for bead_list"))
            sections << [this, iteration = nebIteration, path = QVector<Document>(),
                         energies = QVector<double>(), bead = Document(), atoms = -1, energy = qQNaN(),
                         heading = true](const QString *text) mutable {
                const auto finish = [&] {
                    if (!path.isEmpty() && path.size() == energies.size()) {
                        nebPath = path;
                        nebPathEnergies = energies;
                        nebPathIteration = iteration;
                    }
                    return false;
                };
                if (!text)
                    return finish();
                const auto row = words(*text);
                if (heading && row.size() == 1 && row[0].startsWith(u"---")) {
                    heading = false;
                    return true;
                }
                heading = false;
                try {
                    if (atoms < 0) { // The number of atoms of the next bead, or the end of the path.
                        if (row.size() != 1 || path.size() >= 1000)
                            return finish();
                        atoms = integer(row[0], 1, maxAtoms);
                        bead = Document();
                        energy = qQNaN();
                    } else if (!std::isfinite(energy)) {
                        if (!text->trimmed().startsWith(u"energy="))
                            return finish();
                        energy = number(text->section('=', 1).trimmed());
                    } else {
                        if (row.size() != 4)
                            return finish();
                        bead.atoms << atom(row[0], point(row, 1));
                        if (bead.atoms.size() == atoms) {
                            if (!path.isEmpty() && path.first().atoms.size() != atoms)
                                return finish();
                            path << bead;
                            energies << energy;
                            atoms = -1;
                        }
                    }
                } catch (const std::exception &) {
                    return finish();
                }
                return true;
            };
        if (line.startsWith(u"@neb")) {
            static const QRegularExpression step(R"(^@neb\s+(\d+)((?:\s+\S+){9})\s*$)");
            static const QRegularExpression tolerance(R"(^@neb (Gmax|Grms|Xmax|Xrms) tolerance\s*=\s*(\S+))");
            if (const auto m = step.match(line); m.hasMatch() && nebSteps.size() < 20000) {
                const auto columns = words(m.captured(2));
                nebSteps << QJsonObject{{"step", m.captured(1).toInt()}, {"energy", value(columns[0])},
                                        {"midpoint", value(columns[1])}, {"minimum", value(columns[2])},
                                        {"maximum", value(columns[3])},  {"gmax", value(columns[4])},
                                        {"grms", value(columns[5])},     {"xrms", value(columns[6])},
                                        {"xmax", value(columns[7])}};
            } else if (const auto t = tolerance.match(line); t.hasMatch())
                nebThresholds[t.captured(1).toLower()] = value(t.captured(2));
            else if (line.contains(u"NEB calculation not converged"))
                nebConverged = 0;
            else if (line.contains(u"NEB calculation converged"))
                nebConverged = 1;
        }
    }
    if (line.contains(u"Forward IRC optimization") || line.contains(u"Backward IRC optimization")) {
        if (ircDirection.isEmpty()) {
            // The transition state: its energy and structure were computed before the path.
            if (!r.energies.isEmpty())
                irc["ts_energy"] = r.energies.last().hartree;
            irc["ts_frame"] = r.frames.size() - 1;
        }
        ircDirection = line.contains(u"Forward") ? "forward" : "backward";
    }
    ircConverged += line.contains(u"IRC Optimization converged");
    if (auto m = line.startsWith('&') ? ircPoint.match(line) : unmatched;
        m.hasMatch() && !ircDirection.isEmpty() && ircPoints.size() < 5000) {
        const auto columns = words(m.captured(2));
        ircPoints << QJsonObject{{"direction", ircDirection},
                                 {"point", m.captured(1).toInt()},
                                 {"energy", value(columns[0])},
                                 {"s", value(columns[2])},
                                 {"frame", r.frames.size() - 1}};
    }
    const auto field = [&](const char *label) {
        return value(words(line.section(label, 1).section('=', 1)).value(0));
    };
    if (line.contains(u"gas phase energy =") || line.contains(u"sol phase energy ="))
        solvation[line.contains(u"gas phase") ? "gas" : "solution"] = field("energy");
    if (line.contains(u"fixed-concentration free energy of solvation ="))
        solvation["smd_dg"] = field("solvation");
    if (line.contains(u"total free energy in solvent including G(SMD-CDS) ="))
        solvation["smd_total"] = field("G(SMD-CDS)");
    // Mulliken charges of the PROPERTY module (nuclear charge minus gross population).
    if (line.contains(u"Total      gross population on atoms"))
        sections << [&r, value, charges = QJsonArray(), left = 1 + maxAtoms](const QString *row) mutable {
            const auto m = row ? atomRow.match(*row) : QRegularExpressionMatch();
            if (row && --left >= 0) {
                if (row->trimmed().isEmpty() && charges.isEmpty())
                    return true;
                if (m.hasMatch() && m.captured(1).toInt() == charges.size() + 1) {
                    charges << value(m.captured(3)) - value(m.captured(4));
                    return true;
                }
            }
            if (!charges.isEmpty())
                r.properties["mulliken"] = charges;
            return false;
        };
    // Bond indices of the same analysis: the last complete matrix of the log counts, as for the charges.
    // A log may end anywhere (that of a running job even within a line), so that only a matrix with
    // the list of the large indices after it is complete.
    if (line.contains(u"----- Bond indices -----")) {
        bondAtoms = 0;
        sections << [this, table = BondIndexTable(), rows = false, blank = 0](const QString *text) mutable {
            if (!text)
                return false;
            if ((!rows || blank == 0) && table.add(*text)) {
                rows = true;
                blank = 0;
                return true;
            }
            if (text->trimmed().isEmpty() && ++blank <= 2)
                return true;
            if (rows && text->contains(u"Large bond indices"))
                bondAtoms = table.store(result->properties);
            return false;
        };
    }
    // Its valencies and numbers of free electrons, under the heading that ends
    // "=Mulliken charge     = Net spin population": complete with a row for each atom of the matrix.
    if (bondAtoms > 0 && line.contains(u"=Mulliken charge") && line.contains(u"Valency"))
        sections << [this, valency = QJsonArray(), free = QJsonArray()](const QString *text) mutable {
            // Number, element, valency, free electrons, sum of bond indices, population and spin.
            const auto row = text ? words(*text) : QStringList();
            if ((row.size() != 6 && row.size() != 7) || !index.match(row[0]).hasMatch() ||
                row[0].toInt() != valency.size() + 1)
                return false;
            const auto v = fortranNumber(row[row.size() - 5]), f = fortranNumber(row[row.size() - 4]);
            if (!v || !f)
                return false;
            valency << *v;
            free << *f;
            if (valency.size() < bondAtoms)
                return true;
            result->properties["valency"] = valency;
            result->properties["free_electrons"] = free;
            return false;
        };
    // ESP (and RESP with "restrain") charges of the ESP module.
    if (line.contains(u"Atom") && line.contains(u"Coordinates") && line.contains(u"Charge"))
        // The rows start within seven lines of the heading; a log repeating the heading must not keep
        // thousands of sections waiting for them.
        sections <<
            [&r, value, esp = QJsonArray(), resp = QJsonArray(), preamble = 7](const QString *text) mutable {
                if (text) {
                    const auto row = words(*text);
                    if (row.size() < 6 || !index.match(row[0]).hasMatch()) {
                        if (esp.isEmpty() && --preamble >= 0)
                            return true;
                    } else if (row[0].toInt() == esp.size() + 1 && esp.size() < maxAtoms) {
                        esp << value(row[5]);
                        if (row.size() >= 7)
                            resp << value(row[6]);
                        return true;
                    }
                }
                if (!esp.isEmpty()) {
                    r.properties["esp"] = esp;
                    if (resp.size() == esp.size())
                        r.properties["resp"] = resp;
                    else
                        r.properties.remove("resp");
                }
                return false;
            };
    // GIAO shielding tensors: "Atom:  n  X" followed by "isotropic =" and "anisotropy =".
    if (line.contains(u"Chemical Shielding Tensors")) {
        shielding = {};
        shieldingAtom = 0;
    }
    if (const auto t = QStringView(line).trimmed(); t.startsWith(u"Atom:")) {
        const auto w = words(t);
        shieldingAtom = w.value(1).toInt();
        shieldingElement = w.value(2);
    }
    if (shieldingAtom > 0 && line.contains(u"isotropic =")) {
        if (shieldingAtom == 1)
            shielding = {};              // A later NMR calculation replaces the earlier one.
        if (shielding.size() < maxAtoms) // One per atom.
            shielding << QJsonObject{{"atom", shieldingAtom},
                                     {"element", shieldingElement},
                                     {"isotropic", value(words(line.section('=', 1)).value(0))}};
    }
    if (shieldingAtom > 0 && line.contains(u"anisotropy =") && !shielding.isEmpty()) {
        auto last = shielding.last().toObject();
        last["anisotropy"] = value(words(line.section('=', 1)).value(0));
        shielding[shielding.size() - 1] = last;
        shieldingAtom = 0;
    }
    // Indirect spin–spin couplings: "Atom 1: 13-C and Atom 2: 19-F", then the total isotropic coupling.
    if (line.contains(u"Indirect Spin-Spin Tensors")) {
        spinspin = {};
        spinPairAtoms = {};
    }
    if (line.contains(u" and Atom ")) {
        if (const auto m = spinPair.match(line); m.hasMatch())
            spinPairAtoms = {m.captured(1), m.captured(2), m.captured(3), m.captured(4)};
    }
    if (spinPairAtoms.size() == 4 && line.contains(u"Isotropic Spin-Spin Coupling =")) {
        if (spinspin.size() < 20000)
            spinspin << QJsonObject{{"a", spinPairAtoms[0].toInt()},
                                    {"b", spinPairAtoms[2].toInt()},
                                    {"isotope_a", spinPairAtoms[1]},
                                    {"isotope_b", spinPairAtoms[3]},
                                    {"hz", value(words(line.section('=', 1)).value(0))}};
        spinPairAtoms = {};
    }
    if (line.contains(u"NWChem Input Module")) {
        responseGauge.clear();
        responseFrequency = 0;
    }
    if (line.contains(u"Using GIAO formalism for OptRot"))
        responseGauge = "giao";
    if (line.trimmed().startsWith("Frequency  ="))
        responseFrequency = value(words(line.section('=', 1)).value(0));
    if (line.contains(u"Optical rotation tensor Beta") && responseGauge == "giao" && responseFrequency > 0)
        sections << [&r, value, frequency = responseFrequency, tensor = QJsonArray(),
                     left = 15](const QString *text) mutable {
            if (!text || --left < 0)
                return false;
            const auto row = words(*text);
            if (row.size() == 4 && row[0].toInt() == tensor.size() / 3 + 1)
                for (int k = 1; k < 4; ++k)
                    tensor.append(value(row[k]));
            if (tensor.size() == 9 && text->trimmed().startsWith("average:")) {
                const double iso = value(row.value(1)), imaginary = value(row.value(4));
                if (std::isfinite(iso) && imaginary == 0) {
                    auto rows = r.properties["ord"].toArray();
                    if (rows.size() < 2000)
                        rows.append(QJsonObject{{"frequency_au", frequency},
                                                {"wavelength_nm", 45.5633525316 / frequency},
                                                {"isotropic", iso},
                                                {"tensor", tensor},
                                                {"unit", "au"},
                                                {"gauge", "giao"}});
                    r.properties["ord"] = rows;
                }
                return false;
            }
            return true;
        };
    if (line.contains(u"Linear Response polarizability / au"))
        sections << [&r, value, tensor = QJsonArray(), polarizability = QJsonObject(),
                     left = 15](const QString *text) mutable {
            if (text && --left >= 0) {
                const auto row = words(*text);
                if (row.size() == 4 && QStringList{"X", "Y", "Z"}.contains(row[0]))
                    for (int k = 1; k < 4; ++k)
                        tensor << value(row[k]);
                if (text->contains("Isotropic   ="))
                    polarizability["isotropic"] = value(words(text->section('=', 1)).value(0));
                if (text->contains("Anisotropic ="))
                    polarizability["anisotropy"] = value(words(text->section('=', 1)).value(0));
                if (text->contains("Frequency  ="))
                    polarizability["frequency"] = value(words(text->section('=', 1)).value(0));
                return true;
            }
            if (tensor.size() == 9 && polarizability.contains(u"isotropic")) {
                polarizability["tensor"] = tensor;
                if (!r.frames.isEmpty())
                    polarizability["geometry"] = r.frames.last().molecule.xyz();
                r.properties["polarizability"] = polarizability;
            }
            return false;
        };
    if (auto m = has(u"Molecular Orbital Analysis") ? orbitalHeader.match(line) : unmatched; m.hasMatch()) {
        orbitalSpin = m.captured(1).trimmed() == "Beta" ? "Beta" : "Alpha";
        if (orbitalSpin == "Alpha")
            orbitals.clear(); // Keep the last analysis (after optimization steps etc.).
    }
    if (auto m = has(u"Vector") ? vector.match(line) : unmatched;
        m.hasMatch() && !orbitalSpin.isEmpty() && orbitals.size() < 20000)
        orbitals << ResultOrbital{m.captured(4), orbitalSpin, value(m.captured(3)), value(m.captured(2)),
                                  m.captured(1).toInt()};
    if (line.contains(u"Multipole analysis of the density"))
        sections << [&r, value, dipole = QJsonObject(), left = 11](const QString *text) mutable {
            if (text && --left >= 0) {
                const auto row = words(*text);
                if (row.size() >= 5 && row[0] == "1" &&
                    QStringList{"100", "010", "001"}.contains(row.mid(1, 3).join("")))
                    dipole[row[1] == "1" ? "x" : row[2] == "1" ? "y" : "z"] = value(row[4]);
                return true;
            }
            if (dipole.size() == 3) {
                const double au =
                    std::hypot(dipole["x"].toDouble(), dipole["y"].toDouble(), dipole["z"].toDouble());
                dipole["debye"] = au * 2.541746473;
                if (!r.frames.isEmpty())
                    dipole["geometry"] = r.frames.last().molecule.xyz();
                dipole["origin"] = "nwchem_multipole_origin";
                r.properties["dipole"] = dipole;
            }
            return false;
        };
    if (QStringView(line).trimmed().startsWith(u"Temperature") && line.contains('=') &&
        QStringView(line).trimmed().endsWith('K'))
        sections << [this, value,
                     t = QJsonObject{{"temperature", value(line.section('=', 1).trimmed().chopped(1))}},
                     left = 15](const QString *text) mutable {
            if (text && --left >= 0) {
                const auto field = [&](const char *label, const char *key) {
                    if (text->contains(label))
                        t[key] = value(words(text->section('=', 1)).value(0));
                };
                field("Zero-Point correction to Energy", "zpe_kcal");     // kcal/mol
                field("Thermal correction to Energy", "energy_kcal");     // kcal/mol
                field("Thermal correction to Enthalpy", "enthalpy_kcal"); // kcal/mol
                field("Total Entropy", "entropy_cal");                    // cal/(mol K)
                field("Cv (constant volume heat capacity)", "cv_cal");    // cal/(mol K)
                // NWChem's own scale factor (vib:scalefreq), and the parts of the entropy: the heat
                // capacity, which follows them, repeats their labels.
                field("frequency scaling parameter", "nwchem_frequency_scale");
                if (!t.contains(u"cv_cal")) {
                    field("- Translational", "entropy_translational_cal");
                    field("- Rotational", "entropy_rotational_cal");
                    field("- Vibrational", "entropy_vibrational_cal");
                }
                if (text->contains("symmetry #"))
                    t["symmetry_number"] = value(text->section('=', -1).remove(')').trimmed());
                return true;
            }
            if (t.contains(u"enthalpy_kcal") && t.contains(u"entropy_cal") && thermochemistry.size() < 20)
                thermochemistry << t;
            return false;
        };
    if (line.contains(u"Optimization converged"))
        warn(r, tr("最適化の収束メッセージを検出しました。"));
    if (const auto e = totalEnergy(line)) {
        if (r.energies.size() >= 20000)
            fail(tr("エネルギーの記録数が2万件を超えています。"));
        r.energies << ResultEnergy{e->first, number(e->second), i};
        if (scanPoint > 0 && scan.contains(scanPoint))
            scan[scanPoint]["energy"] = r.energies.last().hartree;
    }
    readCoordinates(line);
    readModes(line);
    readExcitations(line);
}
void NwchemLog::readCoordinates(const QString &line) {
    auto &r = *result;
    const int i = lineNumber;
    static const QRegularExpression index("^\\d+$");
    if (line.contains(u"Output coordinates in ")) {
        const double scale = line.contains(u"angstroms") ? 1 : line.contains(u"a.u.") ? BohrToAngstrom : 0;
        if (!scale) {
            warn(r, tr("未対応の座標単位を読み飛ばしました。"));
            return;
        }
        // The table starts after the dashes under its heading (or nine lines on); a blank line ends
        // it. Only a terminated table counts, never a partial last row at the end of the log.
        const auto label =
            scanPoint > 0 ? tr("スキャン %1・ログ 行 %2").arg(scanPoint).arg(i) : tr("ログ 行 %1").arg(i);
        sections << [this, scale, label, i, scanAt = scanPoint, heading = 0,
                     d = Document()](const QString *text) mutable {
            auto &r = *result;
            if (text && heading < 10) {
                if (++heading == 10 || text->contains("----"))
                    heading = 10;
                return true;
            }
            if (text) {
                const auto row = words(*text);
                if (row.size() == 6 && index.match(row[0]).hasMatch() &&
                    integer(row[0], 1, maxAtoms) == d.atoms.size() + 1) {
                    try {
                        d.atoms << atom(row[1], point(row, 3, scale));
                        return true;
                    } catch (const std::exception &e) {
                        warn(r, QString::fromUtf8(e.what()));
                        d.atoms.clear();
                    }
                }
            }
            // A complete table of another size is another geometry, such as a monomer of a
            // counterpoise correction.
            const bool complete = !d.atoms.isEmpty() && text && text->trimmed().isEmpty();
            if (complete &&
                (r.frames.isEmpty() || r.frames.first().molecule.atoms.size() == d.atoms.size())) {
                addFrame(r, d, label);
                if (scanAt > 0 && scan.contains(scanAt))
                    scan[scanAt]["frame"] = r.frames.size() - 1;
            } else if (!complete)
                warn(r, tr("行 %1の不完全な座標表を読み飛ばしました。").arg(i));
            return false;
        };
    }
}
void NwchemLog::readModes(const QString &line) {
    auto &r = *result;
    static const QRegularExpression index("^\\d+$");
    if (line.contains(u"(Projected Frequencies expressed")) {
        r.modes.clear();
        if (!r.frames.isEmpty())
            r.vibrationGeometry = r.frames.last().molecule;
    }
    if (QStringView(line).trimmed().startsWith(u"P.Frequency") && !r.vibrationGeometry.atoms.isEmpty()) {
        const auto values = words(line).mid(1);
        // NWChem prints six modes per table and 3N in all: a longer line is not one of its tables.
        if (values.size() > 6 || r.modes.size() + values.size() > 3 * r.vibrationGeometry.atoms.size()) {
            warn(r, tr("未完の振動モード表を読み飛ばしました。"));
            return;
        }
        QVector<ResultMode> modes;
        for (const auto &f : values)
            modes.append({number(f), std::numeric_limits<double>::quiet_NaN(),
                          QVector<Vec>(r.vibrationGeometry.atoms.size(), Vec::Zero())});
        // Blank lines, then one row per Cartesian coordinate.
        sections << [&r, modes, rows = 0, blank = true](const QString *text) mutable {
            const int count = 3 * r.vibrationGeometry.atoms.size();
            if (text && blank && text->trimmed().isEmpty())
                return true;
            blank = false;
            if (text) {
                const auto row = words(*text);
                if (row.size() == modes.size() + 1 && row[0] == QString::number(rows + 1)) {
                    for (int k = 0; k < modes.size(); ++k)
                        modes[k].displacement[rows / 3][rows % 3] = number(row[k + 1]);
                    if (++rows < count)
                        return true;
                }
            }
            if (rows == count)
                r.modes += modes;
            else
                warn(r, tr("未完の振動モード表を読み飛ばしました。"));
            return false;
        };
    }
    if (line.contains(u"Projected Infra Red Intensities"))
        sections << [&r, after = 0](const QString *text) mutable {
            if (!text)
                return false;
            ++after;
            auto row = words(*text);
            if (row.size() == 7 && row[2] == "||" && index.match(row[0]).hasMatch()) {
                const int n = integer(row[0], 1, 6000) - 1;
                if (n < r.modes.size())
                    r.modes[n].intensity = number(row[5]);
                return true;
            }
            return after <= 3;
        };
}
void NwchemLog::readExcitations(const QString &line) {
    auto &r = *result;
    const auto value = [](const QString &text) { return logNumber(text); };
    const auto has = [&](QStringView word) { return line.contains(word); };
    const auto unmatched = QRegularExpressionMatch();
    static const QRegularExpression root(
        R"(^\s*Root\s+(\d+)\s+(?:(singlet|triplet)\s+)?(\S+)\s+(-?[\d.]+)\s+a\.u\.\s+(-?[\d.]+)\s+eV)");
    // "Occ.    5  a   ---  Virt.    6  a   -1.00014 X" (or Y; neither for Tamm–Dancoff states), with
    // "alpha" / "beta" after the numbers of unrestricted states. An irreducible representation of four
    // characters is not separated from a negative amplitude.
    static const QRegularExpression amplitude(
        R"(^\s*Occ\.\s*(\d+)\s+(?:(alpha|beta)\s+)?\S{0,4}\s*---\s*Virt\.\s*(\d+)\s+(?:(alpha|beta)\s+)?)"
        R"(\S{0,4}?\s*(-?\d*\.\d+)(?:\s+([XY]))?\s*$)");
    static const QRegularExpression electrons(R"(^\s*(Alpha|Beta) electrons\s*:\s*(\d+)\s*$)");
    // The electrons of each spin, as the DFT and TDDFT modules print them before the excited states.
    if (auto m = has(u"electrons :") ? electrons.match(line) : unmatched; m.hasMatch())
        (m.captured(1) == "Alpha" ? alphaElectrons : betaElectrons) = m.captured(2).toInt();
    if (auto m = has(u"Root") ? root.match(line) : unmatched; m.hasMatch() && r.excitations.size() < 2000) {
        ResultExcitation x{m.captured(1).toInt(), m.captured(2), m.captured(3), value(m.captured(5))};
        if (x.root == 1 && std::any_of(r.excitations.begin(), r.excitations.end(),
                                       [&](const auto &e) { return e.root == 1 && e.spin == x.spin; })) {
            r.excitations.clear(); // A later TDDFT task replaces the earlier one.
            r.properties.remove("ecd");
            r.properties.remove("tddft");
        }
        if (alphaElectrons >= 0 && betaElectrons >= 0)
            r.properties["tddft"] =
                QJsonObject{{"alpha_electrons", alphaElectrons}, {"beta_electrons", betaElectrons}};
        // Its <S2> and oscillator strength follow, before the next root, and then its amplitudes in one
        // block, which are not counted among the lines (a large molecule has many). A log may end
        // anywhere (that of a running job even within a line): the block is complete once a line with
        // other text follows it, after blank ones or not.
        enum { Before, Within, Closing, After };
        sections << [&r, value, x, rotation = qQNaN(), gauge = QString(), left = 80,
                     block = int(Before)](const QString *text) mutable {
            if (const auto m = text && block <= Within && text->contains(u"Virt.")
                                   ? amplitude.match(*text)
                                   : QRegularExpressionMatch();
                m.hasMatch()) {
                block = Within;
                if (const double coefficient = value(m.captured(5)); std::isfinite(coefficient))
                    x.contributions << ResultContribution{m.captured(1).toInt(), m.captured(3).toInt(),
                                                          m.captured(2), coefficient, m.captured(6) == "Y"};
                if (x.contributions.size() >= 8 * ResultExcitation::maxContributions)
                    keepLargest(x.contributions);
                return true;
            }
            if (text && (block == Within || block == Closing)) {
                const auto after = QStringView(*text).trimmed();
                block = after.isEmpty() ? Closing : After;
                if (after.startsWith(u"Occ.") || (block == After && QStringView(u"Occ.").startsWith(after)))
                    x.contributions.clear(); // An amplitude cut short.
            }
            if (text && --left >= 0 && !root.match(*text).hasMatch()) {
                if (text->contains("Magnetic Transition Dipole (Length)"))
                    gauge = "length";
                if (text->contains("Rotatory Strength (1E-40 esu**2cm**2):"))
                    rotation = value(words(*text).last());
                if (text->contains("<S2> ="))
                    x.s2 = value(words(*text).value(2));
                if (text->contains("Dipole Oscillator Strength"))
                    x.oscillator = value(words(*text).last());
                return true;
            }
            if (block == Within || block == Closing)
                x.contributions.clear(); // The log ends in them.
            keepLargest(x.contributions);
            if (std::isfinite(x.ev)) {
                if (std::isfinite(rotation) && gauge == "length") {
                    auto rows = r.properties["ecd"].toArray();
                    rows.append(QJsonObject{{"root", x.root},
                                            {"spin", x.spin},
                                            {"ev", x.ev},
                                            {"strength", rotation},
                                            {"unit", "1e-40 esu^2 cm^2"},
                                            {"gauge", gauge},
                                            {"excitation", int(r.excitations.size())}});
                    r.properties["ecd"] = rows;
                }
                r.excitations << x;
            }
            return false;
        };
    }
}
void NwchemLog::finish() {
    for (auto &section : sections)
        section(nullptr);
    sections.clear();
    if (!result)
        return;
    auto &r = *result;
    if (!recognized)
        fail(tr("NWChemの出力ログを認識できません。"));
    r.steps = driver.steps;
    r.properties.remove("optimization");
    r.properties.remove("thresholds");
    if (driver.present)
        r.properties["optimization"] = driver.converged ? "converged" : "not_converged";
    if (!driver.converged)
        r.warnings.removeAll(tr("最適化の収束メッセージを検出しました。"));
    if (r.orbitals.isEmpty())
        r.orbitals = orbitals; // Energies only: shapes need MOLDEN (or a CUBE) for the basis.
    if (!thermochemistry.isEmpty())
        r.properties["thermochemistry"] = thermochemistry;
    if (!driver.thresholds.isEmpty())
        r.properties["thresholds"] = driver.thresholds;
    if (!scan.isEmpty()) {
        QJsonArray points;
        for (const auto &point : std::as_const(scan))
            if (point.contains("energy"))
                points << point;
        r.properties["scan"] = points;
    }
    if (!nebPath.isEmpty() || !nebEnergies.isEmpty()) {
        QJsonObject neb;
        QJsonArray points;
        if (!nebPath.isEmpty()) {
            // The energies of the path's step at full precision where the log printed them.
            const bool precise =
                nebEnergiesIteration == nebPathIteration && nebEnergies.size() == nebPath.size();
            double distance = 0;
            for (int k = 0; k < nebPath.size(); ++k) {
                if (k > 0) {
                    double squares = 0;
                    for (int i = 0; i < nebPath[k].atoms.size(); ++i)
                        squares +=
                            (nebPath[k].atoms[i].position - nebPath[k - 1].atoms[i].position).squaredNorm();
                    distance += std::sqrt(squares);
                }
                addFrame(r, nebPath[k], tr("NEB 像 %1").arg(k + 1));
                points << QJsonObject{{"bead", k + 1},
                                      {"energy", precise ? nebEnergies[k] : nebPathEnergies[k]},
                                      {"frame", r.frames.size() - 1},
                                      {"distance", distance}};
            }
            neb["iteration"] = nebPathIteration;
        } else {
            for (int k = 0; k < nebEnergies.size(); ++k)
                points << QJsonObject{{"bead", k + 1}, {"energy", nebEnergies[k]}};
            neb["iteration"] = nebEnergiesIteration;
            warn(r, tr("NEBの経路の構造はログにありません（入力のnebブロックにprint_shiftを指定すると出力"
                       "されます）。"));
        }
        neb["points"] = points;
        if (!nebSteps.isEmpty())
            neb["steps"] = nebSteps;
        if (!nebThresholds.isEmpty())
            neb["thresholds"] = nebThresholds;
        if (nebConverged >= 0)
            neb["converged"] = nebConverged == 1;
        if (nebConverged == 0)
            warn(r, tr("NEBの経路は最大ステップ数までに収束していません。最高点の構造は遷移状態の目安として"
                       "遷移状態探索に使ってください。"));
        r.properties["neb"] = neb;
    }
    if (!ircPoints.isEmpty()) {
        irc["points"] = ircPoints;
        // Complete when each direction traced (both, or the one chosen) printed its convergence.
        QJsonArray directions;
        for (const auto *direction : {"forward", "backward"})
            if (std::any_of(ircPoints.begin(), ircPoints.end(),
                            [&](const QJsonValue &p) { return p.toObject()["direction"] == direction; }))
                directions << direction;
        irc["directions"] = directions;
        irc["complete"] = ircConverged >= directions.size();
        r.properties["irc"] = irc;
    }
    if (!shielding.isEmpty())
        r.properties["shielding"] = shielding;
    if (!spinspin.isEmpty())
        r.properties["spinspin"] = spinspin;
    if (r.properties.contains("raman")) {
        auto rows = r.properties["raman"].toArray();
        QSet<int> matched;
        const auto rigid = resultRigidModes(r);
        for (int i = 0; i < rows.size(); ++i) {
            auto row = rows[i].toObject();
            int best = -1;
            double distance = .02; // Printed Raman/mode frequencies are rounded to 0.01 cm⁻¹.
            for (int j = 0; j < r.modes.size(); ++j) {
                const auto delta = std::abs(r.modes[j].frequency - row["frequency"].toDouble());
                if (!rigid.value(j) && !matched.contains(j) && delta < distance) {
                    best = j;
                    distance = delta;
                }
            }
            if (best >= 0) {
                row["mode"] = best;
                matched.insert(best);
                rows[i] = row;
            }
        }
        r.properties["raman"] = rows;
    }
    if (!solvation.isEmpty())
        r.properties["solvation"] = solvation;
    r.outcome = fatal ? ResultOutcome::Error : ended ? ResultOutcome::Normal : ResultOutcome::Incomplete;
    if (!abort.isEmpty())
        r.status = tr(ErrorStatus).arg(abort);
    else if (fatal)
        r.status = tr(MessageStatus);
    else if (ended)
        r.status = tr(NormalStatus);
    if (const auto final = energySummary.energy(); !final.isEmpty())
        r.properties["final_energy"] = final;
    if (const auto bsse = energySummary.bsse(); !bsse.isEmpty()) {
        r.properties["bsse"] = bsse;
        if (bsse.contains("nwchem_error"))
            r.warnings << tr("NWChemが出力したBSSE補正量 %1 Eh は一部の単量体の和なので、"
                             "すべての単量体のエネルギーから求めた %2 Eh を使います。")
                              .arg(bsse["nwchem_error"].toDouble(), 0, 'f', 9)
                              .arg(bsse["error"].toDouble(), 0, 'f', 9);
    }
    r.warnings.removeDuplicates();
}
int NwchemLog::driverSteps(const QString &input) {
    bool driver = false;
    for (const auto &line : input.split('\n')) {
        if (!line.isEmpty() && !line[0].isSpace())
            driver = line == "driver";
        else if (driver && line.trimmed().startsWith("maxiter "))
            return line.trimmed().section(' ', 1, 1).toInt();
    }
    return 0;
}
QStringList NwchemLog::tasks(const QString &input) {
    static const QHash<QString, const char *> names{{"energy", QT_TR_NOOP("一点エネルギー")},
                                                    {"gradient", QT_TR_NOOP("勾配")},
                                                    {"optimize", QT_TR_NOOP("構造最適化")},
                                                    {"saddle", QT_TR_NOOP("遷移状態探索")},
                                                    {"frequencies", QT_TR_NOOP("振動数")},
                                                    {"freq", QT_TR_NOOP("振動数")},
                                                    {"mepgs", "IRC"},
                                                    {"neb", "NEB"},
                                                    {"property", QT_TR_NOOP("物性")},
                                                    {"dplot", QT_TR_NOOP("CUBE出力")}};
    // An operation without a name stays as written in the input.
    const auto name = [](const QString &operation) {
        const char *text = names.value(operation);
        return text ? tr(text) : operation;
    };
    QStringList result;
    for (const auto &line : input.split('\n')) {
        if (!line.startsWith("task "))
            continue;
        const auto words = line.split(' ', Qt::SkipEmptyParts);
        // "task dft optimize", "task esp", "task dplot", "task tddft energy".
        const auto theory = words.value(1).toLower(), operation = words.value(2).toLower();
        if (theory == "tddft" && operation == "optimize")
            result << tr("励起状態の構造最適化（TDDFT）");
        else if (theory == "tddft")
            result << tr("励起状態（TDDFT）");
        else if (theory == "esp")
            result << tr("ESP電荷");
        else if (theory == "dplot")
            result << name("dplot");
        else
            result << name(operation.isEmpty() ? "energy" : operation);
    }
    return result;
}

} // namespace mf
