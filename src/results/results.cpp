#include "results/results.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include "model/units.h"
#include "results/molden_orbitals.h"
#include <Eigen/Geometry>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace mf {
namespace {
constexpr qint64 maxBytes = 128 * 1024 * 1024;
constexpr int maxAtoms = 2000, maxFrames = 2000, maxPoints = 2000000, maxTriangles = 500000;
QStringList words(const QString &line) {
    static const QRegularExpression whitespace("\\s+");
    return line.split(whitespace, Qt::SkipEmptyParts);
}
double number(QString text) {
    text.replace(QChar('D'), QChar('E'), Qt::CaseInsensitive);
    bool ok;
    const double n = text.toDouble(&ok);
    if (!ok || !std::isfinite(n))
        fail(tr("結果ファイルの数値が不正です: %1").arg(text.left(80)));
    return n;
}
int integer(const QString &text, int lo, int hi) {
    const double n = number(text);
    if (n < lo || n > hi || n != std::floor(n))
        fail(tr("結果ファイルの整数・個数が範囲外です: %1").arg(text.left(80)));
    return int(n);
}
Vec point(const QStringList &w, int start, double scale = 1) {
    if (w.size() < start + 3)
        fail(tr("結果ファイルの座標が不足しています。"));
    const Vec p(number(w[start]), number(w[start + 1]), number(w[start + 2]));
    if (p.cwiseAbs().maxCoeff() > 1e6)
        fail(tr("結果ファイルの座標が範囲外です。"));
    return p * scale;
}
QString symbol(int z) {
    for (const auto &e : elements())
        if (e.number == z)
            return e.symbol;
    fail(tr("このビューアで扱えない原子番号です: %1").arg(z));
}
Atom atom(QString tag, const Vec &p) {
    Atom a;
    if (tag.startsWith('@')) {
        a.ghost = true;
        tag.remove(0, 1);
    }
    if (tag.startsWith("bq", Qt::CaseInsensitive)) {
        a.ghost = true;
        tag = tag.mid(2);
    }
    const auto match = QRegularExpression("^([A-Za-z]{1,2})[0-9_]*$").match(tag);
    if (!match.hasMatch())
        fail(tr("未対応の原子タグです: %1").arg(tag.left(80)));
    tag = match.captured(1).toLower();
    tag[0] = tag[0].toUpper();
    a.element = element(tag).symbol;
    a.position = p;
    return a;
}
void addFrame(ResultData &r, Document d, const QString &label) {
    if (d.atoms.isEmpty() || d.atoms.size() > maxAtoms || r.frames.size() >= maxFrames)
        fail(tr("構造数または原子数が対応範囲外です（2000構造・2000原子まで）。"));
    qsizetype total = d.atoms.size();
    for (const auto &frame : r.frames)
        total += frame.molecule.atoms.size();
    if (total > 200000)
        fail(tr("構造列が大きすぎます（合計20万原子まで）。"));
    d.inferBonds();
    r.frames.append({std::move(d), label});
}
QByteArray boundedRead(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    if (f.size() > maxBytes)
        fail(tr("結果ファイルは128 MiBまで読み込めます。ログを分割するか格子を小さくしてください。"));
    auto data = f.read(maxBytes + 1);
    if (data.size() > maxBytes)
        fail(tr("読込中に結果ファイルが128 MiBを超えました。"));
    return data;
}
void xyz(ResultData &r, const QStringList &lines) {
    int i = 0;
    while (i < lines.size()) {
        if (lines[i].trimmed().isEmpty()) {
            ++i;
            continue;
        }
        const int count = integer(lines[i++].trimmed(), 1, maxAtoms);
        if (i + count >= lines.size())
            fail(tr("XYZの構造が途中で切れています。"));
        const QString title = lines[i++];
        Document d;
        for (int n = 0; n < count; ++n) {
            const auto row = words(lines[i++]);
            if (row.size() != 4)
                fail(tr("結果XYZは元素・X・Y・Zの4列で指定してください。"));
            d.atoms << atom(row[0], point(row, 1));
        }
        addFrame(r, d, title);
    }
}
} // namespace
bool NwchemEnergySummary::relevant(QByteArrayView line) {
    return line.contains("energy") || line.contains("NWChem Input Module") ||
           line.contains("Frequency Analysis") || line.contains("Total times") ||
           line.contains("number of monomers") || line.contains("BSSE error");
}
void NwchemEnergySummary::add(const QString &line) {
    static const QRegularExpression total(
        R"(^\s*Total\s+(SCF|DFT|MP2|CCSD(?:\(T\))?)\s+energy\s*[:=]?\s*([-+0-9.eEdD]+)\s*$)",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression corrected(R"(^\s*Corrected energy\s*=\s*([-+0-9.eEdD]+)\s*$)");
    static const QRegularExpression supermolecular(R"(^\s*Supermolecular energy\s*=\s*([-+0-9.eEdD]+)\s*$)");
    static const QRegularExpression bsseError(R"(^\s*BSSE error\s*=\s*([-+0-9.eEdD]+)\s*$)");
    static const QRegularExpression monomers(R"(^\s*number of monomers\s*=\s*(\d+)\s*$)");
    const auto read = [](QString text, double &value) {
        bool ok;
        value = text.replace(QChar('D'), QChar('E'), Qt::CaseInsensitive).toDouble(&ok);
        return ok && std::isfinite(value);
    };
    auto &task = tasks.last();
    double value;
    if (line.contains("NWChem Input Module")) {
        if (task.energy || task.frequency)
            tasks.append(Task{});
    } else if (line.contains("NWChem Nuclear Hessian and Frequency Analysis"))
        task.frequency = true;
    else if (line.contains("Total times  cpu:"))
        ended = true;
    else if (const auto n = monomers.match(line); n.hasMatch())
        task.monomers = std::min(n.captured(1).toInt(), 1000);
    else if (const auto e = bsseError.match(line); e.hasMatch() && read(e.captured(1), value))
        task.nwchemError = value;
    else if (!line.contains("energy"))
        return;
    else if (const auto m = total.match(line); m.hasMatch() && read(m.captured(2), value)) {
        if (!task.energy) {
            task.first = value;
            task.firstMethod = m.captured(1).toUpper();
        }
        task.last = value;
        task.lastMethod = m.captured(1).toUpper();
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
        if (!it->energy)
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
double logNumber(QString text) {
    bool ok;
    const double n = text.replace(QChar('D'), QChar('E'), Qt::CaseInsensitive).toDouble(&ok);
    return ok && std::isfinite(n) ? n : std::numeric_limits<double>::quiet_NaN();
}
ResultStep resultStep(const QRegularExpressionMatch &m) {
    const auto columns = words(m.captured(2));
    return {m.captured(1).toInt(), logNumber(columns[0]), logNumber(columns[1]), logNumber(columns[2]),
            logNumber(columns[3]), logNumber(columns[4]), logNumber(columns[5]), logNumber(columns[6])};
}
void nwchem(ResultData &r, const QStringList &lines) {
    r.status = tr(IncompleteStatus);
    r.outcome = ResultOutcome::Incomplete;
    bool fatal = false, ended = false, recognized = false;
    static const QRegularExpression energy(
        R"(\bTotal\s+(SCF|DFT|MP2|CCSD(?:\(T\))?)\s+energy\s*[:=]?\s*([-+0-9.eEdD]+))",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression error(
        "failed to converge|convergence failure|not converged|MPI_ABORT|An error occurred|"
        "error termination|task.*failed|failed.*task|Calculation failed|There is an error in|"
        "This error has not yet been assigned|This type of error is most commonly associated|"
        "Received an Error in Communication",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression code(R"(\s+-?\d+\s*$)");
    static const QRegularExpression root(
        R"(^\s*Root\s+(\d+)\s+(?:(singlet|triplet)\s+)?(\S+)\s+(-?[\d.]+)\s+a\.u\.\s+(-?[\d.]+)\s+eV)");
    const auto &step = stepLine();
    const auto &threshold = thresholdLine();
    static const QRegularExpression orbitalHeader(R"(Final (Alpha |Beta )?Molecular Orbital Analysis)");
    static const QRegularExpression vector(
        R"(^\s*Vector\s+(\d+)\s+Occ=\s*(\S+)\s+E=\s*(\S+)(?:\s+Symmetry=\s*(\S+))?)");
    // Lenient numbers for the summaries: one odd line must not reject the whole log.
    const auto value = [](QString text) {
        bool ok;
        const double n = text.replace(QChar('D'), QChar('E'), Qt::CaseInsensitive).toDouble(&ok);
        return ok && std::isfinite(n) ? n : std::numeric_limits<double>::quiet_NaN();
    };
    // Markers written by MolForma's scan input ("title" of each point) and IRC point lines.
    static const QRegularExpression scanMark(R"(MolForma scan (\d+)/(\d+) ([a-z]+(?: \d+)+) = (\S+))");
    static const QRegularExpression ircPoint(R"(^&\s+(\d+)((?:\s+\S+){6})\s*$)");
    static const QRegularExpression atomRow(R"(^\s*(\d+)\s+([A-Za-z]{1,3}\w*)\s+(\S+)\s+(\S+)\s*$)");
    QString abort, orbitalSpin;
    QVector<ResultOrbital> logOrbitals;
    QJsonArray thermochemistry;
    QJsonObject thresholds;
    QMap<int, QJsonObject> scan;
    int scanPoint = 0;
    QJsonArray ircPoints;
    QString ircDirection;
    QJsonObject irc, solvation;
    QJsonArray shielding;
    int shieldingAtom = 0;
    QString shieldingElement;
    NwchemEnergySummary summary;
    for (int i = 0; i < lines.size(); ++i) {
        const auto &line = lines[i];
        summary.add(line);
        recognized |= line.contains("NWChem");
        // errquit prints its message, framed by dashes, just above "current input line :".
        if (line.trimmed().startsWith("current input line") && abort.isEmpty()) {
            for (int j = i - 1; j >= std::max(0, i - 4); --j) {
                const auto text = lines[j].trimmed();
                if (text.isEmpty() || text.startsWith("----"))
                    continue;
                abort = QString(text).remove(code).left(200);
                fatal = true;
                r.warnings << tr("行 %1: NWChemエラー: %2").arg(j + 1).arg(abort);
                break;
            }
        }
        if (error.match(line).hasMatch()) {
            fatal = true;
            if (r.warnings.size() < 30)
                r.warnings << tr("行 %1: %2").arg(i + 1).arg(line.trimmed().left(250));
        }
        ended |= line.contains("Total times  cpu:");
        if (auto m = scanMark.match(line); m.hasMatch() && m.captured(1).toInt() > scanPoint) {
            scanPoint = m.captured(1).toInt();
            if (scan.size() < 1000)
                scan[scanPoint] = QJsonObject{{"point", scanPoint},
                                              {"points", m.captured(2).toInt()},
                                              {"coordinate", m.captured(3)},
                                              {"value", value(m.captured(4))}};
        }
        if (line.contains("Forward IRC optimization") || line.contains("Backward IRC optimization")) {
            if (ircDirection.isEmpty()) {
                // The transition state: its energy and structure were computed before the path.
                if (!r.energies.isEmpty())
                    irc["ts_energy"] = r.energies.last().hartree;
                irc["ts_frame"] = r.frames.size() - 1;
            }
            ircDirection = line.contains("Forward") ? "forward" : "backward";
        }
        if (auto m = ircPoint.match(line);
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
        if (line.contains("BSSE error =")) {
            auto bsse = r.properties["bsse"].toObject();
            bsse["error"] = field("BSSE error");
            r.properties["bsse"] = bsse;
        }
        if (line.contains("Supermolecular energy =") || line.contains("Corrected energy =")) {
            auto bsse = r.properties["bsse"].toObject();
            bsse[line.contains("Corrected") ? "corrected" : "supermolecular"] = field("energy");
            r.properties["bsse"] = bsse;
        }
        if (line.contains("gas phase energy =") || line.contains("sol phase energy ="))
            solvation[line.contains("gas phase") ? "gas" : "solution"] = field("energy");
        if (line.contains("fixed-concentration free energy of solvation ="))
            solvation["smd_dg"] = field("solvation");
        if (line.contains("total free energy in solvent including G(SMD-CDS) ="))
            solvation["smd_total"] = field("G(SMD-CDS)");
        // Mulliken charges of the PROPERTY module (nuclear charge minus gross population).
        if (line.contains("Total      gross population on atoms")) {
            QJsonArray charges;
            for (int j = i + 1; j < lines.size() && j < i + 2 + maxAtoms; ++j) {
                if (lines[j].trimmed().isEmpty() && charges.isEmpty())
                    continue;
                const auto m = atomRow.match(lines[j]);
                if (!m.hasMatch() || m.captured(1).toInt() != charges.size() + 1)
                    break;
                charges << value(m.captured(3)) - value(m.captured(4));
            }
            if (!charges.isEmpty())
                r.properties["mulliken"] = charges;
        }
        // ESP (and RESP with "restrain") charges of the ESP module.
        if (line.contains("Atom") && line.contains("Coordinates") && line.contains("Charge")) {
            QJsonArray esp, resp;
            for (int j = i + 1; j < lines.size() && j < i + 8 + maxAtoms; ++j) {
                const auto row = words(lines[j]);
                if (row.size() < 6 || !QRegularExpression("^\\d+$").match(row[0]).hasMatch()) {
                    if (!esp.isEmpty())
                        break;
                    continue;
                }
                if (row[0].toInt() != esp.size() + 1)
                    break;
                esp << value(row[5]);
                if (row.size() >= 7)
                    resp << value(row[6]);
            }
            if (!esp.isEmpty()) {
                r.properties["esp"] = esp;
                if (resp.size() == esp.size())
                    r.properties["resp"] = resp;
                else
                    r.properties.remove("resp");
            }
        }
        // GIAO shielding tensors: "Atom:  n  X" followed by "isotropic =" and "anisotropy =".
        if (const auto t = line.trimmed(); t.startsWith("Atom:")) {
            const auto w = words(t);
            shieldingAtom = w.value(1).toInt();
            shieldingElement = w.value(2);
        }
        if (shieldingAtom > 0 && line.contains("isotropic =")) {
            if (shieldingAtom == 1)
                shielding = {}; // A later NMR calculation replaces the earlier one.
            shielding << QJsonObject{{"atom", shieldingAtom},
                                     {"element", shieldingElement},
                                     {"isotropic", value(words(line.section('=', 1)).value(0))}};
        }
        if (shieldingAtom > 0 && line.contains("anisotropy =") && !shielding.isEmpty()) {
            auto last = shielding.last().toObject();
            last["anisotropy"] = value(words(line.section('=', 1)).value(0));
            shielding[shielding.size() - 1] = last;
            shieldingAtom = 0;
        }
        if (line.contains("Linear Response polarizability / au")) {
            QJsonArray tensor;
            QJsonObject polarizability;
            for (int j = i + 1; j < lines.size() && j < i + 16; ++j) {
                const auto row = words(lines[j]);
                if (row.size() == 4 && QStringList{"X", "Y", "Z"}.contains(row[0]))
                    for (int k = 1; k < 4; ++k)
                        tensor << value(row[k]);
                if (lines[j].contains("Isotropic   ="))
                    polarizability["isotropic"] = value(words(lines[j].section('=', 1)).value(0));
                if (lines[j].contains("Anisotropic ="))
                    polarizability["anisotropy"] = value(words(lines[j].section('=', 1)).value(0));
                if (lines[j].contains("Frequency  ="))
                    polarizability["frequency"] = value(words(lines[j].section('=', 1)).value(0));
            }
            if (tensor.size() == 9 && polarizability.contains("isotropic")) {
                polarizability["tensor"] = tensor;
                r.properties["polarizability"] = polarizability;
            }
        }
        if (auto m = root.match(line); m.hasMatch() && r.excitations.size() < 2000) {
            ResultExcitation x{m.captured(1).toInt(), m.captured(2), m.captured(3), value(m.captured(5))};
            if (x.root == 1 && std::any_of(r.excitations.begin(), r.excitations.end(),
                                           [&](const auto &e) { return e.root == 1 && e.spin == x.spin; }))
                r.excitations.clear(); // A later TDDFT task replaces the earlier one.
            for (int j = i + 1; j < lines.size() && j < i + 40 && !root.match(lines[j]).hasMatch(); ++j) {
                if (lines[j].contains("<S2> ="))
                    x.s2 = value(words(lines[j]).value(2));
                if (lines[j].contains("Dipole Oscillator Strength"))
                    x.oscillator = value(words(lines[j]).last());
            }
            if (std::isfinite(x.ev))
                r.excitations << x;
        }
        if (auto m = step.match(line); m.hasMatch()) {
            const auto s = resultStep(m);
            // The converged step is printed twice; a new step 0 starts another optimization.
            if (s.step == 0 || (!r.steps.isEmpty() && s.step <= r.steps.last().step))
                while (!r.steps.isEmpty() && r.steps.last().step >= s.step)
                    r.steps.removeLast();
            if (std::isfinite(s.energy) && r.steps.size() < 20000)
                r.steps << s;
        }
        if (auto m = threshold.match(line); m.hasMatch())
            thresholds[m.captured(1)] = value(m.captured(2));
        if (line.contains("Optimization converged"))
            r.properties["optimization"] = "converged";
        if (auto m = orbitalHeader.match(line); m.hasMatch()) {
            orbitalSpin = m.captured(1).trimmed() == "Beta" ? "Beta" : "Alpha";
            if (orbitalSpin == "Alpha")
                logOrbitals.clear(); // Keep the last analysis (after optimization steps etc.).
        }
        if (auto m = vector.match(line); m.hasMatch() && !orbitalSpin.isEmpty() && logOrbitals.size() < 20000)
            logOrbitals << ResultOrbital{m.captured(4), orbitalSpin, value(m.captured(3)),
                                         value(m.captured(2)), m.captured(1).toInt()};
        if (line.contains("Multipole analysis of the density")) {
            QJsonObject dipole;
            for (int j = i + 1; j < lines.size() && j < i + 12; ++j) {
                const auto row = words(lines[j]);
                if (row.size() >= 5 && row[0] == "1" &&
                    QStringList{"100", "010", "001"}.contains(row.mid(1, 3).join(""))) {
                    const auto axis = row[1] == "1" ? "x" : row[2] == "1" ? "y" : "z";
                    dipole[axis] = value(row[4]);
                }
            }
            if (dipole.size() == 3) {
                const double au =
                    std::hypot(dipole["x"].toDouble(), dipole["y"].toDouble(), dipole["z"].toDouble());
                dipole["debye"] = au * 2.541746473;
                r.properties["dipole"] = dipole;
            }
        }
        if (line.trimmed().startsWith("Temperature") && line.contains('=') && line.trimmed().endsWith('K')) {
            QJsonObject t{{"temperature", value(line.section('=', 1).trimmed().chopped(1))}};
            for (int j = i + 1; j < lines.size() && j < i + 16; ++j) {
                const auto text = lines[j];
                const auto field = [&](const char *label, const char *key) {
                    if (text.contains(label))
                        t[key] = value(words(text.section('=', 1)).value(0));
                };
                field("Zero-Point correction to Energy", "zpe_kcal");     // kcal/mol
                field("Thermal correction to Energy", "energy_kcal");     // kcal/mol
                field("Thermal correction to Enthalpy", "enthalpy_kcal"); // kcal/mol
                field("Total Entropy", "entropy_cal");                    // cal/(mol K)
                field("Cv (constant volume heat capacity)", "cv_cal");    // cal/(mol K)
                if (text.contains("symmetry #"))
                    t["symmetry_number"] = value(text.section('=', -1).remove(')').trimmed());
            }
            if (t.contains("enthalpy_kcal") && t.contains("entropy_cal") && thermochemistry.size() < 20)
                thermochemistry << t;
        }
        if (line.contains("Optimization converged"))
            r.warnings << tr("最適化の収束メッセージを検出しました。");
        auto match = energy.match(line);
        if (match.hasMatch()) {
            if (r.energies.size() >= 20000)
                fail(tr("エネルギーの記録数が2万件を超えています。"));
            r.energies << ResultEnergy{match.captured(1).toUpper(), number(match.captured(2)), i + 1};
            if (scanPoint > 0 && scan.contains(scanPoint))
                scan[scanPoint]["energy"] = r.energies.last().hartree;
        }
        if (line.contains("Output coordinates in ")) {
            const double scale = line.contains("angstroms") ? 1 : line.contains("a.u.") ? BohrToAngstrom : 0;
            if (!scale) {
                r.warnings << tr("未対応の座標単位を読み飛ばしました。");
                continue;
            }
            Document d;
            int j = i + 1;
            while (j < lines.size() && j < i + 10 && !lines[j].contains("----"))
                ++j;
            ++j;
            while (j < lines.size()) {
                const auto row = words(lines[j]);
                if (row.size() != 6 || !QRegularExpression("^\\d+$").match(row[0]).hasMatch())
                    break;
                if (integer(row[0], 1, maxAtoms) != d.atoms.size() + 1)
                    break;
                try {
                    d.atoms << atom(row[1], point(row, 3, scale));
                } catch (const std::exception &e) {
                    r.warnings << QString::fromUtf8(e.what());
                    d.atoms.clear();
                    break;
                }
                ++j;
            }
            // Only accept a terminated table, never a partial last row at EOF. A complete table of
            // another size is another geometry, such as a monomer of a counterpoise correction.
            const bool complete = !d.atoms.isEmpty() && j < lines.size() && lines[j].trimmed().isEmpty();
            if (complete &&
                (r.frames.isEmpty() || r.frames.first().molecule.atoms.size() == d.atoms.size())) {
                addFrame(r, d,
                         scanPoint > 0 ? tr("スキャン %1・ログ 行 %2").arg(scanPoint).arg(i + 1)
                                       : tr("ログ 行 %1").arg(i + 1));
                if (scanPoint > 0 && scan.contains(scanPoint))
                    scan[scanPoint]["frame"] = r.frames.size() - 1;
            } else if (!complete)
                r.warnings << tr("行 %1の不完全な座標表を読み飛ばしました。").arg(i + 1);
        }
        if (line.contains("(Projected Frequencies expressed")) {
            r.modes.clear();
            if (!r.frames.isEmpty())
                r.vibrationGeometry = r.frames.last().molecule;
        }
        if (line.trimmed().startsWith("P.Frequency") && !r.vibrationGeometry.atoms.isEmpty()) {
            const auto frequencies = words(line).mid(1);
            QVector<ResultMode> modes;
            for (const auto &f : frequencies)
                modes.append({number(f), std::numeric_limits<double>::quiet_NaN(),
                              QVector<Vec>(r.vibrationGeometry.atoms.size(), Vec::Zero())});
            int j = i + 1, rowCount = 0;
            while (j < lines.size() && lines[j].trimmed().isEmpty())
                ++j;
            for (; j < lines.size() && rowCount < 3 * r.vibrationGeometry.atoms.size(); ++j) {
                auto row = words(lines[j]);
                if (row.size() != frequencies.size() + 1 || row[0] != QString::number(rowCount + 1))
                    break;
                for (int k = 0; k < modes.size(); ++k)
                    modes[k].displacement[rowCount / 3][rowCount % 3] = number(row[k + 1]);
                ++rowCount;
            }
            if (rowCount == 3 * r.vibrationGeometry.atoms.size())
                r.modes += modes;
            else
                r.warnings << tr("未完の振動モード表を読み飛ばしました。");
        }
        if (line.contains("Projected Infra Red Intensities")) {
            for (int j = i + 1; j < lines.size(); ++j) {
                auto row = words(lines[j]);
                if (row.size() == 7 && row[2] == "||" &&
                    QRegularExpression("^\\d+$").match(row[0]).hasMatch()) {
                    const int n = integer(row[0], 1, 6000) - 1;
                    if (n < r.modes.size())
                        r.modes[n].intensity = number(row[5]);
                } else if (j > i + 3)
                    break;
            }
        }
    }
    if (!recognized)
        fail(tr("NWChemの出力ログを認識できません。"));
    if (r.orbitals.isEmpty())
        r.orbitals = logOrbitals; // Energies only: shapes need MOLDEN (or a CUBE) for the basis.
    if (!thermochemistry.isEmpty())
        r.properties["thermochemistry"] = thermochemistry;
    if (!thresholds.isEmpty())
        r.properties["thresholds"] = thresholds;
    if (!scan.isEmpty()) {
        QJsonArray points;
        for (const auto &point : scan)
            if (point.contains("energy"))
                points << point;
        r.properties["scan"] = points;
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
        irc["complete"] = std::count_if(lines.begin(), lines.end(), [](const auto &l) {
                              return l.contains("IRC Optimization converged");
                          }) >= directions.size();
        r.properties["irc"] = irc;
    }
    if (!shielding.isEmpty())
        r.properties["shielding"] = shielding;
    if (!solvation.isEmpty())
        r.properties["solvation"] = solvation;
    if (!r.steps.isEmpty() && !r.properties.contains("optimization"))
        r.properties["optimization"] = "not_converged";
    r.outcome = fatal ? ResultOutcome::Error : ended ? ResultOutcome::Normal : ResultOutcome::Incomplete;
    if (!abort.isEmpty())
        r.status = tr(ErrorStatus).arg(abort);
    else if (fatal)
        r.status = tr(MessageStatus);
    else if (ended)
        r.status = tr(NormalStatus);
    if (const auto energy = summary.energy(); !energy.isEmpty())
        r.properties["final_energy"] = energy;
    if (const auto bsse = summary.bsse(); !bsse.isEmpty()) {
        r.properties["bsse"] = bsse;
        if (bsse.contains("nwchem_error"))
            r.warnings << tr("NWChemが出力したBSSE補正量 %1 Eh は一部の単量体の和なので、"
                             "すべての単量体のエネルギーから求めた %2 Eh を使います。")
                              .arg(bsse["nwchem_error"].toDouble(), 0, 'f', 9)
                              .arg(bsse["error"].toDouble(), 0, 'f', 9);
    }
    r.warnings.removeDuplicates();
}
void mesh(ResultData &r, const QVector<double> &values, const Vec &origin, const Mat &axes);
void molden(ResultData &r, const QStringList &lines, int requestedOrbital) {
    QMap<QString, QStringList> blocks;
    QMap<QString, QString> headers;
    QString current;
    const QRegularExpression section("^\\s*\\[([^]]+)\\](.*)$");
    for (const auto &line : lines) {
        auto match = section.match(line);
        if (match.hasMatch()) {
            current = match.captured(1).trimmed().toUpper();
            if (blocks.contains(current))
                fail(tr("MOLDENのセクションが重複しています: %1").arg(current));
            blocks[current] = {};
            headers[current] = match.captured(2).trimmed().toUpper();
        } else if (!current.isEmpty() && !line.trimmed().isEmpty())
            blocks[current] << line;
    }
    if (!blocks.contains("MOLDEN FORMAT"))
        fail(tr("MOLDENヘッダーがありません。"));
    if (blocks.contains("ATOMS")) {
        const auto units = headers["ATOMS"];
        if (units != "ANGS" && units != "AU" && units != "(ANGS)" && units != "(AU)")
            fail(tr("MOLDEN [Atoms] の単位はAngsまたはAUを指定してください。"));
        const double scale = units.contains("ANGS") ? 1 : BohrToAngstrom;
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
                active = line.contains("[GEOMETRIES]", Qt::CaseInsensitive);
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
    for (const auto &line : blocks["MO"]) {
        const auto pair = line.split('=');
        if (pair.size() != 2) {
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
        const QString key = pair[0].trimmed().toUpper(), value = pair[1].trimmed();
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
    if (!r.orbitals.isEmpty()) {
        r.orbitalBasis = blocks.contains("GTO") && r.orbitalFrame >= 0;
        try {
            if (!r.orbitalBasis)
                fail(tr("軌道の形状表示には[Atoms]・[GTO]・[MO]が必要です。"));
            const auto &atoms = r.frames[r.orbitalFrame].molecule;
            const auto wavefunction = moldenWavefunction(blocks, atoms, coefficients, r.normalizationRequest);
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
            const auto values = moldenOrbitalGrid(wavefunction, selected, box.origin, step, r.grid);
            r.minimum = *std::min_element(values.cbegin(), values.cend());
            r.maximum = *std::max_element(values.cbegin(), values.cend());
            mesh(r, values, box.origin, step.asDiagonal());
            r.selectedOrbital = selected;
            r.orbitalNormalization = wavefunction.normalization;
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
}
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
void cube(ResultData &r, QString text) {
    QTextStream stream(&text, QIODevice::ReadOnly);
    stream.readLine();
    stream.readLine();
    auto row = words(stream.readLine());
    if (row.size() < 4 || row.size() > 5)
        fail(tr("CUBEヘッダーが不正です。"));
    const int count = integer(row[0], -maxAtoms, maxAtoms);
    if (count == 0)
        fail(tr("原子を含まないCUBEは未対応です。"));
    int channels = row.size() == 5 ? integer(row[4], 1, 256) : 1;
    if (count < 0 && channels != 1)
        fail(tr("CUBEの軌道数指定が不正です。"));
    const Vec origin = point(row, 1, BohrToAngstrom);
    Mat axes;
    qint64 size = 1;
    for (int i = 0; i < 3; ++i) {
        row = words(stream.readLine());
        if (row.size() != 4)
            fail(tr("CUBEの格子軸が不正です。"));
        // NWChem Gaussian CUBE stores coordinates in Bohr. Reject ambiguous signed counts.
        r.grid[i] = integer(row[0], 2, maxPoints);
        size *= r.grid[i];
        if (size > maxPoints)
            fail(tr("CUBE格子は200万点まで読み込めます。"));
        axes.col(i) = point(row, 1, BohrToAngstrom);
    }
    if (std::abs(axes.determinant()) < 1e-15)
        fail(tr("CUBE格子の軸が独立ではありません。"));
    Document d;
    for (int i = 0; i < std::abs(count); ++i) {
        row = words(stream.readLine());
        if (row.size() != 5)
            fail(tr("CUBE原子表が不正です。"));
        d.atoms << atom(symbol(integer(row[0], 1, elements().last().number)), point(row, 2, BohrToAngstrom));
        number(row[1]); // Nuclear charge may differ from Z when an ECP is used.
    }
    addFrame(r, d, tr("CUBE原子座標"));
    auto token = [&] {
        QString value;
        stream >> value;
        if (value.isEmpty())
            fail(tr("CUBEデータが途中で切れています。"));
        return value;
    };
    if (count < 0) {
        channels = integer(token(), 1, 256);
        for (int i = 0; i < channels; ++i)
            r.datasets << integer(token(), 1, 1000000);
        if (QSet<int>(r.datasets.begin(), r.datasets.end()).size() != channels)
            fail(tr("CUBEデータ番号が重複しています。"));
    } else
        for (int i = 0; i < channels; ++i)
            r.datasets << i + 1;
    if (size * channels > 4000000)
        fail(tr("CUBE全データは400万値まで読み込めます。"));
    if (r.dataset < 0 || r.dataset >= channels)
        fail(tr("CUBEのデータ番号が範囲外です。"));
    QVector<double> values;
    values.reserve(size);
    for (qint64 i = 0; i < size; ++i)
        for (int channel = 0; channel < channels; ++channel) {
            double value = number(token());
            if (channel == r.dataset)
                values << value;
        }
    if (!stream.readAll().trimmed().isEmpty())
        fail(tr("CUBE格子数とデータ数が一致しません。"));
    auto bounds = std::minmax_element(values.begin(), values.end());
    r.minimum = *bounds.first;
    r.maximum = *bounds.second;
    mesh(r, values, origin, axes);
    if (r.surface.isEmpty())
        r.warnings << tr("指定した等値の面がありません。等値とデータ範囲を確認してください。");
    r.warnings << tr(
        "CUBEの値はファイルの単位を使用します。赤が正、青が負です。密度と軌道の区別はタイトル・生成"
        "条件を確認してください。");
}
} // namespace
void NwchemProgress::add(const QString &line) {
    static const QRegularExpression total(
        R"(^\s*Total\s+(?:SCF|DFT|MP2|CCSD(?:\(T\))?)\s+energy\s*[:=]?\s*(\S+)\s*$)",
        QRegularExpression::CaseInsensitiveOption);
    if (line.contains("NWChem Input Module")) {
        // NWChem reads the input again before each task; repeated banners start no new task.
        if (started == 0 || busy)
            ++started;
        busy = false;
    } else if (line.contains("Total times  cpu:"))
        ended = true;
    else if (const auto m = stepLine().match(line); m.hasMatch()) {
        const auto s = resultStep(m);
        // The converged step is printed twice; a new step 0 starts another optimization.
        if (s.step == 0 || stepTask != started - 1)
            steps.clear();
        while (!steps.isEmpty() && steps.last().step >= s.step)
            steps.removeLast();
        if (std::isfinite(s.energy) && steps.size() < 20000)
            steps << s;
        stepTask = started - 1;
        busy = true;
    } else if (const auto t = thresholdLine().match(line); t.hasMatch()) {
        thresholds[t.captured(1)] = logNumber(t.captured(2));
        busy = true;
    } else if (line.contains("Optimization converged")) {
        convergedTask = started - 1;
        busy = true;
    } else if (const auto e = total.match(line); e.hasMatch()) {
        if (const double value = logNumber(e.captured(1)); std::isfinite(value))
            energy = value;
        busy = true;
    } else if (line.contains("Frequency Analysis"))
        busy = true;
}
int NwchemProgress::driverSteps(const QString &input) {
    bool driver = false;
    for (const auto &line : input.split('\n')) {
        if (!line.isEmpty() && !line[0].isSpace())
            driver = line == "driver";
        else if (driver && line.trimmed().startsWith("maxiter "))
            return line.trimmed().section(' ', 1, 1).toInt();
    }
    return 0;
}
QStringList NwchemProgress::tasks(const QString &input) {
    static const QHash<QString, const char *> names{{"energy", QT_TR_NOOP("一点エネルギー")},
                                                    {"gradient", QT_TR_NOOP("勾配")},
                                                    {"optimize", QT_TR_NOOP("構造最適化")},
                                                    {"saddle", QT_TR_NOOP("遷移状態探索")},
                                                    {"frequencies", QT_TR_NOOP("振動数")},
                                                    {"freq", QT_TR_NOOP("振動数")},
                                                    {"mepgs", "IRC"},
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
        if (theory == "tddft")
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
ResultData readResult(const QString &path, double iso, int dataset, int orbital, const QString &normalization,
                      int orbitalDivisions) {
    if (!std::isfinite(iso) || iso <= 0 || iso > 1e6)
        fail(tr("等値は0より大きい有限の値で指定してください。"));
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
    auto lines = text.split('\n');
    for (auto &line : lines)
        if (line.endsWith('\r'))
            line.chop(1);
    const auto suffix = QFileInfo(path).suffix().toLower();
    r.status = tr("ファイルを読み込みました（計算の成否は判定しません）");
    if (suffix == "cube" || suffix == "cub") {
        r.format = "CUBE";
        cube(r, text);
    } else if (text.trimmed().startsWith("[Molden Format]", Qt::CaseInsensitive)) {
        r.format = "MOLDEN";
        molden(r, lines, orbital);
    } else if (suffix == "xyz") {
        r.format = "XYZ";
        xyz(r, lines);
    } else {
        r.format = "NWChem";
        nwchem(r, lines);
    }
    if (r.format != "NWChem" && r.frames.isEmpty())
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
void saveResultData(const QString &path, const ResultData &r) {
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_4);
    s << QString("MolFormaResult6") << r.source << r.format << r.status << r.log << r.warnings
      << qint32(r.outcome);
    s << r.orbitalBasis << qint32(r.selectedOrbital) << qint32(r.orbitalFrame) << qint32(r.orbitalDivisions)
      << r.orbitalError << r.orbitalNormalization << r.normalizationRequest;
    auto doc = [&](const Document &d) { s << QJsonDocument(d.toJson()).toJson(QJsonDocument::Compact); };
    auto vec = [&](const Vec &v) { s << v.x() << v.y() << v.z(); };
    s << qint32(r.frames.size());
    for (const auto &f : r.frames) {
        doc(f.molecule);
        s << f.label;
    }
    s << qint32(r.energies.size());
    for (const auto &e : r.energies)
        s << e.method << e.hartree << qint32(e.line);
    s << qint32(r.orbitals.size());
    for (const auto &o : r.orbitals)
        s << o.symmetry << o.spin << o.energy << o.occupation << qint32(o.number);
    doc(r.vibrationGeometry);
    s << qint32(r.modes.size());
    for (const auto &m : r.modes) {
        s << m.frequency << m.intensity << qint32(m.displacement.size());
        for (const auto &v : m.displacement)
            vec(v);
    }
    s << qint32(r.surface.size());
    for (const auto &t : r.surface) {
        vec(t.a);
        vec(t.b);
        vec(t.c);
        s << t.positive;
        for (const auto &normal : {t.na, t.nb, t.nc})
            s << float(normal.x()) << float(normal.y()) << float(normal.z());
    }
    s << r.datasets << qint32(r.dataset) << r.isovalue << r.minimum << r.maximum;
    for (int n : r.grid)
        s << qint32(n);
    s << qint32(r.excitations.size());
    for (const auto &x : r.excitations)
        s << qint32(x.root) << x.spin << x.symmetry << x.ev << x.oscillator << x.s2;
    s << qint32(r.steps.size());
    for (const auto &t : r.steps)
        s << qint32(t.step) << t.energy << t.deltaE << t.gmax << t.grms << t.xrms << t.xmax << t.seconds;
    s << QJsonDocument(r.properties).toJson(QJsonDocument::Compact);
    if (s.status() != QDataStream::Ok)
        fail(tr("結果の転送データを保存できません。"));
    writeFile(path, bytes);
}
ResultData loadResultData(const QString &path) {
    auto bytes = boundedRead(path);
    QDataStream s(bytes);
    s.setVersion(QDataStream::Qt_6_4);
    QString magic;
    s >> magic;
    if (magic != "MolFormaResult6")
        fail(tr("結果の転送形式が不正です。"));
    ResultData r;
    s >> r.source >> r.format >> r.status >> r.log >> r.warnings;
    qint32 outcome = 0;
    s >> outcome;
    if (outcome < int(ResultOutcome::NotApplicable) || outcome > int(ResultOutcome::Error))
        fail(tr("結果の転送形式が不正です。"));
    r.outcome = ResultOutcome(outcome);
    qint32 orbital, orbitalFrame, divisions;
    s >> r.orbitalBasis >> orbital >> orbitalFrame >> divisions >> r.orbitalError >> r.orbitalNormalization >>
        r.normalizationRequest;
    r.selectedOrbital = orbital;
    r.orbitalFrame = orbitalFrame;
    r.orbitalDivisions = divisions;
    auto count = [&](int max) {
        qint32 n;
        s >> n;
        if (s.status() != QDataStream::Ok || n < 0 || n > max)
            fail(tr("結果の転送件数が不正です。"));
        return n;
    };
    auto doc = [&] {
        QByteArray b;
        s >> b;
        return Document::fromJson(parseJson(b));
    };
    auto vec = [&] {
        double x, y, z;
        s >> x >> y >> z;
        return Vec(x, y, z);
    };
    for (int n = count(maxFrames); n--;) {
        ResultFrame f;
        f.molecule = doc();
        s >> f.label;
        r.frames << f;
    }
    for (int n = count(20000); n--;) {
        ResultEnergy e;
        qint32 line;
        s >> e.method >> e.hartree >> line;
        e.line = line;
        r.energies << e;
    }
    for (int n = count(20000); n--;) {
        ResultOrbital o;
        qint32 number;
        s >> o.symmetry >> o.spin >> o.energy >> o.occupation >> number;
        o.number = number;
        r.orbitals << o;
    }
    r.vibrationGeometry = doc();
    for (int n = count(6000); n--;) {
        ResultMode m;
        s >> m.frequency >> m.intensity;
        for (int k = count(maxAtoms); k--;)
            m.displacement << vec();
        r.modes << m;
    }
    for (int n = count(maxTriangles); n--;) {
        SurfaceTriangle t;
        t.a = vec();
        t.b = vec();
        t.c = vec();
        s >> t.positive;
        for (auto *normal : {&t.na, &t.nb, &t.nc}) {
            float x, y, z;
            s >> x >> y >> z;
            *normal = Vec(x, y, z);
            if (!normal->allFinite())
                normal->setZero();
        }
        r.surface << t;
    }
    qint32 dataset;
    s >> r.datasets >> dataset >> r.isovalue >> r.minimum >> r.maximum;
    r.dataset = dataset;
    for (int &n : r.grid) {
        qint32 value;
        s >> value;
        n = value;
    }
    for (int n = count(2000); n--;) {
        ResultExcitation x;
        qint32 root;
        s >> root >> x.spin >> x.symmetry >> x.ev >> x.oscillator >> x.s2;
        x.root = root;
        r.excitations << x;
    }
    for (int n = count(20000); n--;) {
        ResultStep t;
        qint32 step;
        s >> step >> t.energy >> t.deltaE >> t.gmax >> t.grms >> t.xrms >> t.xmax >> t.seconds;
        t.step = step;
        r.steps << t;
    }
    QByteArray properties;
    s >> properties;
    r.properties = QJsonDocument::fromJson(properties).object();
    if (s.status() != QDataStream::Ok || !s.atEnd())
        fail(tr("結果の転送データが不完全です。"));
    return r;
}
int resultWorker(const QString &request, const QString &response) {
    try {
        const auto q = parseJson(boundedRead(request));
        auto path = q["path"].toString();
        const auto animation = q["animation_files"].toArray();
        if (!animation.isEmpty()) {
            // The loader owns this scratch directory, so cancellation removes the joined file too.
            // Bound each read before allocating; all source I/O runs in this cancellable worker.
            constexpr qint64 limit = 64 * 1024 * 1024;
            QByteArray joined;
            for (const auto &source : animation) {
                QFile file(source.toString());
                if (!file.open(QIODevice::ReadOnly))
                    fail(file.errorString());
                const auto remaining = limit - joined.size();
                if (file.size() > remaining)
                    fail(tr("振動アニメーションのファイルが大きすぎます。"));
                joined += file.read(remaining + 1);
                if (file.error() != QFileDevice::NoError)
                    fail(file.errorString());
                if (!joined.endsWith('\n'))
                    joined += '\n';
                if (joined.size() > limit)
                    fail(tr("振動アニメーションのファイルが大きすぎます。"));
            }
            path = QDir(QFileInfo(response).absolutePath()).filePath("animation.xyz");
            writeFile(path, joined);
        }
        auto data =
            readResult(path, q["isovalue"].toDouble(.03), q["dataset"].toInt(), q["orbital"].toInt(-1),
                       q["normalization"].toString("auto"), q["orbital_divisions"].toInt(40));
        data.source = QFileInfo(q["path"].toString()).absoluteFilePath();
        saveResultData(response, data);
        return 0;
    } catch (const std::exception &e) {
        try {
            writeFile(response + ".error", QByteArray(e.what()).left(4096));
        } catch (...) {
        }
        return 1;
    }
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
QStringList resultFiles(const QString &directory) {
    auto files = QDir(directory).entryList(
        {"*.out", "*.log", "*.xyz", "*.cube", "*.cub", "*.molden", "*.mld", "*.molde"},
        QDir::Files | QDir::Readable, QDir::Name);
    // Generated files can live in permanent_dir; do not recursively scan it or trust the saved cwd.
    QFile record(QDir(directory).filePath("run.json"));
    if (record.open(QIODevice::ReadOnly) && record.size() <= 256 * 1024) {
        const auto m = QJsonDocument::fromJson(record.read(256 * 1024)).object();
        if (m["format"] == "molforma-nwchem-run" && m["version"].toDouble() == 1) {
            QSet<QString> known;
            for (const auto &file : files)
                known.insert(QFileInfo(QDir(directory).filePath(file)).canonicalFilePath());
            const auto outputs = m["visualization_files"].toArray();
            for (int i = 0; i < std::min(2, int(outputs.size())); ++i) {
                const auto name = outputs[i].toString();
                const auto clean = QDir::cleanPath(name);
                if (clean == ".." || clean.startsWith("../"))
                    continue;
                const QFileInfo info(QDir(directory).filePath(name));
                if (!QStringList{"cube", "cub", "molden"}.contains(info.suffix().toLower()) ||
                    !info.isFile() || !info.isReadable() || known.contains(info.canonicalFilePath()))
                    continue;
                known.insert(info.canonicalFilePath());
                files << name;
            }
        }
    }
    return files;
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
