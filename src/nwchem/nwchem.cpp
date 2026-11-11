// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include "model/memo.h"
#include "model/topology.h"
#include "nwchem/input_builder.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_geometry.h"
#include "nwchem/nwchem_import.h"
#include "nwchem/nwchem_output.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>

namespace mf {
NwchemCalculation::NwchemCalculation(const QJsonObject &settings) {
    static const QHash<QString, NwchemMethod> methods{{"hf", NwchemMethod::Hf},
                                                      {"dft", NwchemMethod::Dft},
                                                      {"mp2", NwchemMethod::Mp2},
                                                      {"ccsd", NwchemMethod::Ccsd},
                                                      {"ccsd(t)", NwchemMethod::CcsdT}};
    static const QHash<QString, NwchemTask> tasks{{"energy", NwchemTask::Energy},
                                                  {"gradient", NwchemTask::Gradient},
                                                  {"optimize", NwchemTask::Optimize},
                                                  {"frequencies", NwchemTask::Frequencies},
                                                  {"optimize_frequencies", NwchemTask::OptimizeFrequencies},
                                                  {"saddle", NwchemTask::Saddle},
                                                  {"saddle_frequencies", NwchemTask::SaddleFrequencies},
                                                  {"irc", NwchemTask::Irc},
                                                  {"scan", NwchemTask::Scan},
                                                  {"neb", NwchemTask::Neb}};
    method = methods.value(settings["method"].toString(), NwchemMethod::Unknown);
    task = tasks.value(settings["task"].toString(), NwchemTask::Unknown);
    relaxedScan = settings["scan_relaxed"].toBool();
}
namespace {
// The offered NWChem library bases: element coverage from the "X_<name>" entries of the NWChem
// 7.3.1 library files, and the ECP library their heavy elements need.
struct LibraryBasis {
    const char *name, *label;
    QVector<QPair<int, int>> elements;
    const char *ecp;
    bool relativistic = false; // All-electron, contracted for scalar-relativistic Hamiltonians.
};
const QVector<LibraryBasis> &libraryBases() {
    static const QVector<LibraryBasis> list{
        {"sto-3g", "STO-3G", {{1, 53}}, ""},
        {"3-21g", "3-21G", {{1, 55}}, ""},
        {"6-31g", "6-31G", {{1, 30}}, ""},
        {"6-31g*", "6-31G*", {{1, 36}}, ""},
        {"6-31g**", "6-31G**", {{1, 30}}, ""},
        {"6-31+g*", "6-31+G*", {{1, 20}}, ""},
        {"6-31++g**", "6-31++G**", {{1, 20}}, ""},
        {"6-311g**", "6-311G**", {{1, 20}, {31, 36}, {53, 53}}, ""},
        {"6-311+g**", "6-311+G**", {{1, 20}}, ""},
        {"6-311++g**", "6-311++G**", {{1, 20}}, ""},
        {"cc-pvdz", "cc-pVDZ", {{1, 18}, {20, 36}}, ""},
        {"cc-pvtz", "cc-pVTZ", {{1, 18}, {20, 36}}, ""},
        {"cc-pvqz", "cc-pVQZ", {{1, 18}, {20, 36}}, ""},
        {"aug-cc-pvdz", "aug-cc-pVDZ", {{1, 18}, {21, 36}}, ""},
        {"aug-cc-pvtz", "aug-cc-pVTZ", {{1, 18}, {21, 36}}, ""},
        {"def2-svp", "def2-SVP", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"def2-svpd", "def2-SVPD", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"def2-tzvp", "def2-TZVP", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"def2-tzvpp", "def2-TZVPP", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"def2-tzvpd", "def2-TZVPD", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"def2-qzvp", "def2-QZVP", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"lanl2dz ecp",
         QT_TR_NOOP("LANL2DZ（Na以降ECP）"),
         {{1, 1}, {3, 57}, {72, 83}, {92, 94}},
         "lanl2dz ecp"},
        {"x2c-svpall", QT_TR_NOOP("x2c-SVPall（相対論・全電子）"), {{1, 86}}, "", true},
        {"x2c-tzvpall", QT_TR_NOOP("x2c-TZVPall（相対論・全電子）"), {{1, 86}}, "", true},
        {"cc-pvdz-dk", QT_TR_NOOP("cc-pVDZ-DK（相対論）"), {{1, 18}, {21, 36}}, "", true},
        {"cc-pvtz-dk", QT_TR_NOOP("cc-pVTZ-DK（相対論）"), {{1, 18}, {21, 36}, {39, 46}}, "", true},
        {"sarc-zora", QT_TR_NOOP("SARC-ZORA（La–Rn・Ac–Lr、相対論）"), {{57, 86}, {89, 103}}, "", true},
        {"sarc-dkh", QT_TR_NOOP("SARC-DKH（La–Rn・Ac–Lr、相対論）"), {{57, 86}, {89, 103}}, "", true}};
    return list;
}
const LibraryBasis *libraryBasis(const QString &name) {
    for (const auto &b : libraryBases())
        if (name == b.name)
            return &b;
    return nullptr;
}
// Core electrons replaced by the basis' ECP ("nelec" in NWChem's def2-ecp and lanl2dz_ecp).
int ecpCore(const QString &basis, int z) {
    const auto *b = libraryBasis(basis);
    if (!b || !*b->ecp)
        return 0;
    static const QMap<QString, QVector<std::array<int, 3>>> cores{
        {"def2-ecp", {{37, 54, 28}, {55, 57, 46}, {72, 86, 60}}},
        {"lanl2dz ecp",
         {{11, 29, 10},
          {30, 30, 18},
          {31, 47, 28},
          {48, 48, 36},
          {49, 57, 46},
          {72, 79, 60},
          {80, 81, 68},
          {82, 83, 78},
          {92, 94, 78}}}};
    for (const auto &[lo, hi, core] : cores.value(b->ecp))
        if (z >= lo && z <= hi)
            return core;
    return 0;
}
// The pasted basis and ECP of settings ("basis_custom", "ecp_custom"), read once for each text.
struct Pasted {
    NwchemPastedBasis basis, ecp;
};
Pasted pasted(const QJsonObject &settings) {
    thread_local QHash<QString, NwchemPastedBasis> read[2];
    const auto get = [](const QString &text, bool ecp) {
        if (text.trimmed().isEmpty())
            return NwchemPastedBasis{};
        auto &cache = read[ecp];
        if (const auto it = cache.constFind(text); it != cache.cend())
            return *it;
        if (cache.size() > 16)
            cache.clear();
        return *cache.insert(text, nwchemPastedBasis(text, ecp));
    };
    return {get(settings["basis_custom"].toString(), false), get(settings["ecp_custom"].toString(), true)};
}
// The library basis of an element: its per-element override, otherwise the common basis.
QString elementBasis(const QJsonObject &settings, const QString &symbol) {
    const auto overrides = settings["basis_overrides"].toObject();
    return overrides.contains(symbol) ? overrides[symbol].toString() : settings["basis"].toString();
}
// Core electrons an ECP replaces: a pasted ECP's "nelec"; none for an element with a pasted basis and
// no pasted ECP (the library's ECP belongs to the library's basis); otherwise the library basis' ECP.
int coreElectrons(const QJsonObject &settings, const Pasted &p, const QString &symbol, int z) {
    if (const auto it = p.ecp.cores.constFind(symbol); it != p.ecp.cores.cend())
        return *it;
    return p.basis.lines.contains(symbol) ? 0 : ecpCore(elementBasis(settings, symbol), z);
}
int atomicCoreOrbitals(int z) {
    // NWChem FREEZE ATOMIC table. Ghosts and ECP centers are excluded by the caller.
    return z <= 2 ? 0 : z <= 10 ? 1 : z <= 18 ? 5 : z <= 36 ? 9 : z <= 54 ? 18 : 27;
}
// Coverage of the curated NWChem basis library entries (including ghost centers).
bool basisCovers(const QString &basis, int z) {
    const auto *b = libraryBasis(basis);
    return b && std::any_of(b->elements.begin(), b->elements.end(),
                            [z](const auto &range) { return z >= range.first && z <= range.second; });
}
// DFT functionals offered in the GUI. Dispersion corrections are listed only where NWChem 7.3.1
// prints Grimme's published parameters: it has none for M06*-D3(BJ) and SCAN/r2SCAN-D3, and its
// B3PW91-D3(BJ) and PW6B95-D3 values disagree with Grimme's. Meta-GGA and range-separated
// functionals have no analytic Hessian in NWChem (it falls back to finite differences), and its
// TDDFT stops for meta-GGA kernels ("d2 not coded").
struct Functional {
    const char *value, *label, *xc, *extra;
    bool d3, d3bj, numericalHessian, metaGga = false;
};
const QVector<Functional> &functionals() {
    static const QVector<Functional> list{
        {"b3lyp", "B3LYP", "b3lyp", "", true, true, false},
        {"pbe", "PBE", "xpbe96 cpbe96", "", true, true, false},
        {"pbe0", "PBE0", "pbe0", "", true, true, false},
        {"blyp", "BLYP", "becke88 lyp", "", true, true, false},
        {"bp86", "BP86", "becke88 perdew86", "", true, true, false},
        {"b3pw91", "B3PW91", "b3pw91", "", true, false, false},
        {"bhlyp", "BHLYP", "bhlyp", "", true, true, false},
        {"tpss", QT_TR_NOOP("TPSS（meta-GGA）"), "xtpss03 ctpss03", "", true, true, true, true},
        {"scan", QT_TR_NOOP("SCAN（meta-GGA）"), "scan", "", false, true, true, true},
        {"r2scan", QT_TR_NOOP("r²SCAN（meta-GGA）"), "r2scan", "", false, true, true, true},
        {"m06-l", QT_TR_NOOP("M06-L（meta-GGA）"), "m06-l", "", true, false, true, true},
        {"m06", "M06", "m06", "", true, false, true, true},
        {"m06-2x", "M06-2X", "m06-2x", "", true, false, true, true},
        {"pw6b95", "PW6B95", "pw6b95", "", false, true, true, true},
        {"cam-b3lyp", QT_TR_NOOP("CAM-B3LYP（長距離補正）"), "xcamb88 1.00 lyp 0.81 vwn_5 0.19 hfexch 1.00",
         "cam 0.33 cam_alpha 0.19 cam_beta 0.46", true, true, true},
        {"wb97x-d3", QT_TR_NOOP("ωB97X-D3（D3を含む長距離補正）"), "wb97x-d3", "", false, false, true},
        {"custom", QT_TR_NOOP("任意のxc指定"), "", "", false, false, true}};
    return list;
}
const Functional *functional(const QString &value) {
    for (const auto &f : functionals())
        if (value == f.value)
            return &f;
    return nullptr;
}
// Pople's 6-31G family is defined with Cartesian d shells (6d); the other library bases are
// conventionally used with spherical harmonics (5d/7f), as in Gaussian's defaults.
bool cartesianByConvention(const QString &basis) {
    return basis == "3-21g" || basis.startsWith("6-31g") || basis.startsWith("6-31+") ||
           basis.startsWith("6-31++");
}
// Earlier projects and presets stored a boolean "spherical"; read it as an explicit choice.
QJsonObject withBasisFunctions(QJsonObject settings) {
    if (!settings.contains("basis_functions") && settings["spherical"].isBool())
        settings["basis_functions"] = settings["spherical"].toBool() ? "spherical" : "cartesian";
    settings.remove("spherical");
    return settings;
}
// The shortest text that reads back as the same double (coordinates, charges, typed values).
QString shortest(double n) {
    return QString::number(n, 'g', QLocale::FloatingPointShortest);
}
QStringList sorted(const QSet<QString> &set) {
    auto list = set.values();
    std::sort(list.begin(), list.end());
    return list;
}
// A choice with its label in the source language, translated where it is offered.
struct ChoiceSource {
    const char *value, *label;
};
// Named solvents of NWChem's SMD database (8-character codes). Plain COSMO takes the dielectric
// constant of a named solvent from the same table.
const QVector<ChoiceSource> &solventNames() {
    static const QVector<ChoiceSource> list{{"dielectric", QT_TR_NOOP("誘電率を指定（COSMOのみ）")},
                                            {"h2o", QT_TR_NOOP("水")},
                                            {"methanol", QT_TR_NOOP("メタノール")},
                                            {"ethanol", QT_TR_NOOP("エタノール")},
                                            {"propanol", QT_TR_NOOP("1-プロパノール")},
                                            {"butanol", QT_TR_NOOP("1-ブタノール")},
                                            {"acetntrl", QT_TR_NOOP("アセトニトリル")},
                                            {"acetone", QT_TR_NOOP("アセトン")},
                                            {"dmso", QT_TR_NOOP("ジメチルスルホキシド（DMSO）")},
                                            {"dmf", QT_TR_NOOP("N,N-ジメチルホルムアミド（DMF）")},
                                            {"thf", QT_TR_NOOP("テトラヒドロフラン（THF）")},
                                            {"dioxane", QT_TR_NOOP("1,4-ジオキサン")},
                                            {"ether", QT_TR_NOOP("ジエチルエーテル")},
                                            {"etoac", QT_TR_NOOP("酢酸エチル")},
                                            {"dcm", QT_TR_NOOP("ジクロロメタン")},
                                            {"chcl3", QT_TR_NOOP("クロロホルム")},
                                            {"ch3no2", QT_TR_NOOP("ニトロメタン")},
                                            {"pyridine", QT_TR_NOOP("ピリジン")},
                                            {"benzene", QT_TR_NOOP("ベンゼン")},
                                            {"toluene", QT_TR_NOOP("トルエン")},
                                            {"hexane", QT_TR_NOOP("n-ヘキサン")},
                                            {"cychexan", QT_TR_NOOP("シクロヘキサン")}};
    return list;
}
// Weigend's universal Coulomb fitting set ("weigend coulomb fitting" in the NWChem library).
bool fittingCovers(int z) {
    return (z >= 1 && z <= 57) || (z >= 72 && z <= 86);
}
QString coordinateError(const Document &d, const Ids &ids, double value) {
    if (ids.size() < 2 || ids.size() > 4)
        return tr("2〜4原子を指定してください。");
    if (QSet<QString>(ids.begin(), ids.end()).size() != ids.size())
        return tr("同じ原子を重複して指定できません。");
    for (const auto &id : ids) {
        if (!d.contains(id))
            return tr("原子が見つかりません。指定し直してください。");
        if (d.atom(id).ghost)
            return tr("ゴースト中心は指定できません。");
    }
    if (!std::isfinite(value))
        return tr("値は有限の数値で指定してください。");
    if (ids.size() == 2 && (value < 0.1 || value > 50))
        return tr("距離は0.1〜50 Åで指定してください。");
    if (ids.size() == 3 && (value <= 0 || value >= 180))
        return tr("角度は0°より大きく180°未満で指定してください。");
    if (ids.size() == 4 && (value < -180 || value > 180))
        return tr("二面角は−180〜180°で指定してください。");
    return {};
}
// A constrained ZCOORD internal coordinate; distances are in the GEOMETRY units.
QString zcoordLine(const Document &d, const Ids &ids, double value, double scale, const QString &name) {
    QStringList numbers;
    for (const auto &id : ids)
        numbers << QString::number(d.index(id) + 1);
    const char *kind = ids.size() == 2 ? "bond" : ids.size() == 3 ? "angle" : "torsion";
    return QString("%1 %2 %3 %4 constant")
        .arg(kind, numbers.join(' '), shortest(ids.size() == 2 ? value / scale : value), name);
}
QStringList constraintLines(const Document &d, const QJsonObject &s, double scale) {
    QStringList lines;
    const auto constraints = s["constraints"].toArray();
    for (int i = 0; i < constraints.size(); ++i) {
        const auto c = constraints[i].toObject();
        const auto ids = idsFromJson(c["atoms"]);
        if (c["free"].toBool()) {
            // A coordinate added to those of AUTOZ, which the optimization moves (NWChem names it
            // "User00n").
            QStringList numbers;
            for (const auto &id : ids)
                numbers << QString::number(d.index(id) + 1);
            lines << QString("%1 %2").arg(ids.size() == 2   ? "bond"
                                          : ids.size() == 3 ? "angle"
                                                            : "torsion",
                                          numbers.join(' '));
        } else
            lines << zcoordLine(d, ids, c["value"].toDouble(), scale, QString("mfc%1").arg(i + 1));
    }
    return lines;
}
bool constrained(const QJsonObject &s) {
    const auto list = s["constraints"].toArray();
    return std::any_of(list.begin(), list.end(),
                       [](const QJsonValue &c) { return !c.toObject()["free"].toBool(); });
}
// The value of a scan coordinate (1 or 2) at a point.
double scanValue(const QJsonObject &s, int step, int coordinate = 1) {
    const QString prefix = coordinate == 1 ? "scan_" : "scan2_";
    const int points = std::max(2, s[prefix + "steps"].toInt());
    const double start = s[prefix + "start"].toDouble(), end = s[prefix + "end"].toDouble();
    // Rounded to 1e-9, so that inputs read 1.55 rather than 1.5499999999999998.
    return std::round((start + (end - start) * step / (points - 1)) * 1e9) / 1e9;
}
bool twoDimensionalScan(const QJsonObject &s) {
    return !s["scan2_atoms"].toArray().isEmpty();
}
// How far a coordinate is from a value (Å or degrees; dihedral angles across ±180°).
double coordinateDeviation(const Document &d, const Ids &ids, double value) {
    const double now = measure(d, ids);
    if (ids.size() < 4)
        return std::abs(now - value);
    const double turn = std::fmod(std::abs(now - value), 360.0);
    return std::min(turn, 360 - turn);
}
// Geometry of one scan point (with its point of the second coordinate): the later atoms' side moved to
// the scanned values, as when the dimension is set in the editor. The first coordinate is set again
// when setting the second moved it.
Document scanGeometry(const Document &d, const QJsonObject &s, int step, int step2 = 0) {
    Document point = d;
    const auto first = idsFromJson(s["scan_atoms"]);
    setDimension(point, first, scanValue(s, step));
    if (twoDimensionalScan(s)) {
        setDimension(point, idsFromJson(s["scan2_atoms"]), scanValue(s, step2, 2));
        if (coordinateDeviation(point, first, scanValue(s, step)) > 1e-4)
            setDimension(point, first, scanValue(s, step));
    }
    return point;
}
// Counterpoise monomers: the bonded molecules of the real atoms, in atom order (atom indices).
QVector<QVector<int>> bsseMonomers(const Document &d) {
    const auto molecule = Topology(d).molecules();
    QVector<int> monomer(d.atoms.size(), -1); // By molecule; molecules of ghosts only are none.
    QVector<QVector<int>> monomers;
    for (int i = 0; i < d.atoms.size(); ++i)
        if (!d.atoms[i].ghost && monomer[molecule[i]] < 0) {
            monomer[molecule[i]] = monomers.size();
            monomers.append(QVector<int>());
        }
    for (int i = 0; i < d.atoms.size(); ++i)
        if (const int m = monomer[molecule[i]]; m >= 0)
            monomers[m] << i;
    return monomers;
}
int monomerCharge(const Document &d, const QVector<int> &atoms) {
    int charge = 0;
    for (int i : atoms)
        charge += d.atoms[i].charge;
    return charge;
}
// The follow-up single point as a calculation of its own: the optimized structure at another
// method or basis. Solvent, relativity and RI-J stay, as they stay in NWChem's runtime database.
Document followupDocument(const Document &d) {
    Document next = d;
    const auto full = nwchemSettings(d);
    auto s = d.data["nwchem"].toObject();
    s["method"] = full["followup_method"];
    s["functional"] = full["followup_functional"];
    s["dispersion"] = full["followup_dispersion"];
    s["basis"] = full["followup_basis"];
    s["xc_custom"] = "";
    s["basis_overrides"] = QJsonObject();
    s["basis_custom"] = ""; // The follow-up basis is one library basis for all elements.
    s["ecp_custom"] = "";
    s["task"] = "energy";
    // Property requests stay: they run at this level, and MOLDEN output must clear them first.
    for (const auto *key : {"tddft", "followup", "bsse"})
        s[key] = false;
    s["constraints"] = QJsonArray();
    s["extra_input"] = "";
    s["extra_tasks"] = "";
    auto advanced = s["advanced"].toObject();
    advanced.remove("vectors_guess"); // Orbitals of the first basis do not fit the second one.
    advanced.remove("vectors_input");
    if (advanced.isEmpty())
        s.remove("advanced");
    else
        s["advanced"] = advanced;
    next.data["nwchem"] = s;
    return next;
}
bool anyProperty(const QJsonObject &s) {
    return s["prop_mulliken"].toBool() || s["prop_esp"].toBool() || s["prop_nmr"].toBool() ||
           s["prop_spinspin"].toBool() || s["prop_polarizability"].toBool() || s["prop_epr"].toBool() ||
           s["prop_esp_grid"].toBool() || s["prop_efield_grid"].toBool();
}
// Input that MolForma does not interpret. Shell tasks and Python blocks would run programs.
QString extraInputError(const QString &text) {
    if (text.size() > 20000)
        return tr("20000文字以内で指定してください。");
    if (text.contains(QRegularExpression("[\\x00-\\x08\\x0b-\\x1f\\x7f]")))
        return tr("制御文字を含めないでください。");
    // Statements as NWChem splits them (";" and "\\" continuations), not physical lines. NWChem
    // reads quoted words without their quotes: task "shell" is task shell.
    for (const auto &statement : nwchemStatements(text)) {
        const auto words =
            QString(statement).remove('"').simplified().toLower().split(' ', Qt::SkipEmptyParts);
        if (words.isEmpty())
            continue;
        if (words[0] == "python" ||
            (words[0] == "task" && words.size() > 1 && (words[1] == "shell" || words[1] == "python")))
            return tr("外部プログラムを実行するtask shell・task python・pythonブロックは使用できません。");
    }
    return {};
}
} // namespace
NwchemPastedBasis nwchemPastedBasis(const QString &text, bool ecp) {
    NwchemPastedBasis result;
    if (text.size() > 500000) {
        result.error = ecp ? tr("貼り付けたECPは500000文字以内にしてください。")
                           : tr("貼り付けた基底は500000文字以内にしてください。");
        return result;
    }
    static const QRegularExpression number(R"(^[+-]?(\d+\.?\d*|\.\d+)([eEdD][+-]?\d+)?$)");
    static const QStringList shells{"s", "p", "d", "f", "g", "h", "i", "k", "sp", "l"},
        ecpShells{"ul", "s", "p", "d", "f", "g", "h", "i"};
    QString current;     // The element of the shell being read.
    bool numbers = true; // The shell begun last has its lines of numbers.
    int headerLine = 0;
    const auto problem = [&](int line, const QString &why) {
        if (result.error.isEmpty())
            result.error =
                (ecp ? tr("貼り付けたECPの%1行目: %2") : tr("貼り付けた基底の%1行目: %2")).arg(line).arg(why);
    };
    const auto lines = text.split('\n');
    for (int i = 0; i < lines.size() && result.error.isEmpty(); ++i) {
        const auto raw = lines[i];
        if (raw.contains(QRegularExpression("[\\x00-\\x08\\x0b-\\x1f\\x7f]"))) {
            problem(i + 1, tr("制御文字を含めないでください。"));
            break;
        }
        const auto line = raw.section('#', 0, 0).simplified();
        if (line.isEmpty())
            continue;
        const auto words = line.split(' ');
        const auto first = words[0].toLower();
        // The block around the entries, as the Basis Set Exchange writes it.
        if (first == "end" || (!ecp && first == "basis") || (ecp && first == "ecp")) {
            if (!numbers)
                problem(headerLine, tr("殻の後に数値の行がありません。"));
            current.clear();
            numbers = true;
            continue;
        }
        if (std::all_of(words.begin(), words.end(),
                        [&](const QString &w) { return number.match(w).hasMatch(); })) {
            if (current.isEmpty())
                problem(i + 1, tr("数値の行の前に、元素と殻の行が必要です。"));
            else if (ecp ? words.size() != 3 : words.size() < 2)
                problem(i + 1, ecp ? tr("ECPの数値の行は3個の数値（rの次数・指数・係数）です。")
                                   : tr("基底の数値の行は指数と係数の2個以上の数値です。"));
            else {
                result.lines[current] << "    " + words.join(' ');
                numbers = true;
            }
            continue;
        }
        // "<element> <shell>": the element as its symbol (NWChem takes either case).
        const auto symbol = words[0].left(1).toUpper() + words[0].mid(1).toLower();
        bool known = true;
        try {
            element(symbol);
        } catch (const std::exception &) {
            known = false;
        }
        if (!known || words[0].size() > 2) {
            problem(i + 1, tr("「%1」は元素記号ではありません。").arg(words[0]));
            break;
        }
        if (!numbers)
            problem(headerLine, tr("殻の後に数値の行がありません。"));
        const auto kind = words.value(1).toLower();
        if (!ecp) {
            if (words.size() != 2 || !shells.contains(kind)) {
                problem(i + 1, tr("「元素 殻」（例: C S）の行が必要です。"));
                break;
            }
            result.lines[symbol] << QString("  %1 %2").arg(symbol, kind.toUpper());
            numbers = false;
        } else if (words.size() == 3 && kind == "nelec") {
            bool ok;
            const int core = words[2].toInt(&ok);
            if (!ok || core < 0 || core >= element(symbol).number) {
                problem(i + 1, tr("nelecは0以上で原子番号より小さい整数にしてください。"));
                break;
            }
            result.cores[symbol] = core;
            result.lines[symbol] << QString("  %1 nelec %2").arg(symbol).arg(core);
        } else if (words.size() == 2 && ecpShells.contains(kind)) {
            result.lines[symbol] << QString("  %1 %2").arg(symbol, kind == "ul" ? kind : kind.toUpper());
            numbers = false;
        } else {
            problem(i + 1, tr("「元素 nelec 数」「元素 ul」「元素 殻」のいずれかの行が必要です。"));
            break;
        }
        headerLine = i + 1;
        current = symbol;
        if (!result.elements.contains(symbol))
            result.elements << symbol;
    }
    if (!numbers)
        problem(headerLine, tr("殻の後に数値の行がありません。"));
    if (ecp && result.error.isEmpty())
        for (const auto &symbol : std::as_const(result.elements))
            if (!result.cores.contains(symbol)) {
                result.error = tr("貼り付けたECPの元素 %1 に「%1 nelec 数」の行がありません。").arg(symbol);
                break;
            }
    if (!result.error.isEmpty()) {
        result.lines.clear();
        result.cores.clear();
    }
    return result;
}
QVector<NwchemFragment> nwchemFragments(const Document &d) {
    QVector<NwchemFragment> result;
    const auto list = nwchemSettings(d)["fragment_guess"].toArray();
    if (list.isEmpty())
        return result;
    QHash<QString, int> index;
    for (int i = 0; i < d.atoms.size(); ++i)
        index[d.atoms[i].id] = i;
    for (const auto &value : list) {
        const auto o = value.toObject();
        NwchemFragment fragment;
        for (const auto &id : idsFromJson(o["atoms"]))
            if (const auto it = index.constFind(id); it != index.cend())
                fragment.atoms << *it;
        std::sort(fragment.atoms.begin(), fragment.atoms.end());
        fragment.charge = o["charge"].toInt();
        fragment.multiplicity = o["multiplicity"].toInt(1);
        result << fragment;
    }
    return result;
}
QVector<QPair<int, int>> nwchemSpinSpinPairs(const Document &d) {
    const bool all = nwchemSettings(d)["spinspin_pairs"] == "all";
    QVector<int> atoms;
    for (int i = 0; i < d.atoms.size(); ++i)
        if (!d.atoms[i].ghost &&
            (all || d.atoms[i].element == "H" || d.atoms[i].element == "F" || d.atoms[i].element == "P"))
            atoms << i;
    QVector<QPair<int, int>> pairs;
    if (atoms.size() > 1000) // More than any limit allows; their number is all that matters then.
        return QVector<QPair<int, int>>(atoms.size() * (atoms.size() - 1) / 2);
    for (int i = 0; i < atoms.size(); ++i)
        for (int j = i + 1; j < atoms.size(); ++j)
            pairs << qMakePair(atoms[i], atoms[j]);
    return pairs;
}
QString nwchemFunctionalXc(const QString &value) {
    const auto *f = functional(value);
    return f ? QString(f->xc) : QString();
}
bool nwchemAutoz(const Document &d) {
    const auto s = nwchemSettings(d);
    return NwchemCalculation(s).optimization() && s["constraints"].toArray().isEmpty() &&
           !nwchemAdvanced(d)["driver_cartesian"].toBool() &&
           std::none_of(d.atoms.begin(), d.atoms.end(), [](const Atom &a) { return a.fixed || a.ghost; });
}
QString nwchemCoordinateLabel(const Document &d, const Ids &ids) {
    QStringList numbers;
    for (const auto &id : ids)
        numbers << (d.contains(id) ? QString::number(d.index(id) + 1) : QString("?"));
    const auto atoms = numbers.join("–");
    return ids.size() == 2   ? tr("距離 %1").arg(atoms)
           : ids.size() == 3 ? tr("角度 %1").arg(atoms)
                             : tr("二面角 %1").arg(atoms);
}
QVector<QVector<int>> nwchemBsseMonomers(const Document &d) {
    return bsseMonomers(d);
}
QString nwchemScfThreshold(const QJsonObject &s) {
    const auto convergence = s["scf_convergence"].toString();
    return convergence == "default"  ? QString()
           : convergence == "normal" ? "1e-5"
           : convergence == "tight"  ? "1e-7"
                                     : "1e-9";
}
QString nwchemSectionUnused(const Document &d, const QString &section) {
    // As the scopes of the section's settings: a section takes part when one of them does.
    const auto s = nwchemSettings(d);
    const auto applies = [&](NwchemScope scope) { return nwchemScopeApplies(scope, s, &d); };
    if (section == "path")
        return applies(NwchemScope::Irc) || applies(NwchemScope::Scan) || applies(NwchemScope::Neb)
                   ? QString()
                   : tr("使わない");
    if (section == "constraints")
        return applies(NwchemScope::Driver) ? QString() : tr("最適化のみ");
    if (section == "optimization")
        return applies(NwchemScope::DriverOrIrc) || applies(NwchemScope::Frequency) ? QString()
                                                                                    : tr("使わない");
    if (section == "followup")
        return applies(NwchemScope::Optimization) ? QString() : tr("最適化のみ");
    if (section == "bsse")
        return bsseMonomers(d).size() >= 2 ? QString() : tr("単量体なし");
    if (section == "excited")
        return !NwchemCalculation(s).dft()            ? tr("DFTのみ")
               : !applies(NwchemScope::ExcitedToggle) ? tr("一点・最適化")
                                                      : QString();
    return {};
}
QJsonObject nwchemDefaults() {
    QJsonObject defaults;
    for (const auto &spec : nwchemSettingSpecs())
        if (!spec.structural)
            defaults[spec.key] = spec.initial;
    return defaults;
}
QJsonObject nwchemSettings(const Document &d) {
    auto settings = nwchemDefaults();
    const auto stored = withBasisFunctions(d.data["nwchem"].toObject());
    for (auto it = stored.begin(); it != stored.end(); ++it)
        settings[it.key()] = it.value();
    return settings;
}
NwchemRestart nwchemRestart(const QJsonObject &settings) {
    const auto stored = settings["restart"].toObject();
    NwchemRestart restart;
    restart.mode = stored["mode"].toString();
    restart.job = stored["job"].toString();
    restart.geometry = stored["geometry"].toString();
    for (const auto &line : stored["done"].toArray())
        restart.done << line.toString();
    return restart;
}
QString nwchemRestartDigest(const Document &d, bool masses) {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    const auto number = [](double value) { return QString::number(value, 'g', 17); };
    for (const auto &a : d.atoms)
        hash.addData(QStringList{a.element, a.ghost ? "g" : "", a.fixed ? "f" : "", number(a.position.x()),
                                 number(a.position.y()), number(a.position.z()),
                                 masses ? QString::number(a.isotope) : QString(),
                                 masses ? number(a.mass) : QString()}
                         .join(' ')
                         .toUtf8() +
                     '\n');
    hash.addData(
        QJsonDocument(d.data["nwchem"].toObject()["constraints"].toArray()).toJson(QJsonDocument::Compact));
    return QString::fromLatin1(hash.result().toHex());
}
NwchemNebEnd nwchemNebEnd(const Document &d) {
    NwchemNebEnd result;
    const auto stored = d.data["nwchem"].toObject()["neb_end"].toObject();
    if (stored.isEmpty()) {
        result.problem = tr("NEBの終点の構造を「2 計算条件」の「IRC・スキャン・NEB」で設定してください。");
        return result;
    }
    Document end = d;
    int missing = 0;
    for (auto &a : end.atoms) {
        const auto value = stored.value(a.id).toArray();
        const Vec position(value.at(0).toDouble(qQNaN()), value.at(1).toDouble(qQNaN()),
                           value.at(2).toDouble(qQNaN()));
        if (value.size() != 3 || !position.allFinite())
            ++missing;
        else
            a.position = position;
    }
    if (missing)
        result.problem = tr("NEBの終点に位置のない原子が %n 個あります（終点を設定した後に原子を加えた場合"
                            "など）。終点を設定し直してください。",
                            nullptr, missing);
    else
        result.document = end;
    return result;
}
QJsonObject nwchemNebEndSetting(const Document &d, const Document &end, bool fit) {
    if (end.atoms.size() != d.atoms.size())
        fail(tr("終点の構造の原子数（%1）が分子（%2）と異なります。")
                 .arg(end.atoms.size())
                 .arg(d.atoms.size()));
    for (int i = 0; i < d.atoms.size(); ++i) {
        const auto &a = d.atoms[i], &b = end.atoms[i];
        if (a.element != b.element || a.ghost != b.ghost)
            fail(tr("原子%1の元素（%2 / 終点 %3）が一致しません。原子の順番が同じ構造を選んでください。")
                     .arg(i + 1)
                     .arg(a.element, b.element));
        if (!b.position.allFinite())
            fail(tr("終点の構造の座標が有限ではありません。"));
    }
    Superposition placement;
    if (fit && d.atoms.size() >= 2)
        placement = superpose(end, d);
    QJsonObject positions;
    for (int i = 0; i < d.atoms.size(); ++i)
        positions[d.atoms[i].id] = vectorToJson(placement.rotation * end.atoms[i].position + placement.shift);
    return positions;
}
QVector<Document> nwchemNebInitialPath(const Document &start, const Document &end, int beads) {
    QVector<Document> path;
    beads = std::max(2, beads);
    for (int k = 0; k < beads; ++k) {
        Document bead = start;
        const double t = double(k) / (beads - 1);
        for (int i = 0; i < bead.atoms.size() && i < end.atoms.size(); ++i)
            bead.atoms[i].position = (1 - t) * start.atoms[i].position + t * end.atoms[i].position;
        path << bead;
    }
    return path;
}
QStringList nwchemTaskLines(const QString &input) {
    QStringList lines;
    for (const auto &line : input.split('\n'))
        if (line.startsWith("task "))
            lines << line.simplified();
    return lines;
}
namespace {
// The choices of a setting, with their labels in the source language.
QVector<ChoiceSource> choiceSources(const QString &key) {
    if (key == "method" || key == "followup_method")
        return {
            {"hf", "Hartree–Fock"}, {"dft", "DFT"}, {"mp2", "MP2"}, {"ccsd", "CCSD"}, {"ccsd(t)", "CCSD(T)"}};
    if (key == "task")
        return {{"energy", QT_TR_NOOP("一点エネルギー")},
                {"gradient", QT_TR_NOOP("勾配")},
                {"optimize", QT_TR_NOOP("構造最適化")},
                {"frequencies", QT_TR_NOOP("振動数")},
                {"optimize_frequencies", QT_TR_NOOP("最適化 → 振動数")},
                {"saddle", QT_TR_NOOP("遷移状態探索")},
                {"saddle_frequencies", QT_TR_NOOP("遷移状態探索 → 振動数")},
                {"irc", QT_TR_NOOP("IRC（遷移状態からの反応経路）")},
                {"scan", QT_TR_NOOP("座標スキャン")},
                {"neb", QT_TR_NOOP("NEB（2つの構造を結ぶ反応経路）")}};
    if (key == "irc_direction")
        return {{"both", QT_TR_NOOP("両方向")},
                {"forward", QT_TR_NOOP("前進のみ")},
                {"backward", QT_TR_NOOP("後退のみ")}};
    if (key == "reference")
        return {{"auto", QT_TR_NOOP("自動（多重度・手法から選択）")},
                {"rhf", QT_TR_NOOP("RHF / RKS（制限）")},
                {"uhf", QT_TR_NOOP("UHF / UKS（非制限）")},
                {"rohf", QT_TR_NOOP("ROHF（制限開殻HF）")}};
    if (key == "functional" || key == "followup_functional") {
        QVector<ChoiceSource> result;
        for (const auto &f : functionals())
            if (key == "functional" || QString(f.value) != "custom")
                result << ChoiceSource{f.value, f.label};
        return result;
    }
    if (key == "dispersion" || key == "followup_dispersion")
        return {{"none", QT_TR_NOOP("なし")}, {"d3", "Grimme D3"}, {"d3bj", "Grimme D3(BJ)"}};
    if (key == "basis" || key == "followup_basis") {
        QVector<ChoiceSource> result;
        for (const auto &b : libraryBases())
            result << ChoiceSource{b.name, b.label};
        return result;
    }
    if (key == "basis_functions")
        return {{"auto", QT_TR_NOOP("自動（6-31G系はCartesian、他は球面）")},
                {"spherical", QT_TR_NOOP("球面調和関数（5d・7f）")},
                {"cartesian", QT_TR_NOOP("Cartesian（6d・10f）")}};
    if (key == "scf_convergence")
        return {{"default", QT_TR_NOOP("NWChem既定")},
                {"normal", QT_TR_NOOP("標準")},
                {"tight", QT_TR_NOOP("厳密")},
                {"verytight", QT_TR_NOOP("さらに厳密")}};
    if (key == "opt_convergence")
        return {
            {"default", QT_TR_NOOP("標準")}, {"tight", QT_TR_NOOP("厳密")}, {"loose", QT_TR_NOOP("緩い")}};
    if (key == "grid")
        return {{"coarse", "coarse"},
                {"medium", "medium"},
                {"fine", "fine"},
                {"xfine", "xfine"},
                {"huge", "huge"}};
    if (key == "relativistic")
        return {{"none", QT_TR_NOOP("なし")},
                {"zora", QT_TR_NOOP("ZORA（DFTのみ）")},
                {"dkh", QT_TR_NOOP("Douglas–Kroll–Hess（DKH2）")},
                {"x2c", "X2C"}};
    if (key == "solvent")
        return {{"none", QT_TR_NOOP("気相")},
                {"cosmo", "COSMO"},
                {"smd", QT_TR_NOOP("SMD（DFT・溶媒名を指定）")}};
    if (key == "solvent_name")
        return solventNames();
    if (key == "geometry_units")
        return {{"angstrom", "Å"}, {"bohr", "Bohr"}};
    if (key == "spinspin_pairs")
        return {{"hfp", QT_TR_NOOP("H・F・Pどうしの組（¹H・¹⁹F NMRの分裂）")},
                {"all", QT_TR_NOOP("すべての原子の組")}};
    return {};
}
} // namespace
const QVector<NwchemSettingSpec> &nwchemSettingSpecs() {
    using Kind = NwchemSettingKind;
    using Scope = NwchemScope;
    // Fields of the Document rather than of its NWChem settings.
    const auto structural = [](NwchemSettingSpec spec) {
        spec.structural = true;
        spec.preset = false;
        return spec;
    };
    // Atom references, free input and the identity of a run belong to a project, not a reusable method.
    const auto project = [](NwchemSettingSpec spec) {
        spec.preset = false;
        return spec;
    };
    // Can still be switched off when the calculation does not use it.
    const auto disableable = [](NwchemSettingSpec spec) {
        spec.allowDisable = true;
        return spec;
    };
    static const QVector<NwchemSettingSpec> specs = [=] {
        QVector<NwchemSettingSpec> list{
            project({"start", Kind::String, "molforma"}),
            project({"title", Kind::String, "MolForma calculation"}),
            project({"memory_mb", Kind::Integer, 1024, 64, 1048576}),
            structural({"charge", Kind::Integer, 0, -1000, 1000}),
            structural({"multiplicity", Kind::Integer, 1, 1, 1000}),
            {"method", Kind::Choice, "hf"},
            {"task", Kind::Choice, "energy"},
            {"basis", Kind::Choice, "sto-3g"},
            {"reference", Kind::Choice, "auto"},
            {"functional", Kind::Choice, "b3lyp", 0, 0, 0, Scope::Dft},
            {"xc_custom", Kind::String, "", 0, 0, 0, Scope::CustomXc},
            {"dispersion", Kind::Choice, "none", 0, 0, 0, Scope::Dft},
            {"basis_functions", Kind::Choice, "auto"},
            {"basis_overrides", Kind::Object, QJsonObject()},
            {"basis_custom", Kind::String, ""}, // Pasted in NWChem's format; before the library.
            {"ecp_custom", Kind::String, ""},
            project({"geometry_units", Kind::Choice, "angstrom"}),
            {"scf_maxiter", Kind::Integer, 100, 1, 10000},
            {"scf_convergence", Kind::Choice, "tight"},
            {"grid", Kind::Choice, "fine", 0, 0, 0, Scope::Dft},
            disableable({"relativistic", Kind::Choice, "none", 0, 0, 0, Scope::Relativistic}),
            disableable({"ri_j", Kind::Flag, false, 0, 0, 0, Scope::Dft}),
            {"opt_maxiter", Kind::Integer, 100, 1, 10000, 0, Scope::DriverOrIrc},
            {"opt_convergence", Kind::Choice, "default", 0, 0, 0, Scope::Driver},
            {"numerical_frequencies", Kind::Flag, false, 0, 0, 0, Scope::Frequency},
            {"freeze_core", Kind::Flag, false, 0, 0, 0, Scope::Correlation},
            {"cc_maxiter", Kind::Integer, 50, 1, 10000, 0, Scope::Ccsd},
            {"solvent", Kind::Choice, "none"},
            {"solvent_name", Kind::Choice, "dielectric", 0, 0, 0, Scope::Solvent},
            {"dielectric", Kind::Real, 78.4, 1, 1000, 4, Scope::Dielectric},
            {"include_point_charges", Kind::Flag, false},
            // Atom IDs, charge and multiplicity of each fragment of an initial guess.
            project({"fragment_guess", Kind::Array, QJsonArray()}),
            disableable({"tddft", Kind::Flag, false, 0, 0, 0, Scope::ExcitedToggle}),
            {"tddft_roots", Kind::Integer, 10, 1, 100, 0, Scope::Excited},
            {"tddft_triplets", Kind::Flag, false, 0, 0, 0, Scope::Excited},
            {"tddft_tda", Kind::Flag, false, 0, 0, 0, Scope::Excited},
            {"tddft_cdspectrum", Kind::Flag, false, 0, 0, 0, Scope::Excited},
            {"tddft_root", Kind::Integer, 1, 1, 100, 0, Scope::ExcitedOptimization},
            project({"constraints", Kind::Array, QJsonArray(), 0, 0, 0, Scope::Driver}),
            project({"scan_atoms", Kind::Array, QJsonArray(), 0, 0, 0, Scope::Scan}),
            project({"scan_start", Kind::Real, 1.0, -180, 180, 4, Scope::Scan}),
            project({"scan_end", Kind::Real, 2.0, -180, 180, 4, Scope::Scan}),
            // Checked only for scans: a range of points that other tasks do not use.
            [](NwchemSettingSpec spec) {
                spec.validateUnused = false;
                return spec;
            }({"scan_steps", Kind::Integer, 6, 2, 50, 0, Scope::Scan}),
            {"scan_relaxed", Kind::Flag, true, 0, 0, 0, Scope::Scan},
            // The second coordinate of a two-dimensional scan (none: one-dimensional).
            project({"scan2_atoms", Kind::Array, QJsonArray(), 0, 0, 0, Scope::Scan}),
            project({"scan2_start", Kind::Real, 1.0, -180, 180, 4, Scope::Scan2}),
            project({"scan2_end", Kind::Real, 2.0, -180, 180, 4, Scope::Scan2}),
            [](NwchemSettingSpec spec) {
                spec.validateUnused = false;
                return spec;
            }({"scan2_steps", Kind::Integer, 6, 2, 50, 0, Scope::Scan2}),
            {"irc_points", Kind::Integer, 30, 1, 500, 0, Scope::Irc},
            {"irc_stride", Kind::Real, 0.1, .01, 1, 3, Scope::Irc},
            {"irc_direction", Kind::Choice, "both", 0, 0, 0, Scope::Irc},
            {"neb_beads", Kind::Integer, 10, 3, 100, 0, Scope::Neb},
            {"neb_spring", Kind::Real, 1.0, .001, 100, 3, Scope::Neb},
            {"neb_maxiter", Kind::Integer, 50, 1, 10000, 0, Scope::Neb},
            {"neb_stepsize", Kind::Real, .1, .001, 10, 3, Scope::Neb},
            // The end of the path: atom positions by atom ID, kept with the atoms of the project.
            [](NwchemSettingSpec spec) {
                spec.preset = false;
                spec.validateUnused = false;
                return spec;
            }({"neb_end", Kind::Object, QJsonObject(), 0, 0, 0, Scope::Neb}),
            {"bsse", Kind::Flag, false},
            disableable({"followup", Kind::Flag, false, 0, 0, 0, Scope::Optimization}),
            {"followup_method", Kind::Choice, "dft", 0, 0, 0, Scope::Followup},
            {"followup_functional", Kind::Choice, "b3lyp", 0, 0, 0, Scope::FollowupDft},
            {"followup_dispersion", Kind::Choice, "none", 0, 0, 0, Scope::FollowupDft},
            {"followup_basis", Kind::Choice, "def2-tzvp", 0, 0, 0, Scope::Followup},
            {"prop_mulliken", Kind::Flag, false},
            {"prop_esp", Kind::Flag, false},
            {"prop_nmr", Kind::Flag, false},
            {"prop_spinspin", Kind::Flag, false},
            {"spinspin_pairs", Kind::Choice, "hfp", 0, 0, 0, Scope::SpinSpin},
            {"prop_polarizability", Kind::Flag, false},
            {"prop_raman", Kind::Flag, false},
            {"prop_epr", Kind::Flag, false},
            {"prop_ord", Kind::Flag, false},
            {"prop_esp_grid", Kind::Flag, false},
            {"prop_efield_grid", Kind::Flag, false},
            {"property_grid_padding", Kind::Real, 4., .5, 20, 2},
            {"property_grid_points", Kind::Integer, 50, 5, 100},
            {"ord_start_nm", Kind::Real, 400., 100, 5000, 3},
            {"ord_end_nm", Kind::Real, 700., 100, 5000, 3},
            {"ord_points", Kind::Integer, 7, 2, 50},
            {"raman_wavelength_nm", Kind::Real, 532.0, 100, 5000, 3},
            project({"extra_input", Kind::String, ""}),
            project({"extra_tasks", Kind::String, ""}),
            project({"restart", Kind::Object, QJsonObject()})};
        for (auto &spec : list)
            for (const auto &choice : choiceSources(spec.key))
                spec.choices << NwchemChoice{choice.value, choice.label};
        return list;
    }();
    return specs;
}
const NwchemSettingSpec &nwchemSettingSpec(const QString &key) {
    for (const auto &spec : nwchemSettingSpecs())
        if (spec.key == key)
            return spec;
    fail(tr("不明なNWChem設定: %1").arg(key));
}
bool nwchemScopeApplies(NwchemScope scope, const QJsonObject &s, const Document *d) {
    const NwchemCalculation c(s);
    using Scope = NwchemScope;
    switch (scope) {
    case Scope::Always:
        return true;
    case Scope::Dft:
        return c.dft();
    case Scope::ReferenceScf:
        return !c.dft();
    case Scope::CustomXc:
        return c.dft() && s["functional"] == "custom";
    case Scope::Correlation:
        return c.correlated();
    case Scope::Mp2:
        return c.method == NwchemMethod::Mp2;
    case Scope::Ccsd:
        return c.ccsd();
    case Scope::Driver:
        return c.driver();
    case Scope::DriverOrIrc:
        return c.driver() || c.task == NwchemTask::Irc;
    case Scope::Optimization:
        return c.optimization();
    case Scope::UnconstrainedOptimization: // Constraints and relaxed scans need internal coordinates.
        return c.optimization() && s["constraints"].toArray().isEmpty();
    case Scope::Frequency:
        return c.frequencies();
    case Scope::NumericalHessian:
        return c.frequencies() && d && nwchemNumericalFrequencies(*d);
    case Scope::AnalyticHessian:
        return c.frequencies() && d && !nwchemNumericalFrequencies(*d);
    case Scope::Irc:
        return c.task == NwchemTask::Irc;
    case Scope::Scan:
        return c.task == NwchemTask::Scan;
    case Scope::Neb:
        return c.task == NwchemTask::Neb;
    case Scope::Followup:
        return s["followup"].toBool();
    case Scope::FollowupDft:
        return s["followup"].toBool() && s["followup_method"] == "dft";
    case Scope::Solvent:
        return s["solvent"] != "none";
    case Scope::Dielectric:
        return s["solvent"] == "cosmo" && s["solvent_name"] == "dielectric";
    case Scope::Cosmo:
        return s["solvent"] != "none" && (c.method == NwchemMethod::Hf || c.dft());
    case Scope::ExcitedToggle:
        return c.dft() && (c.task == NwchemTask::Energy || c.task == NwchemTask::Optimize);
    case Scope::Excited:
        return c.dft() && s["tddft"].toBool();
    case Scope::ExcitedOptimization:
        return c.dft() && s["tddft"].toBool() && c.task == NwchemTask::Optimize;
    case Scope::Relativistic:
        return c.method == NwchemMethod::Hf || c.dft();
    case Scope::SpinSpin:
        return s["prop_spinspin"].toBool();
    case Scope::Scan2:
        return c.task == NwchemTask::Scan && !s["scan2_atoms"].toArray().isEmpty();
    }
    return false;
}
QString nwchemScopeText(NwchemScope scope) {
    using Scope = NwchemScope;
    switch (scope) {
    case Scope::Always:
        return tr("すべての計算");
    case Scope::Dft:
        return tr("DFTの計算");
    case Scope::ReferenceScf:
        return tr("HF・MP2・CCSD（参照SCF）");
    case Scope::Correlation:
        return tr("MP2・CCSD・CCSD(T)");
    case Scope::Mp2:
        return "MP2";
    case Scope::Ccsd:
        return tr("CCSD・CCSD(T)");
    case Scope::Driver:
        return tr("構造最適化・遷移状態探索・緩和スキャン");
    case Scope::UnconstrainedOptimization:
        return tr("構造最適化・遷移状態探索（内部座標の拘束・スキャンなし）");
    case Scope::Frequency:
        return tr("振動数を含む計算");
    case Scope::NumericalHessian:
        return tr("数値Hessianを使う振動数");
    case Scope::AnalyticHessian:
        return tr("解析Hessianを使う振動数");
    case Scope::Cosmo:
        return tr("溶媒（COSMO・SMD）を使うHF・DFT");
    default:
        return {};
    }
}
bool nwchemSettingApplies(const NwchemSettingSpec &spec, const QJsonObject &s) {
    return nwchemScopeApplies(spec.scope, s);
}
bool nwchemSettingEditable(const NwchemSettingSpec &spec, const QJsonObject &settings) {
    return nwchemSettingApplies(spec, settings) || (spec.allowDisable && settings[spec.key] != spec.initial);
}
QJsonValue nwchemSettingValue(const Document &d, const QJsonObject &settings, const QString &key) {
    if (key == "charge")
        return d.charge;
    if (key == "multiplicity")
        return d.multiplicity;
    return settings[key];
}
void setNwchemSetting(Document &d, const QString &key, const QJsonValue &value) {
    const auto &spec = nwchemSettingSpec(key);
    if (spec.structural) {
        const double n = value.toDouble();
        if (!value.isDouble() || !std::isfinite(n) || std::floor(n) != n || n < spec.minimum ||
            n > spec.maximum)
            fail(tr("%1 は %2〜%3 の整数で指定してください。").arg(key).arg(spec.minimum).arg(spec.maximum));
        (key == "charge" ? d.charge : d.multiplicity) = int(n);
    } else {
        auto settings = d.data["nwchem"].toObject();
        settings[key] = value;
        d.data["nwchem"] = settings;
    }
}
QVector<NwchemChoice> nwchemChoices(const QString &key) {
    QVector<NwchemChoice> result;
    for (const auto &c : nwchemSettingSpec(key).choices)
        result << NwchemChoice{c.value, translated(c.label)};
    return result;
}
QVector<NwchemPreset> nwchemPresets() {
    auto hf = nwchemDefaults();
    auto dft = hf;
    dft["method"] = "dft";
    dft["basis"] = "def2-svp";
    dft["dispersion"] = "d3bj";
    dft["task"] = "optimize_frequencies";
    dft["grid"] = "xfine";
    auto mp2 = hf;
    mp2["method"] = "mp2";
    mp2["basis"] = "cc-pvdz";
    mp2["freeze_core"] = true;
    return {{tr("HF / STO-3G・エネルギー"), nwchemPresetSettings(hf)},
            {tr("B3LYP-D3(BJ) / def2-SVP・最適化＋振動数"), nwchemPresetSettings(dft)},
            {tr("MP2 / cc-pVDZ・エネルギー（内殻凍結）"), nwchemPresetSettings(mp2)}};
}
QJsonObject nwchemPresetSettings(const QJsonObject &stored) {
    const auto settings = withBasisFunctions(stored);
    QJsonObject preset;
    for (const auto &spec : nwchemSettingSpecs())
        if (spec.preset)
            preset[spec.key] = settings.contains(spec.key) ? settings[spec.key] : spec.initial;
    preset["advanced"] = nwchemAdvancedPreset(settings["advanced"].toObject());
    return preset;
}
void applyNwchemPreset(Document &d, const QJsonObject &settings) {
    auto stored = d.data["nwchem"].toObject();
    const auto preset = nwchemPresetSettings(settings);
    for (auto it = preset.begin(); it != preset.end(); ++it)
        if (it.key() != "advanced")
            stored[it.key()] = it.value();
    d.data["nwchem"] = stored;
    applyNwchemAdvancedPreset(d, preset["advanced"].toObject());
}
QString nwchemBasisLabel(const QJsonObject &settings) {
    // The usual spelling of the choices (def2-SVP, cc-pVDZ), without their notes ("（…）" of the
    // source label); others as stored.
    const auto name = [](const QString &value) {
        const auto *b = libraryBasis(value);
        return b ? QString(b->label).section(QChar(0xFF08), 0, 0) : value;
    };
    QString label = name(settings["basis"].toString());
    const auto overrides = settings["basis_overrides"].toObject();
    const auto p = pasted(settings);
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        if (!p.basis.lines.contains(it.key()))
            label = tr("%1、%2: %3").arg(label, it.key(), name(it.value().toString()));
    if (!p.basis.lines.isEmpty())
        label = tr("%1、貼り付け: %2").arg(label, p.basis.elements.join(tr("・")));
    if (!p.ecp.lines.isEmpty())
        label = tr("%1、貼り付けたECP: %2").arg(label, p.ecp.elements.join(tr("・")));
    return label;
}
QString nwchemFunctionalLabel(const QJsonObject &settings) {
    if (settings["functional"] == "custom")
        return "xc " + settings["xc_custom"].toString().simplified();
    const auto *f = functional(settings["functional"].toString());
    return f ? QString(f->value).toUpper() : settings["functional"].toString().toUpper();
}
bool nwchemNumericalFrequencies(const Document &d) {
    const auto s = nwchemSettings(d);
    const NwchemCalculation calc(s);
    const auto *f = functional(s["functional"].toString());
    // NWChem's analytic Hessian covers neither solvation, relativistic Hamiltonians nor CD fitting.
    if (s["numerical_frequencies"].toBool() || calc.method == NwchemMethod::Mp2 || s["reference"] == "rohf" ||
        (calc.dft() && f && f->numericalHessian) ||
        (s["reference"] == "auto" && calc.method == NwchemMethod::Hf && d.multiplicity > 1) ||
        s["solvent"] != "none" || s["relativistic"] != "none" || (calc.dft() && s["ri_j"].toBool()))
        return true;
    const auto p = pasted(s);
    for (const auto &atom : d.atoms)
        if (!atom.ghost) {
            try {
                if (coreElectrons(s, p, atom.element, element(atom.element).number))
                    return true;
            } catch (const std::exception &) {
                // Validation reports unknown atoms separately.
            }
        }
    return false;
}
static NwchemValidation validation(const Document &d);
NwchemValidation validateNwchem(const Document &d) {
    thread_local Memo<Document, NwchemValidation, 8> memo;
    return memo.get(d, [&] { return validation(d); });
}
static NwchemValidation validation(const Document &d) {
    NwchemValidation result;
    for (const auto &message : d.validate())
        result.error("geometry.validation", {}, message, {}, 0);
    auto &errors = result.errors;
    const auto s = nwchemSettings(d);
    const auto advanced = nwchemAdvanced(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    if (d.data.contains("nwchem") && !d.data["nwchem"].isObject())
        result.error("settings.object", "nwchem", tr("NWChem設定はオブジェクトである必要があります。"));
    for (const auto &spec : nwchemSettingSpecs()) {
        const auto value = nwchemSettingValue(d, s, spec.key);
        if (value.type() != spec.initial.type())
            result.error("settings.type", spec.key, tr("設定 %1 の型が不正です。").arg(spec.key));
        if (!spec.choices.isEmpty() &&
            std::none_of(spec.choices.begin(), spec.choices.end(),
                         [&](const auto &c) { return value.toString() == c.value; }))
            result.error("settings.choice", spec.key,
                         tr("設定 %1 の値「%2」は未対応です。").arg(spec.key, value.toString()));
        if (spec.kind == NwchemSettingKind::Integer &&
            (spec.validateUnused || nwchemSettingApplies(spec, s))) {
            const double n = value.toDouble();
            if (!value.isDouble() || !std::isfinite(n) || n < spec.minimum || n > spec.maximum ||
                std::floor(n) != n)
                result.error("settings.integer", spec.key,
                             tr("%1 は %2〜%3 の整数で指定してください。")
                                 .arg(spec.key)
                                 .arg(spec.minimum)
                                 .arg(spec.maximum));
        }
    }
    const auto outsideRange = [&](const QString &key) {
        const auto &spec = nwchemSettingSpec(key);
        const auto value = s[key];
        const double n = value.toDouble();
        return !value.isDouble() || !std::isfinite(n) || n < spec.minimum || n > spec.maximum;
    };
    if (!QRegularExpression("^[A-Za-z][A-Za-z0-9_-]{0,63}$").match(str("start")).hasMatch())
        result.error("start.syntax", "start",
                     tr("計算名は英字で始まる英数字・_・- の64文字以内で指定してください。"));
    if (str("title").size() > 120 || str("title").contains(QRegularExpression("[\\x00-\\x1f\\x7f\"\\\\;#]")))
        result.error("title.syntax", "title",
                     tr("タイトルは120文字以内で、改行・引用符・\\・;・#を含めず指定してください。"));
    if (outsideRange("dielectric"))
        result.error("dielectric.range", "dielectric", tr("誘電率は1〜1000で指定してください。"));
    const auto basis = str("basis");
    const NwchemCalculation calc(s);
    const int mult = d.multiplicity;
    const int charge = d.charge;
    int realAtoms = 0, fixed = 0, ghosts = 0, movable = 0;
    QSet<QString> unsupported, ecpLibraries, unfitted;
    const auto overrides = s["basis_overrides"].toObject();
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        bool known = true;
        try {
            element(it.key());
        } catch (const std::exception &) {
            known = false;
        }
        if (!known || !libraryBasis(it.value().toString()))
            result.error("basis.override", "basis_overrides",
                         tr("元素別の基底「%1: %2」は未対応です。").arg(it.key(), it.value().toString()));
    }
    // Bases and ECPs pasted in NWChem's format, for the elements they define.
    const auto paste = pasted(s);
    for (const auto &[key, read] :
         {std::pair{QString("basis_custom"), paste.basis}, std::pair{QString("ecp_custom"), paste.ecp}})
        if (!read.error.isEmpty())
            result.error("basis.pasted", key, read.error);
    QSet<QString> present;
    for (const auto &a : d.atoms)
        present.insert(a.element);
    QStringList unusedPasted, overridden;
    for (const auto &symbol : paste.basis.elements + paste.ecp.elements)
        if (!present.contains(symbol) && !unusedPasted.contains(symbol))
            unusedPasted << symbol;
    for (const auto &symbol : paste.basis.elements)
        if (present.contains(symbol) && overrides.contains(symbol))
            overridden << symbol;
    if (!unusedPasted.isEmpty())
        result.warning(
            "basis.pasted_unused", paste.basis.elements.isEmpty() ? "ecp_custom" : "basis_custom",
            tr("分子にない元素（%1）の貼り付けた基底・ECPは使いません。").arg(unusedPasted.join(", ")));
    if (!overridden.isEmpty())
        result.warning("basis.pasted_override", "basis_custom",
                       tr("%1 には元素別の基底より貼り付けた基底を使います。").arg(overridden.join(", ")));
    bool nonRelativisticBasis = false;
    for (const auto &a : d.atoms) {
        int z;
        try {
            z = element(a.element).number;
        } catch (const std::exception &) {
            continue; // Document validation already reports unknown elements.
        }
        const auto own = elementBasis(s, a.element);
        const bool ownPasted = paste.basis.lines.contains(a.element);
        if (!ownPasted && !basisCovers(own, z))
            unsupported.insert(own == basis ? a.element : tr("%1（%2）").arg(a.element, own));
        if (!fittingCovers(z))
            unfitted.insert(a.element);
        if (const auto *b = libraryBasis(own); !ownPasted && b && !b->relativistic && z > 36)
            nonRelativisticBasis = true;
        fixed += a.fixed;
        ghosts += a.ghost;
        if (!a.ghost) {
            ++realAtoms;
            movable += !a.fixed;
            const int core = coreElectrons(s, paste, a.element, z);
            if (core)
                ecpLibraries.insert(paste.ecp.cores.contains(a.element) ? tr("貼り付けたECP")
                                                                        : QString(libraryBasis(own)->ecp));
            result.ecpElectrons += core;
            result.electrons += z - core;
            if (!core && calc.correlated() && s["freeze_core"].toBool())
                result.frozenOrbitals += atomicCoreOrbitals(z);
        }
    }
    if (!unsupported.isEmpty())
        result.error("basis.coverage", "basis",
                     tr("基底 %1 の対応元素に含まれません: %2").arg(basis, sorted(unsupported).join(", ")));
    if (!realAtoms)
        result.error("geometry.empty", QString(),
                     tr("実原子を追加してください（ゴーストのみの計算はできません）。"), {}, 0);
    result.electrons -= charge;
    if (calc.correlated() && advanced.contains("corr_freeze"))
        result.frozenOrbitals = advanced["corr_freeze"].toInt();
    if (result.electrons < 1 || mult > result.electrons + 1 || (result.electrons - mult + 1) % 2 != 0)
        result.error(
            "spin.electrons", "multiplicity",
            tr("電子数 %1 と多重度 %2 が整合しません。全電荷または多重度を変更してください。")
                .arg(result.electrons)
                .arg(mult),
            DiagnosticFix{"multiplicity", tr("電子数に合う多重度にする"), result.electrons % 2 ? 2 : 1});
    else {
        result.alpha = (result.electrons + mult - 1) / 2;
        result.beta = (result.electrons - mult + 1) / 2;
    }
    result.reference = str("reference");
    if (result.reference == "auto")
        result.reference = mult == 1 ? "rhf" : (calc.method == NwchemMethod::Hf ? "rohf" : "uhf");
    if (result.reference == "rhf" && mult != 1)
        result.error("reference.closed_shell", "reference",
                     tr("RHF / RKSは多重度1で使用してください。開殻には自動または非制限を選択してください。"),
                     DiagnosticFix{"reference", tr("参照波動関数を「自動」にする"), "auto"});
    if ((calc.dft() || calc.method == NwchemMethod::Mp2) && result.reference == "rohf")
        result.error("reference.rohf", "reference",
                     tr("このタブのDFT・MP2はROHF参照に対応しません。自動または非制限を選択してください。"));
    if (calc.ccsd() && (result.reference != "rhf" || mult != 1 || calc.task != NwchemTask::Energy))
        result.error("correlation.reference", "method",
                     tr("CCSD / CCSD(T)は閉殻RHFの一点エネルギーに対応します。"));
    if (calc.dft()) {
        const auto *f = functional(str("functional"));
        const auto custom = str("xc_custom").simplified();
        if (f && QString(f->value) == "custom") {
            // One XC line inside the DFT block: names, numbers and signs only.
            if (custom.isEmpty() || custom.size() > 200 ||
                str("xc_custom").contains(QRegularExpression("[\\x00-\\x1f\\x7f]")) ||
                !QRegularExpression("^[A-Za-z0-9_.+\\- ]+$").match(custom).hasMatch())
                result.error("functional.custom", "xc_custom",
                             tr("任意のxcは英数字・_・.・+・- と空白の200文字以内で指定してください（例: "
                                "xpbe96 0.75 cpbe96 1.0 hfexch 0.25）。"));
            if (str("dispersion") != "none")
                result.error(
                    "dispersion.custom", "dispersion",
                    tr("任意のxcでは分散補正を選べません。NWChemにパラメータがない場合があるためです。"),
                    DiagnosticFix{"dispersion", tr("分散補正を「なし」にする"), "none"});
        } else if (f && ((str("dispersion") == "d3" && !f->d3) || (str("dispersion") == "d3bj" && !f->d3bj)))
            result.error("dispersion.parameters", "dispersion",
                         tr("%1 の%2はNWChemの公表パラメータと一致しないか、パラメータがありません。%3")
                             .arg(tr(f->label), str("dispersion") == "d3" ? "D3" : "D3(BJ)",
                                  QString(f->value) == "wb97x-d3"
                                      ? tr("ωB97X-D3はD3を含むため、分散補正は「なし」にしてください。")
                                  : f->d3   ? tr("D3または補正なしを選んでください。")
                                  : f->d3bj ? tr("D3(BJ)または補正なしを選んでください。")
                                            : tr("補正なしを選んでください。")),
                         DiagnosticFix{"dispersion", tr("分散補正を「なし」にする"), "none"});
    }
    if (s["tddft"].toBool()) {
        const auto *f = functional(str("functional"));
        if (!calc.dft())
            result.error("tddft.method", "tddft", tr("TDDFTはDFTで使用してください。"));
        else if (f && f->metaGga)
            result.error("tddft.functional", "functional",
                         tr("NWChemのTDDFTはメタGGA（%1）に対応しません。B3LYP・PBE0・CAM-"
                            "B3LYP等を選んでください。")
                             .arg(tr(f->label)));
        if (calc.task != NwchemTask::Energy && calc.task != NwchemTask::Optimize && !calc.path())
            result.error(
                "tddft.task", "task",
                tr("TDDFTは計算目的「一点エネルギー」または「構造最適化」（励起状態の最適化）で使用して"
                   "ください。"),
                DiagnosticFix{"task", tr("計算目的を「一点エネルギー」にする"), "energy"});
        if (calc.task == NwchemTask::Optimize) {
            if (s["tddft_root"].toInt() > s["tddft_roots"].toInt())
                result.error("tddft.root", "tddft_root",
                             tr("最適化する励起状態（%1）は励起状態の数（%2）以下にしてください。")
                                 .arg(s["tddft_root"].toInt())
                                 .arg(s["tddft_roots"].toInt()));
            if (result.reference == "rhf" && s["tddft_triplets"].toBool())
                result.error("tddft.triplet_optimization", "tddft_triplets",
                             tr("励起状態の最適化は一重項の励起状態に対応します。「三重項も計算」をオフにして"
                                "ください（三重項の構造は多重度3の基底状態として最適化できます）。"),
                             DiagnosticFix{"tddft_triplets", tr("三重項の計算をオフにする"), false});
            if (advanced["cube_output"].toBool() && nwchemCubeExcited(advanced["cube_type"].toString()))
                result.error("tddft.optimization_cube", "advanced:cube_type",
                             tr("励起状態の最適化では、励起状態のCUBEを出力できません。最適化した構造で一点"
                                "計算をしてください。"));
            result.warning(
                "tddft.optimization", "tddft_root",
                tr("励起状態 %1 の構造を最適化します（TDDFTの解析勾配）。最適化の途中で状態の順番が"
                   "入れ替わると、同じ番号の別の状態を追います。")
                    .arg(s["tddft_root"].toInt()));
        }
    }
    if (calc.correlated() && result.electrons < 2)
        result.error("correlation.electrons", "method", tr("相関計算には2個以上の電子が必要です。"));
    if (result.frozenOrbitals &&
        (result.frozenOrbitals > result.beta || result.electrons - 2 * result.frozenOrbitals < 2))
        result.error("correlation.frozen", "freeze_core",
                     tr("内殻凍結後の相関電子が不足します。内殻凍結を解除してください。"));
    if (calc.correlated() && (s["freeze_core"].toBool() || advanced.contains("corr_freeze")))
        result.warning("correlation.freeze_notice", "freeze_core",
                       tr("内殻凍結: %n 軌道。%1", nullptr, result.frozenOrbitals)
                           .arg(advanced.contains("corr_freeze")
                                    ? tr("詳細設定の明示的な軌道数を使用します。")
                                    : tr("ECP中心とゴーストはfreeze atomicの対象外です。")));
    const auto solvent = str("solvent"), solventName = str("solvent_name");
    if (solvent != "none") {
        const QString model = solvent == "smd" ? "SMD" : "COSMO";
        if (calc.method != NwchemMethod::Hf && !calc.dft())
            result.error("solvent.method", "solvent", tr("%1はHFまたはDFTで使用してください。").arg(model));
        if (solvent == "smd" && !calc.dft())
            result.error("solvent.smd_method", "solvent",
                         tr("NWChemのSMDはDFT専用です。HFにはCOSMOを使用してください。"),
                         DiagnosticFix{"solvent", tr("溶媒モデルをCOSMOにする"), "cosmo"});
        if (solvent == "smd" && solventName == "dielectric")
            result.error("solvent.smd_name", "solvent_name",
                         tr("SMDでは溶媒名を選んでください（誘電率だけでは非静電項を決められません）。"),
                         DiagnosticFix{"solvent_name", tr("溶媒を水にする"), "h2o"});
        if (ghosts)
            result.error("solvent.ghosts", "solvent", tr("ゴーストを含む%1入力は未対応です。").arg(model));
        if (calc.method == NwchemMethod::Hf && result.reference == "uhf")
            result.error(
                "solvent.uhf", "reference",
                tr("NWChemのCOSMOはUHFに対応しません。参照を自動（ROHF）にするか、DFTを選択してください。"));
    }
    const auto relativistic = str("relativistic");
    if (relativistic != "none") {
        if (calc.method != NwchemMethod::Hf && !calc.dft())
            result.error("relativistic.method", "relativistic",
                         tr("相対論ハミルトニアンはHFまたはDFTで使用してください。"));
        if (relativistic == "zora" && !calc.dft())
            result.error("relativistic.zora", "relativistic",
                         tr("NWChemのZORAはDFT専用です。HFにはDKHまたはX2Cを選んでください。"),
                         DiagnosticFix{"relativistic", tr("相対論をDKHにする"), "dkh"});
        if (result.ecpElectrons)
            result.error(
                "relativistic.ecp", "basis",
                tr("相対論ハミルトニアンはECPと併用できません。x2c-SVPall等の全電子基底を選んでください。"));
        else if (nonRelativisticBasis)
            result.warning("relativistic.basis", "basis",
                           tr("重元素に非相対論の基底を使用しています。x2c-"
                              "SVPall・SARC等の相対論用の基底を推奨します。"));
    }
    if (s["ri_j"].toBool()) {
        if (!calc.dft())
            result.error("ri_j.method", "ri_j", tr("RI-J（Coulombフィッティング）はDFTで使用してください。"),
                         DiagnosticFix{"ri_j", tr("RI-Jをオフにする"), false});
        if (!unfitted.isEmpty())
            result.error("ri_j.coverage", "ri_j",
                         tr("Weigendのフィッティング基底の対応元素に含まれません: %1")
                             .arg(sorted(unfitted).join(", ")));
        result.warning("ri_j.approximation", "ri_j",
                       tr("RI-J（weigend coulomb fitting）でCoulomb項を近似します。全エネルギーは通常の計算と"
                          "わずかに異なります。"));
    }
    // Internal-coordinate constraints (ZCOORD), scans and reaction paths.
    const auto constraints = s["constraints"].toArray();
    if (constraints.size() > 50)
        result.error("constraints.count", "constraints", tr("内部座標の拘束・追加は50個までです。"));
    for (int i = 0; i < constraints.size(); ++i) {
        const auto c = constraints[i].toObject();
        const auto ids = idsFromJson(c["atoms"]);
        // An added coordinate has no value: only its atoms are checked.
        const auto error = c["free"].toBool()      ? coordinateError(d, ids,
                                                                     ids.size() == 2   ? 1.0
                                                                     : ids.size() == 3 ? 90.0
                                                                                       : 0.0)
                           : c["value"].isDouble() ? coordinateError(d, ids, c["value"].toDouble())
                                                   : tr("値を数値で指定してください。");
        if (!error.isEmpty())
            result.error("constraints.coordinate", "constraints",
                         tr("内部座標%1: %2").arg(QString::number(i + 1), error));
    }
    if (!constraints.isEmpty()) {
        if (!calc.optimization() && !(calc.task == NwchemTask::Scan && s["scan_relaxed"].toBool()))
            result.error(
                "constraints.task", "constraints",
                tr("内部座標の拘束・追加は構造最適化・遷移状態探索・緩和スキャンで使用してください。"));
        if (fixed || ghosts)
            result.error(
                "constraints.fixed", "constraints",
                tr("内部座標の拘束・追加は固定原子・ゴーストと併用できません（NWChemはCartesianの固定と"
                   "内部座標を同時に扱えません）。固定を解除してください。"));
        result.warning("constraints.autoz", "constraints",
                       constrained(s)
                           ? tr("拘束付き最適化は内部座標（autoz）で行います。NWChemが内部座標を生成できない"
                                "構造では拘束を適用できません。")
                           : tr("追加した内部座標は、NWChemが自動で作る内部座標（autoz）に加えて最適化に"
                                "使います。"));
        if (calc.frequencies() && constrained(s))
            result.warning(
                "constraints.frequencies", "constraints",
                tr("拘束した構造は停留点ではないため、振動数には虚振動や大きな誤差が出ることがあります。"));
    }
    const bool scan = calc.task == NwchemTask::Scan;
    if (scan) {
        const auto ids = idsFromJson(s["scan_atoms"]);
        if (ids.isEmpty())
            result.error("scan.selection", "scan_atoms",
                         tr("スキャンする座標を「1 分子」で2〜4原子選択して指定してください。"));
        else
            for (const char *key : {"scan_start", "scan_end"}) {
                const auto error = coordinateError(d, ids, s[key].toDouble());
                if (!error.isEmpty())
                    result.error(
                        "scan.range", QString::fromLatin1(key),
                        (key == QString("scan_start") ? tr("スキャン開始値: %1") : tr("スキャン終了値: %1"))
                            .arg(error));
            }
        // The second coordinate of a two-dimensional scan.
        const auto second = idsFromJson(s["scan2_atoms"]);
        const bool plane = !second.isEmpty();
        if (plane) {
            for (const char *key : {"scan2_start", "scan2_end"}) {
                const auto error = coordinateError(d, second, s[key].toDouble());
                if (!error.isEmpty())
                    result.error("scan.range", QString::fromLatin1(key),
                                 (key == QString("scan2_start") ? tr("スキャン座標2の開始値: %1")
                                                                : tr("スキャン座標2の終了値: %1"))
                                     .arg(error));
            }
            auto reversed = second;
            std::reverse(reversed.begin(), reversed.end());
            if (second == ids || reversed == ids)
                result.error("scan.same", "scan2_atoms", tr("スキャン座標1と2に別の座標を選んでください。"));
            const int points = s["scan_steps"].toInt() * s["scan2_steps"].toInt();
            if (points > 400)
                result.error("scan.points", "scan2_steps",
                             tr("2次元スキャンの点は400点までです（今 %1 点）。点の数を減らしてください。")
                                 .arg(points));
        }
        if (errors.isEmpty()) {
            try {
                for (int step = 0; step < s["scan_steps"].toInt(); ++step)
                    for (int step2 = 0; step2 < (plane ? s["scan2_steps"].toInt() : 1); ++step2) {
                        const auto point = scanGeometry(d, s, step, step2);
                        if (plane && (coordinateDeviation(point, ids, scanValue(s, step)) > 1e-3 ||
                                      coordinateDeviation(point, second, scanValue(s, step2, 2)) > 1e-3))
                            fail(tr("座標1と2が同じ原子を動かすため、両方の値にできない点があります（点 %1, "
                                    "%2）。"
                                    "互いに動かし合わない座標を選んでください。")
                                     .arg(step + 1)
                                     .arg(step2 + 1));
                    }
            } catch (const std::exception &e) {
                result.error("scan.geometry", "scan_atoms",
                             tr("スキャン: %1").arg(QString::fromUtf8(e.what())));
            }
        }
        if (plane)
            result.warning("scan.plane", "scan2_atoms",
                           tr("2次元スキャン: %1 × %2 = %3 点を計算します。")
                               .arg(s["scan_steps"].toInt())
                               .arg(s["scan2_steps"].toInt())
                               .arg(s["scan_steps"].toInt() * s["scan2_steps"].toInt()));
        if (s["scan_relaxed"].toBool() && (fixed || ghosts))
            result.error(
                "scan.fixed", "scan_relaxed",
                tr("緩和スキャンは固定原子・ゴーストと併用できません。固定を解除するか、固定スキャンにして"
                   "ください。"));
        if (s["scan_relaxed"].toBool())
            result.warning(
                "scan.relaxed", "scan_relaxed",
                tr("緩和スキャンでは各点でスキャン座標を固定して最適化します（内部座標・autoz）。"));
        else
            result.warning(
                "scan.rigid", "scan_relaxed",
                tr("固定スキャンでは、後に選択した原子の側を動かした構造で一点エネルギーを計算します。"));
    }
    if (calc.task == NwchemTask::Irc) {
        if (fixed || ghosts)
            result.error("irc.fixed", "task", tr("IRCは固定原子・ゴーストを含む構造に対応しません。"));
        if (realAtoms < 3)
            result.error("irc.atoms", "task", tr("IRCには3個以上の実原子が必要です。"));
        result.warning(
            "irc.initial", "irc_direction",
            (str("irc_direction") == "both"
                 ? tr("IRCは遷移状態の構造から始めてください（遷移状態探索と振動数で虚振動が1つである"
                      "ことを確認）。"
                      "初めに遷移状態のHessianを計算し、両方向へ経路を追跡します。")
                 : tr("IRCは遷移状態の構造から始めてください（遷移状態探索と振動数で虚振動が1つである"
                      "ことを確認）。"
                      "初めに遷移状態のHessianを計算し、指定した方向だけ経路を追跡します。")));
    }
    if (calc.task == NwchemTask::Neb) {
        if (fixed || ghosts)
            result.error("neb.fixed", "task", tr("NEBは固定原子・ゴーストを含む構造に対応しません。"));
        if (realAtoms < 2)
            result.error("neb.atoms", "task", tr("NEBには2個以上の実原子が必要です。"));
        if (outsideRange("neb_spring"))
            result.error("neb.spring", "neb_spring", tr("NEBのばね定数は0.001〜100で指定してください。"));
        if (outsideRange("neb_stepsize"))
            result.error("neb.stepsize", "neb_stepsize", tr("NEBの刻み幅は0.001〜10で指定してください。"));
        if (advanced["cube_output"].toBool() || advanced["molden_output"].toBool())
            result.error("neb.output", "task",
                         tr("NEBの計算ではCUBE・MOLDENを出力できません。経路の最高点などの構造で別に計算して"
                            "ください。"));
        if (const auto end = nwchemNebEnd(d); !end.document)
            result.error("neb.end", "neb_end", end.problem);
        else {
            double largest = 0;
            for (int i = 0; i < d.atoms.size(); ++i)
                largest = std::max(largest, (end.document->atoms[i].position - d.atoms[i].position).norm());
            if (largest < .01)
                result.error(
                    "neb.same", "neb_end",
                    tr("NEBの終点が始点（「1 分子」の構造）と同じです。終点を設定し直してください。"));
            else if (d.atoms.size() <= 300) {
                // NWChem starts from the straight line between the ends: atoms that meet on it.
                const auto path = nwchemNebInitialPath(d, *end.document, s["neb_beads"].toInt());
                double closest = INFINITY;
                int bead = 0, first = 0, second = 0;
                for (int k = 1; k + 1 < path.size(); ++k)
                    for (int i = 0; i < d.atoms.size(); ++i)
                        for (int j = i + 1; j < d.atoms.size(); ++j)
                            if (const double r =
                                    (path[k].atoms[i].position - path[k].atoms[j].position).norm();
                                r < closest) {
                                closest = r;
                                bead = k;
                                first = i;
                                second = j;
                            }
                if (closest < .5)
                    result.warning("neb.contact", "neb_end",
                                   tr("最初の経路（始点と終点を直線で結んだ像）の像 %1 で、原子 %2 と %3 が "
                                      "%4 Å まで近づきます。終点の原子の順番と向きを確かめてください。")
                                       .arg(bead + 1)
                                       .arg(first + 1)
                                       .arg(second + 1)
                                       .arg(closest, 0, 'f', 2));
            }
        }
        result.warning("neb.notice", "task",
                       tr("NEBは始点（「1 分子」の構造）と終点を固定し、その間の像を最適化します。経路の"
                          "最高点の構造から遷移状態探索へ進めます。"));
    }
    if (outsideRange("irc_stride"))
        result.error("irc.stride", "irc_stride", tr("IRCの刻み幅は0.01〜1で指定してください。"));
    if (s["tddft"].toBool() && calc.path())
        result.error("tddft.path", "tddft", tr("TDDFTはスキャン・IRC・NEBと併用できません。"));
    // Counterpoise correction of the bonded molecules as monomers.
    if (s["bsse"].toBool()) {
        const auto monomers = bsseMonomers(d);
        if (calc.task != NwchemTask::Energy && calc.task != NwchemTask::Gradient &&
            calc.task != NwchemTask::Optimize)
            result.error("bsse.task", "bsse",
                         tr("BSSE補正は一点エネルギー・勾配・構造最適化で使用してください。"));
        if (monomers.size() < 2)
            result.error("bsse.monomers", "bsse",
                         tr("BSSE補正には結合していない2個以上の分子（単量体）が必要です。"));
        if (monomers.size() > 20)
            result.error("bsse.count", "bsse", tr("BSSE補正の単量体は20個までです。"));
        if (ghosts)
            result.error(
                "bsse.ghosts", "bsse",
                tr("BSSE補正は単量体ごとのゴーストを自動で作るため、ゴースト中心と併用できません。"));
        if (solvent != "none" || s["tddft"].toBool() || anyProperty(s) || s["followup"].toBool())
            result.error("bsse.combination", "bsse",
                         tr("BSSE補正は溶媒・TDDFT・物性計算・後続の一点計算と併用できません。"));
        if (!d.pointCharges.isEmpty() && s["include_point_charges"].toBool())
            result.error("bsse.point_charges", "bsse", tr("BSSE補正は外部点電荷と併用できません。"));
        if (advanced["cube_output"].toBool() || advanced["molden_output"].toBool())
            result.error("bsse.output", "bsse", tr("BSSE補正の計算ではCUBE・MOLDENを出力できません。"));
        int total = 0;
        bool openShell = false;
        for (const auto &monomer : monomers) {
            const int q = monomerCharge(d, monomer);
            total += q;
            int electrons = -q;
            for (int i : monomer)
                try {
                    const int z = element(d.atoms[i].element).number;
                    electrons += z - coreElectrons(s, paste, d.atoms[i].element, z);
                } catch (const std::exception &) {
                    // Document validation reports unknown elements.
                }
            openShell |= electrons % 2 != 0;
        }
        if (monomers.size() >= 2 && total != charge)
            result.error(
                "bsse.charge", "charge",
                tr("BSSE補正の単量体の電荷は原子の形式電荷から決めます。全電荷 %1 を形式電荷の合計 %2 "
                   "に合わせてください。")
                    .arg(charge)
                    .arg(total));
        if (openShell || mult != 1)
            result.error("bsse.spin", "multiplicity",
                         tr("BSSE補正は閉殻の単量体からなる一重項に対応します。"));
        if (monomers.size() >= 2)
            result.warning("bsse.notice", "bsse",
                           tr("BSSE補正（counterpoise）: 単量体 %n 個。単量体ごとに相手の基底をゴーストとして"
                              "計算します。",
                              nullptr, int(monomers.size())));
    }
    // Molecular properties after the last calculation (the follow-up one if present).
    if (anyProperty(s)) {
        const auto level = s["followup"].toBool() ? s["followup_method"].toString() : str("method");
        if (level != "hf" && level != "dft")
            result.error("properties.method", "prop_mulliken",
                         tr("物性計算は最後の計算レベルがHFまたはDFTのときに使用してください。"));
        if (calc.path())
            result.error("properties.path", "prop_mulliken",
                         tr("物性計算はスキャン・IRC・NEBと併用できません。"));
        if (ghosts && s["prop_esp"].toBool())
            result.error("esp.ghosts", "prop_esp", tr("ESP電荷はゴースト中心を含む構造に対応しません。"));
        if ((s["prop_nmr"].toBool() || s["prop_polarizability"].toBool()) && result.reference == "rohf")
            result.error("properties.rohf", "reference",
                         tr("NMR・分極率はROHFに対応しません。自動（UHF）またはDFTを選んでください。"));
        if (s["prop_nmr"].toBool())
            result.warning(
                "nmr.reference", "prop_nmr",
                tr("NMR遮蔽定数（GIAO）を計算します。化学シフトには同じ条件の基準物質（TMS等）の値との差を"
                   "使ってください。"));
    }
    // NMR spin–spin couplings: a DFT response of each nucleus; NWChem computes them for UKS only.
    if (s["prop_spinspin"].toBool()) {
        const auto level = s["followup"].toBool() ? s["followup_method"].toString() : str("method");
        if (level != "dft")
            result.error(
                "spinspin.method", "prop_spinspin",
                tr("J結合（スピン–スピン結合定数）は最後の計算レベルがDFTのときに使用してください。"));
        if (ghosts)
            result.error("spinspin.ghosts", "prop_spinspin",
                         tr("J結合はゴースト中心を含む構造に対応しません。"));
        const int pairs = nwchemSpinSpinPairs(d).size();
        if (str("spinspin_pairs") == "hfp" && pairs == 0)
            result.error("spinspin.nuclei", "spinspin_pairs",
                         tr("H・F・Pの原子が2個以上必要です。「すべての原子の組」も選べます。"));
        if (pairs > 2000)
            result.error("spinspin.pairs", "spinspin_pairs",
                         tr("J結合の原子の組は2000組までです（今 %1 組）。").arg(pairs));
        if (pairs > 0 && pairs <= 2000)
            result.warning("spinspin.notice", "prop_spinspin",
                           tr("J結合（FC・SD・PSO・DSO項、Hz）を %n 組について計算します。核ごとに応答計算を"
                              "行うため時間がかかります。閉殻でもUKS（odft）で計算します。",
                              nullptr, pairs));
    }
    // Initial orbitals from the orbitals of fragments, computed first (vectors input fragment).
    if (const auto list = s["fragment_guess"].toArray(); !list.isEmpty()) {
        const auto fragments = nwchemFragments(d);
        const auto release =
            DiagnosticFix{"fragment_guess", tr("フラグメント初期軌道を解除する"), QJsonArray()};
        if (fragments.size() < 2 || fragments.size() > 20)
            result.error("fragments.count", "fragment_guess",
                         tr("フラグメントは2〜20個で指定してください（今 %1 個）。").arg(fragments.size()),
                         release);
        QVector<int> owner(d.atoms.size(), -1);
        int total = 0, unpaired = 0, missingIds = 0;
        bool overlap = false;
        for (int k = 0; k < list.size(); ++k) {
            const auto o = list[k].toObject();
            const auto ids = idsFromJson(o["atoms"]);
            const auto &charge = o["charge"], &multiplicity = o["multiplicity"];
            if (!charge.isDouble() || std::floor(charge.toDouble()) != charge.toDouble() ||
                std::abs(charge.toDouble()) > 1000 || !multiplicity.isDouble() ||
                std::floor(multiplicity.toDouble()) != multiplicity.toDouble() ||
                multiplicity.toDouble() < 1 || multiplicity.toDouble() > 1000)
                result.error(
                    "fragments.values", "fragment_guess",
                    tr("フラグメント%1の電荷・多重度は整数（多重度は1以上）で指定してください。").arg(k + 1));
            missingIds += int(ids.size() - (k < fragments.size() ? fragments[k].atoms.size() : 0));
            if (k >= fragments.size())
                continue;
            const auto &fragment = fragments[k];
            if (fragment.atoms.isEmpty())
                result.error("fragments.empty", "fragment_guess",
                             tr("フラグメント%1に原子がありません。").arg(k + 1));
            int electrons = -fragment.charge;
            for (int i : fragment.atoms) {
                overlap |= owner[i] >= 0;
                owner[i] = k;
                try {
                    const int z = element(d.atoms[i].element).number;
                    electrons += z - coreElectrons(s, paste, d.atoms[i].element, z);
                } catch (const std::exception &) {
                }
            }
            if (electrons < 1 || fragment.multiplicity > electrons + 1 ||
                (electrons - fragment.multiplicity + 1) % 2 != 0)
                result.error("fragments.spin", "fragment_guess",
                             tr("フラグメント%1の電子数 %2 と多重度 %3 が整合しません。")
                                 .arg(k + 1)
                                 .arg(electrons)
                                 .arg(fragment.multiplicity));
            total += fragment.charge;
            unpaired += fragment.multiplicity - 1;
        }
        const int uncovered = int(std::count(owner.begin(), owner.end(), -1));
        if (missingIds)
            result.error("fragments.atoms", "fragment_guess",
                         tr("フラグメントにない原子が %n 個指定されています。設定し直してください。", nullptr,
                            missingIds),
                         release);
        if (overlap)
            result.error("fragments.overlap", "fragment_guess",
                         tr("同じ原子を2つのフラグメントに含めることはできません。"), release);
        if (uncovered)
            result.error("fragments.cover", "fragment_guess",
                         tr("どのフラグメントにも含まれない原子が %n 個あります。すべての原子をどれか1つの"
                            "フラグメントに含めてください。",
                            nullptr, uncovered));
        if (fragments.size() >= 2 && total != charge)
            result.error("fragments.charge", "fragment_guess",
                         tr("フラグメントの電荷の合計 %1 が全電荷 %2 と異なります。").arg(total).arg(charge));
        if (fragments.size() >= 2 && unpaired != mult - 1)
            result.error(
                "fragments.unpaired", "fragment_guess",
                tr("フラグメントの不対電子の合計 %1 が全体（多重度 %2 → "
                   "%3）と異なります。NWChemのフラグメント"
                   "初期軌道は開殻の数が一致するとき（閉殻どうし、高スピン）に使えます。反強磁性（broken-"
                   "symmetry）の初期軌道は作れません。")
                    .arg(unpaired)
                    .arg(mult)
                    .arg(mult - 1));
        if (ghosts)
            result.error("fragments.ghosts", "fragment_guess",
                         tr("フラグメント初期軌道はゴースト中心と併用できません。"), release);
        if (calc.path() || s["bsse"].toBool())
            result.error("fragments.task", "fragment_guess",
                         tr("フラグメント初期軌道は座標スキャン・IRC・NEB・BSSE補正と併用できません。"),
                         release);
        if (advanced.contains("vectors_guess"))
            result.error("fragments.vectors", "advanced:vectors_guess",
                         tr("フラグメント初期軌道と、詳細設定の初期分子軌道は同時に指定できません。"));
        if (!s["restart"].toObject().isEmpty())
            result.error("fragments.restart", "fragment_guess",
                         tr("再開では再開元の分子軌道を使うため、フラグメント初期軌道は使えません。"),
                         release);
        if (fragments.size() >= 2)
            result.warning(
                "fragments.notice", "fragment_guess",
                tr("フラグメント %n 個の分子軌道を先に計算し、全体の初期軌道にします（vectors input "
                   "fragment）。",
                   nullptr, int(fragments.size())));
    }
    if ((s["prop_esp_grid"].toBool() || s["prop_efield_grid"].toBool()) && (ghosts || s["bsse"].toBool()))
        result.error("property_grid.geometry", "prop_esp_grid",
                     tr("ESP・電場の格子出力はゴースト中心・BSSEと併用できません。"));
    if (s["prop_ord"].toBool() &&
        ((!calc.dft() && calc.method != NwchemMethod::Hf) || mult != 1 || result.reference != "rhf" ||
         s["followup"].toBool() || s["bsse"].toBool() || calc.path() || ghosts ||
         s["ord_start_nm"].toDouble() >= s["ord_end_nm"].toDouble()))
        result.error("ord.method", "prop_ord",
                     tr("ORDは閉殻HF・DFTで、開始波長を終了波長より小さくしてください。後続計算・BSSE・スキャ"
                        "ン・IRC・NEB・ゴースト中心には対応しません。"));
    if (s["prop_epr"].toBool() && (!calc.dft() || mult != 2 || result.reference != "uhf" ||
                                   s["relativistic"] != "zora" || s["followup"].toBool() || ghosts))
        result.error("epr.method", "prop_epr",
                     tr("ESR物性は二重項の非制限DFT・ZORAで使用してください。後続計算・ゴースト中心との併用に"
                        "は対応しません。"));
    if (s["prop_raman"].toBool() &&
        (!calc.dft() || !calc.frequencies() || calc.saddle() || s["followup"].toBool() ||
         s["tddft"].toBool() || s["bsse"].toBool() || mult != 1 || result.reference == "uhf" || ghosts))
        result.error("raman.method", "prop_raman",
                     tr("通常ラマンは閉殻DFTの振動数または最適化＋振動数で使用してください。後続計算・TDDFT・"
                        "BSSE・ゴースト中心との併用には対応しません。"));
    if (s["followup"].toBool()) {
        if (!calc.optimization())
            result.error("followup.task", "followup",
                         tr("後続の一点計算は構造最適化・遷移状態探索の後に使用してください。"));
        if (s["tddft"].toBool())
            result.error("followup.tddft", "followup", tr("後続の一点計算とTDDFTは併用できません。"));
    }
    const QString extraInput = str("extra_input"), extraTasks = str("extra_tasks");
    for (const auto &[key, label, text] :
         {std::tuple{QString("extra_input"), tr("追加入力（タスク前）: %1"), extraInput},
          std::tuple{QString("extra_tasks"), tr("追加入力（末尾）: %1"), extraTasks}})
        if (const auto error = extraInputError(text); !error.isEmpty())
            result.error("extra.syntax", key, label.arg(error));
    if (!extraInput.trimmed().isEmpty() || !extraTasks.trimmed().isEmpty())
        result.warning(
            "extra.unchecked",
            extraInput.trimmed().isEmpty() ? QString("extra_tasks") : QString("extra_input"),
            tr("追加入力はMolFormaが検査せずにそのまま出力します。入力プレビューで内容を確認してください。"));
    if (calc.frequencies() && (ghosts || fixed))
        result.error("frequencies.fixed", "task",
                     tr("固定原子・ゴーストを含む振動数解析は未対応です。固定を解除し、実原子のみで設定してく"
                        "ださい。"));
    if (calc.frequencies() && realAtoms < 2)
        result.error("frequencies.atoms", "task", tr("振動数解析には2個以上の実原子が必要です。"));
    if (calc.optimization() && realAtoms && !movable)
        result.error("optimization.fixed", "task",
                     tr("全原子が固定されています。最適化する原子の固定を解除してください。"));
    if (calc.optimization() && ghosts)
        result.warning(
            "optimization.ghosts", "task",
            tr("ゴースト中心は核を持たないため、最適化中は固定します（constraints の fix atom）。"));
    const bool points = !d.pointCharges.isEmpty();
    if (calc.frequencies() && points && s["include_point_charges"].toBool())
        result.error("frequencies.point_charges", "include_point_charges",
                     tr("外部点電荷中の振動数解析は未対応です。"));
    result.numericalFrequencies = nwchemNumericalFrequencies(d);
    if (calc.frequencies() && result.numericalFrequencies)
        result.warning("frequencies.numerical", "numerical_frequencies",
                       tr("振動数には数値Hessianを使用します。複数回の勾配計算を行います。"));
    if (calc.frequencies() && s["scf_convergence"] == "default")
        result.warning(
            "frequencies.convergence", "scf_convergence",
            tr("SCF収束がNWChem既定のため、振動数（特に数値Hessian）の精度が不足することがあります。"
               "「厳密」を推奨します。"));
    if (calc.task == NwchemTask::Frequencies)
        result.warning("frequencies.geometry", "task",
                       tr("現在の座標で振動数を計算します。極小構造の確認には先に最適化してください。"));
    if (calc.saddle())
        result.warning(
            "saddle.initial", "task",
            tr("遷移状態探索は、反応座標に沿って1方向だけ負の曲率を持つ初期構造から始めてください。"
               "振動数で虚振動が1つであることを確認してください。"));
    if (result.ecpElectrons)
        result.warning("basis.ecp", paste.ecp.lines.isEmpty() ? "basis" : "ecp_custom",
                       (paste.ecp.lines.isEmpty() ? tr("ECP（%1）を自動指定します（除去する内殻電子: %2）。")
                                                  : tr("ECP（%1）を使います（除去する内殻電子: %2）。"))
                           .arg(sorted(ecpLibraries).join(tr("・")), QString::number(result.ecpElectrons)));
    QSet<bool> conventions{cartesianByConvention(basis)};
    for (const auto &a : d.atoms)
        if (!paste.basis.lines.contains(a.element))
            conventions.insert(cartesianByConvention(elementBasis(s, a.element)));
    if (conventions.size() > 1)
        result.warning(
            "basis.mixed_convention", "basis_overrides",
            tr("元素別の基底に、共通の基底とd関数の慣例（Cartesian / 球面）が異なるものがあります。"
               "basis指示は全元素に共通のため、d・f関数の形式を確認してください。"));
    const auto functions = str("basis_functions");
    if (functions != "auto" && basis != "sto-3g" &&
        (functions == "cartesian") != cartesianByConvention(basis))
        result.warning(
            "basis.convention", "basis_functions",
            (cartesianByConvention(basis)
                 ? tr("基底 %1 は通常Cartesian（6d）で使用します。文献値と比べる場合はd・f関数の形式を"
                      "「自動」にしてください。")
                 : tr("基底 %1 は通常球面調和関数（5d）で使用します。文献値と比べる場合はd・f関数の形式を"
                      "「自動」にしてください。"))
                .arg(basis));
    if (s["tddft"].toBool() && calc.dft() && result.reference == "uhf" && s["tddft_triplets"].toBool())
        result.warning("tddft.open_shell", "tddft_triplets",
                       tr("開殻（UKS）のTDDFTは一重項・三重項を分けずに非制限の励起状態を求めます。"));
    if (calc.frequencies())
        if (const int sigma = rotationalSymmetryNumber(d); sigma > 1)
            result.warning(
                "frequencies.symmetry", "task",
                tr("回転対称数σ=%1と推定されます。noautosymのためNWChemの熱化学はσ=1で計算されます。"
                   "エントロピーを R ln σ = %2 cal/(mol·K) 小さく、自由エネルギーを RT ln σ = %3 "
                   "kcal/mol（298.15 K）大きく補正してください。")
                    .arg(sigma)
                    .arg(1.987204 * std::log(sigma), 0, 'f', 3)
                    .arg(0.0019872043 * 298.15 * std::log(sigma), 0, 'f', 3));
    if (d.hasCenters() && charge != d.expectedCharge())
        result.warning(
            "charge.centers", "charge",
            tr("全電荷 %1 は配位中心の酸化数（補助）と配位子の形式電荷から見込まれる %2 と異なります。")
                .arg(charge)
                .arg(d.expectedCharge()));
    else if (!d.hasCenters() && charge != d.formalCharge())
        result.warning(
            "charge.formal", "charge",
            tr("全電荷 %1 は原子の形式電荷の合計 %2 と異なります。").arg(charge).arg(d.formalCharge()));
    if (points && !s["include_point_charges"].toBool())
        result.warning("point_charges.omitted", "include_point_charges",
                       tr("保存された点電荷は入力に含めません。必要なら外部点電荷を有効にしてください。"));
    try {
        if (!d.atoms.isEmpty())
            nwchemGeometry(d,
                           CoordinateUnit::Angstrom); // Also validate isotope masses before enabling export.
    } catch (const std::exception &e) {
        result.error("geometry.invalid", QString(), QString::fromUtf8(e.what()), {}, 0);
    }
    validateNwchemAdvanced(d, result);
    if (s["followup"].toBool() && errors.isEmpty()) {
        const auto next = followupDocument(d);
        const auto check = validateNwchem(next);
        for (const auto &diagnostic : check.diagnostics)
            if (diagnostic.severity == DiagnosticSeverity::Error)
                result.error("followup." + diagnostic.code, "followup",
                             tr("後続の一点計算: %1").arg(diagnostic.message));
        const auto ns = nwchemSettings(next);
        const auto nextPaste = pasted(ns);
        QSet<QString> mainEcp, nextEcp;
        for (const auto &a : d.atoms)
            if (!a.ghost) {
                if (coreElectrons(s, paste, a.element, element(a.element).number))
                    mainEcp.insert(a.element);
                if (coreElectrons(ns, nextPaste, a.element, element(a.element).number))
                    nextEcp.insert(a.element);
            }
        if (!mainEcp.isEmpty() && nextEcp.isEmpty())
            result.error(
                "followup.ecp", "followup_basis",
                tr("主計算のECPはNWChemの実行中に残るため、後続の一点計算にもECPを使う基底（def2系・LANL2DZ）"
                   "を選んでください。"));
        result.warning("followup.notice", "followup",
                       tr("最適化した構造で %1 / %2 の一点エネルギーを計算します。")
                           .arg(NwchemCalculation(ns).dft() ? nwchemFunctionalLabel(ns)
                                                            : ns["method"].toString().toUpper(),
                                ns["basis"].toString()));
    }
    if (!s["restart"].toObject().isEmpty()) {
        const auto restart = nwchemRestart(s);
        const std::optional<DiagnosticFix> release =
            DiagnosticFix{"restart", tr("再開を解除する"), QJsonObject()};
        if (!QStringList{"continue", "hessian", "reuse"}.contains(restart.mode) || restart.job.isEmpty() ||
            restart.job.size() > 4096 || restart.job.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")) ||
            restart.done.size() > 1000)
            result.error("restart.settings", "restart", tr("再開の設定が不正です。再開を解除してください。"),
                         release);
        else {
            const bool reuse = restart.mode == "reuse";
            if (calc.path() || s["bsse"].toBool())
                result.error(
                    "restart.task", "restart",
                    tr("座標スキャン・IRC・NEB・BSSE補正の計算は再開できません。再開を解除してください。"),
                    release);
            else if (restart.mode == "hessian" && !calc.saddle())
                result.error(
                    "restart.task", "task",
                    tr("振動数計算のHessianは遷移状態探索に使います。計算目的を遷移状態探索にするか、"
                       "再開を解除してください。"),
                    DiagnosticFix{"task", tr("計算目的を「遷移状態探索」にする"), "saddle"});
            else if (reuse && calc.task != NwchemTask::Frequencies)
                result.error(
                    "restart.task", "task",
                    tr("Hessianの再利用は「振動数」の計算で行います。計算目的を振動数にするか、再開を"
                       "解除してください。"),
                    DiagnosticFix{"task", tr("計算目的を「振動数」にする"), "frequencies"});
            if (restart.geometry != nwchemRestartDigest(d, !reuse))
                result.error(
                    "restart.geometry", "restart",
                    reuse ? tr("原子の座標・固定または拘束が再開元と異なります。Hessianの再利用で変えら"
                               "れるのは同位体・質量だけです。元に戻すか、再開を解除してください。")
                          : tr("分子（座標・同位体・固定）または拘束が再開元と異なります。再開では変更"
                               "できません。元に戻すか、再開を解除してください。"),
                    release);
            if (advanced["vectors_guess"] == "file")
                result.error("restart.vectors", "advanced:vectors_guess",
                             tr("再開では再開元の分子軌道を使うため、入力movecsは指定できません。"));
            if (errors.isEmpty() && restart.mode == "continue") {
                // The tasks the job finished must still be the first ones of this calculation.
                auto plain = d;
                setNwchemSetting(plain, "restart", QJsonObject());
                QStringList tasks;
                try {
                    tasks = nwchemTaskLines(generateNwchemInput(plain).text);
                } catch (const std::exception &) {
                }
                if (!tasks.isEmpty() && (tasks.size() <= restart.done.size() ||
                                         tasks.mid(0, restart.done.size()) != restart.done))
                    result.error(
                        "restart.tasks", "restart",
                        tr("再開元で終わったタスク（%1）が、今の計算条件のタスク（%2）の先頭と合いませ"
                           "ん。計算目的・物性・出力の設定を再開元に戻すか、再開を解除してください。")
                            .arg(restart.done.join(tr("、")), tasks.join(tr("、"))),
                        release);
            }
            result.warning(
                "restart.notice", "restart",
                restart.mode == "continue"
                    ? tr("「%1」の続きを計算します。構造・拘束・分子軌道は再開元のデータベースの値を使"
                         "い、入力に座標を出力しません。")
                          .arg(QFileInfo(restart.job).fileName())
                : reuse ? tr("「%1」のHessianを再利用して、振動数と熱化学を求め直します（温度・同位体"
                             "の変更用。電子状態の計算は行いません）。")
                              .arg(QFileInfo(restart.job).fileName())
                        : tr("「%1」の振動数計算のHessianと構造から、遷移状態を探索します。")
                              .arg(QFileInfo(restart.job).fileName()));
        }
    }
    errors.removeDuplicates();
    return result;
}
namespace {
void geometryBlocks(NwchemInputBuilder &out, const Document &d, CoordinateUnit unit,
                    const NwchemGeometryOptions &options, bool cartesianOverride) {
    const auto part = nwchemGeometryFragment(d, unit, options);
    NwchemInputBuilder body;
    const auto end = part.geometry.indexOf('\n') + 1;
    body.sourced(part.geometry.left(end), {"driver_cartesian"}, cartesianOverride);
    body.append(part.geometry.mid(end));
    out.block("geometry", body.result, {}, "driver", {1}, tr("geometry（%n 原子）", nullptr, d.atoms.size()));
    out.block("constraints", part.constraints, {}, {}, {1});
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
    out.block("basis", text, p.basis.lines.isEmpty() ? QString("basis") : QString("basis_custom"), {}, {2},
              perElement ? QString() : "basis \"" + basis + '"');
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
QString theoryOf(const QString &method) {
    return method == "hf" ? "scf" : method;
}
void extraInput(NwchemInputBuilder &out, const QString &text, const QString &key) {
    if (text.trimmed().isEmpty())
        return;
    NwchemInputBuilder body;
    body.sourced("\n# Additional input (not checked by MolForma)\n" + text.trimmed() + '\n', {key}, false,
                 true);
    // Free-form input is one opaque region, even if it contains nested blocks or repeated directives.
    out.block("additional", body.result, key, {}, {2}, text.trimmed().section('\n', 0, 0));
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
                   const NwchemValidation &check) {
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
        out.block("task", QString("task %1 energy\n").arg(dft ? "dft" : "scf"), "fragment_guess");
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
    out.block("memory", "memory total " + integer("memory_mb") + " mb\n", "memory_mb", {}, {3});
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
        out.block("bq", text + "end\n", "include_point_charges", {}, {1, 2});
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
    const QString theory = theoryOf(method);
    if (!restart.active())
        fragmentGuess(out, d, s, check);
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
                taskLine("task " + theory + (relaxedScan ? " optimize\n" : " energy\n"));
            }
    } else if (calc.task == NwchemTask::Irc)
        taskLine("task " + theory + " mepgs\n");
    else if (calc.task == NwchemTask::Neb)
        taskLine("task " + theory + " neb\n");
    else {
        if (calc.optimization())
            taskLine("task " + (calc.dft() && s["tddft"].toBool() ? QString("tddft") : theory) +
                     (calc.saddle() ? " saddle\n" : " optimize\n"));
        if (calc.frequencies()) {
            NwchemInputBuilder body;
            body.sourced("task " + theory + " frequencies" +
                             (check.numericalFrequencies && restart.mode != "reuse" ? " numerical" : "") +
                             '\n',
                         {"numerical_frequencies"});
            out.block("task", body.result, "task");
        } else if (calc.dft() && s["tddft"].toBool() && !calc.optimization())
            taskLine(nwchemCubeGradient(d) ? "task tddft gradient\n" : "task tddft energy\n");
        else if (!calc.optimization())
            taskLine("task " + theory + ' ' + str("task") + '\n');
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
        out.block("task", "\ntask " + theoryOf(ns["method"].toString()) + " energy\n", "followup");
    }
    if (anyProperty(s)) {
        const auto level = theoryOf(nwchemSettings(last)["method"].toString());
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
            out.block("task", "task " + level + " property\n", "prop_mulliken");
        }
        if (s["prop_esp"].toBool()) {
            out.block("esp", "\nesp\n  restrain\nend\n", "prop_esp");
            out.block("task", "task esp\n", "prop_esp");
        }
    }
    for (const auto &kind : {QString("esp"), QString("efield")})
        if (s["prop_" + kind + "_grid"].toBool()) {
            out.block("unset", "\nunset prop:*\n", "prop_" + kind + "_grid");
            out.block("property",
                      "property\n  " + kind + "\n  grid pad " +
                          shortest(s["property_grid_padding"].toDouble()) + " ngrid " +
                          integer("property_grid_points") + " output " + str("start") + "_" + kind +
                          "_grid.cube\nend\n",
                      "prop_" + kind + "_grid");
            out.block("task", "task " + theoryOf(nwchemSettings(last)["method"].toString()) + " property\n",
                      "prop_" + kind + "_grid");
        }
    out.append(nwchemOutputLines(last, lastCheck));
    if (s["prop_raman"].toBool()) {
        const double frequency = 45.5633525316 / s["raman_wavelength_nm"].toDouble(); // hc/Eh in nm.
        out.block("unset", "\nunset prop:*\n", "prop_raman");
        out.block("property", "property\n  response 1 " + QString::number(frequency, 'g', 14) + "\nend\n",
                  "raman_wavelength_nm");
        out.block("raman", "raman\n  normal\n  lorentzian\nend\n", "prop_raman");
        out.block("task", "task dft raman\n", "prop_raman");
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
            out.block("task", "task " + theoryOf(str("method")) + " property\n", "prop_ord");
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
        out.block("task", "task dft property\n", "prop_spinspin");
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
QJsonObject nwchemFinalSettings(const Document &d) {
    return nwchemSettings(nwchemSettings(d)["followup"].toBool() ? followupDocument(d) : d);
}
} // namespace mf
