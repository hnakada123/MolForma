#include "nwchem/nwchem.h"
#include "editing/coordinates.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_import.h"
#include "nwchem/nwchem_output.h"
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>

namespace mf {
namespace {
bool frequencyTask(const QString &task) {
    return task == "frequencies" || task == "optimize_frequencies" || task == "saddle_frequencies";
}
bool saddleTask(const QString &task) {
    return task == "saddle" || task == "saddle_frequencies";
}
bool optimizationTask(const QString &task) {
    return task == "optimize" || task == "optimize_frequencies" || saddleTask(task);
}
bool correlated(const QString &method) {
    return method == "mp2" || method.startsWith("ccsd");
}
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
// The library basis of an element: its per-element override, otherwise the common basis.
QString elementBasis(const QJsonObject &settings, const QString &symbol) {
    const auto overrides = settings["basis_overrides"].toObject();
    return overrides.contains(symbol) ? overrides[symbol].toString() : settings["basis"].toString();
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
        lines << zcoordLine(d, idsFromJson(c["atoms"]), c["value"].toDouble(), scale,
                            QString("mfc%1").arg(i + 1));
    }
    return lines;
}
double scanValue(const QJsonObject &s, int step) {
    const int points = std::max(2, s["scan_steps"].toInt());
    const double start = s["scan_start"].toDouble(), end = s["scan_end"].toDouble();
    return start + (end - start) * step / (points - 1);
}
// Geometry of one scan point: the later atoms' side moved to the scanned value, as when the
// dimension is set in the editor.
Document scanGeometry(const Document &d, const QJsonObject &s, int step) {
    Document point = d;
    setDimension(point, idsFromJson(s["scan_atoms"]), scanValue(s, step));
    return point;
}
// Counterpoise monomers: the bonded molecules of the real atoms, in atom order.
QVector<Ids> bsseMonomers(const Document &d) {
    QVector<Ids> monomers;
    QSet<QString> seen;
    for (const auto &a : d.atoms) {
        if (a.ghost || seen.contains(a.id))
            continue;
        auto part = d.component(a.id);
        std::sort(part.begin(), part.end(),
                  [&](const auto &x, const auto &y) { return d.index(x) < d.index(y); });
        for (const auto &id : part)
            seen.insert(id);
        monomers << part;
    }
    return monomers;
}
int monomerCharge(const Document &d, const Ids &ids) {
    int charge = 0;
    for (const auto &id : ids)
        charge += d.atom(id).charge;
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
           s["prop_polarizability"].toBool();
}
// Input that MolForma does not interpret. Shell tasks and Python blocks would run programs.
QString extraInputError(const QString &text) {
    if (text.size() > 20000)
        return tr("20000文字以内で指定してください。");
    if (text.contains(QRegularExpression("[\\x00-\\x08\\x0b-\\x1f\\x7f]")))
        return tr("制御文字を含めないでください。");
    // Statements as NWChem splits them (";" and "\\" continuations), not physical lines.
    for (const auto &statement : nwchemStatements(text)) {
        const auto words = statement.simplified().toLower().split(' ', Qt::SkipEmptyParts);
        if (words.isEmpty())
            continue;
        if (words[0] == "python" ||
            (words[0] == "task" && words.size() > 1 && (words[1] == "shell" || words[1] == "python")))
            return tr("外部プログラムを実行するtask shell・task python・pythonブロックは使用できません。");
    }
    return {};
}
} // namespace
QString nwchemFunctionalXc(const QString &value) {
    const auto *f = functional(value);
    return f ? QString(f->xc) : QString();
}
bool nwchemAutoz(const Document &d) {
    const auto s = nwchemSettings(d);
    return optimizationTask(s["task"].toString()) && s["constraints"].toArray().isEmpty() &&
           !nwchemAdvanced(d)["driver_cartesian"].toBool() &&
           std::none_of(d.atoms.begin(), d.atoms.end(), [](const Atom &a) { return a.fixed || a.ghost; });
}
bool nwchemDriverTask(const QJsonObject &settings) {
    const auto task = settings["task"].toString();
    return optimizationTask(task) || (task == "scan" && settings["scan_relaxed"].toBool());
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
QVector<Ids> nwchemBsseMonomers(const Document &d) {
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
    const auto s = nwchemSettings(d);
    const auto task = s["task"].toString(), method = s["method"].toString();
    if (section == "path")
        return task == "irc" || task == "scan" ? QString() : tr("使わない");
    if (section == "constraints") // Optimizations and relaxed scans write them as zcoord.
        return nwchemDriverTask(s) ? QString() : tr("最適化のみ");
    if (section == "optimization")
        return nwchemDriverTask(s) || frequencyTask(task) || task == "irc" ? QString() : tr("使わない");
    if (section == "followup")
        return optimizationTask(task) ? QString() : tr("最適化のみ");
    if (section == "bsse")
        return bsseMonomers(d).size() >= 2 ? QString() : tr("単量体なし");
    if (section == "excited")
        return method != "dft" ? tr("DFTのみ") : task != "energy" ? tr("一点計算のみ") : QString();
    return {};
}
QJsonObject nwchemDefaults() {
    return {{"start", "molforma"},
            {"title", "MolForma calculation"},
            {"memory_mb", 1024},
            {"method", "hf"},
            {"task", "energy"},
            {"basis", "sto-3g"},
            {"reference", "auto"},
            {"functional", "b3lyp"},
            {"xc_custom", ""},
            {"dispersion", "none"},
            {"basis_functions", "auto"},
            {"basis_overrides", QJsonObject()},
            {"geometry_units", "angstrom"},
            {"scf_maxiter", 100},
            {"scf_convergence", "tight"},
            {"grid", "fine"},
            {"relativistic", "none"},
            {"ri_j", false},
            {"opt_maxiter", 100},
            {"opt_convergence", "default"},
            {"numerical_frequencies", false},
            {"freeze_core", false},
            {"cc_maxiter", 50},
            {"solvent", "none"},
            {"solvent_name", "dielectric"},
            {"dielectric", 78.4},
            {"include_point_charges", false},
            {"tddft", false},
            {"tddft_roots", 10},
            {"tddft_triplets", false},
            {"tddft_tda", false},
            {"constraints", QJsonArray()},
            {"scan_atoms", QJsonArray()},
            {"scan_start", 1.0},
            {"scan_end", 2.0},
            {"scan_steps", 6},
            {"scan_relaxed", true},
            {"irc_points", 30},
            {"irc_stride", 0.1},
            {"irc_direction", "both"},
            {"bsse", false},
            {"followup", false},
            {"followup_method", "dft"},
            {"followup_functional", "b3lyp"},
            {"followup_dispersion", "none"},
            {"followup_basis", "def2-tzvp"},
            {"prop_mulliken", false},
            {"prop_esp", false},
            {"prop_nmr", false},
            {"prop_polarizability", false},
            {"extra_input", ""},
            {"extra_tasks", ""}};
}
QJsonObject nwchemSettings(const Document &d) {
    auto settings = nwchemDefaults();
    const auto stored = withBasisFunctions(d.data["nwchem"].toObject());
    for (auto it = stored.begin(); it != stored.end(); ++it)
        settings[it.key()] = it.value();
    return settings;
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
                {"scan", QT_TR_NOOP("座標スキャン")}};
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
    return {};
}
} // namespace
QVector<NwchemChoice> nwchemChoices(const QString &key) {
    QVector<NwchemChoice> result;
    for (const auto &c : choiceSources(key))
        result << NwchemChoice{c.value, tr(c.label)};
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
    const auto defaults = nwchemDefaults();
    // Atom references and free input belong to one project, not to a reusable method.
    const QStringList excluded{"start",      "title",      "memory_mb", "geometry_units", "constraints",
                               "scan_atoms", "scan_start", "scan_end",  "extra_input",    "extra_tasks"};
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        if (!excluded.contains(it.key()))
            preset[it.key()] = settings.contains(it.key()) ? settings[it.key()] : it.value();
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
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        label = tr("%1、%2: %3").arg(label, it.key(), name(it.value().toString()));
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
    const auto *f = functional(s["functional"].toString());
    // NWChem's analytic Hessian covers neither solvation, relativistic Hamiltonians nor CD fitting.
    if (s["numerical_frequencies"].toBool() || s["method"] == "mp2" || s["reference"] == "rohf" ||
        (s["method"] == "dft" && f && f->numericalHessian) ||
        (s["reference"] == "auto" && s["method"] == "hf" && d.job()["multiplicity"].toInt(1) > 1) ||
        s["solvent"] != "none" || s["relativistic"] != "none" || (s["method"] == "dft" && s["ri_j"].toBool()))
        return true;
    for (const auto &atom : d.atoms)
        if (!atom.ghost) {
            try {
                if (ecpCore(elementBasis(s, atom.element), element(atom.element).number))
                    return true;
            } catch (const std::exception &) {
                // Validation reports unknown atoms separately.
            }
        }
    return false;
}
NwchemValidation validateNwchem(const Document &d) {
    NwchemValidation result;
    result.errors = d.validate();
    auto &errors = result.errors;
    auto &warnings = result.warnings;
    const auto s = nwchemSettings(d);
    const auto advanced = nwchemAdvanced(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    const auto defaults = nwchemDefaults();
    if (d.data.contains("nwchem") && !d.data["nwchem"].isObject())
        errors << tr("NWChem設定はオブジェクトである必要があります。");
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto value = s[it.key()];
        if (value.type() != it.value().type())
            errors << tr("設定 %1 の型が不正です。").arg(it.key());
        const auto choices = choiceSources(it.key());
        if (!choices.isEmpty() && std::none_of(choices.begin(), choices.end(),
                                               [&](const auto &c) { return value.toString() == c.value; }))
            errors << tr("設定 %1 の値「%2」は未対応です。").arg(it.key(), value.toString());
    }
    auto integer = [&](const QJsonObject &object, const char *key, int minimum, int maximum) {
        const auto value = object[key];
        double n = value.toDouble();
        if (!value.isDouble() || !std::isfinite(n) || n < minimum || n > maximum || std::floor(n) != n)
            errors << tr("%1 は %2〜%3 の整数で指定してください。").arg(key).arg(minimum).arg(maximum);
    };
    integer(s, "memory_mb", 64, 1048576);
    integer(s, "scf_maxiter", 1, 10000);
    integer(s, "opt_maxiter", 1, 10000);
    integer(s, "cc_maxiter", 1, 10000);
    integer(s, "tddft_roots", 1, 100);
    integer(d.job(), "charge", -1000, 1000);
    integer(d.job(), "multiplicity", 1, 1000);
    if (!QRegularExpression("^[A-Za-z][A-Za-z0-9_-]{0,63}$").match(str("start")).hasMatch())
        errors << tr("計算名は英字で始まる英数字・_・- の64文字以内で指定してください。");
    if (str("title").size() > 120 || str("title").contains(QRegularExpression("[\\x00-\\x1f\\x7f\"\\\\;#]")))
        errors << tr("タイトルは120文字以内で、改行・引用符・\\・;・#を含めず指定してください。");
    double dielectric = s["dielectric"].toDouble();
    if (!std::isfinite(dielectric) || dielectric < 1 || dielectric > 1000)
        errors << tr("誘電率は1〜1000で指定してください。");
    const auto method = str("method"), task = str("task"), basis = str("basis");
    const int mult = d.job()["multiplicity"].toInt(1);
    const int charge = d.job()["charge"].toInt();
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
            errors << tr("元素別の基底「%1: %2」は未対応です。").arg(it.key(), it.value().toString());
    }
    bool nonRelativisticBasis = false;
    for (const auto &a : d.atoms) {
        int z;
        try {
            z = element(a.element).number;
        } catch (const std::exception &) {
            continue; // Document validation already reports unknown elements.
        }
        const auto own = elementBasis(s, a.element);
        if (!basisCovers(own, z))
            unsupported.insert(own == basis ? a.element : tr("%1（%2）").arg(a.element, own));
        if (!fittingCovers(z))
            unfitted.insert(a.element);
        if (const auto *b = libraryBasis(own); b && !b->relativistic && z > 36)
            nonRelativisticBasis = true;
        fixed += a.fixed;
        ghosts += a.ghost;
        if (!a.ghost) {
            ++realAtoms;
            movable += !a.fixed;
            const int core = ecpCore(own, z);
            if (core)
                ecpLibraries.insert(libraryBasis(own)->ecp);
            result.ecpElectrons += core;
            result.electrons += z - core;
            if (!core && correlated(method) && s["freeze_core"].toBool())
                result.frozenOrbitals += atomicCoreOrbitals(z);
        }
    }
    if (!unsupported.isEmpty())
        errors << tr("基底 %1 の対応元素に含まれません: %2").arg(basis, sorted(unsupported).join(", "));
    if (!realAtoms)
        errors << tr("実原子を追加してください（ゴーストのみの計算はできません）。");
    result.electrons -= charge;
    if (correlated(method) && advanced.contains("corr_freeze"))
        result.frozenOrbitals = advanced["corr_freeze"].toInt();
    if (result.electrons < 1 || mult > result.electrons + 1 || (result.electrons - mult + 1) % 2 != 0)
        errors << tr("電子数 %1 と多重度 %2 が整合しません。全電荷または多重度を変更してください。")
                      .arg(result.electrons)
                      .arg(mult);
    else {
        result.alpha = (result.electrons + mult - 1) / 2;
        result.beta = (result.electrons - mult + 1) / 2;
    }
    result.reference = str("reference");
    if (result.reference == "auto")
        result.reference = mult == 1 ? "rhf" : (method == "hf" ? "rohf" : "uhf");
    if (result.reference == "rhf" && mult != 1)
        errors << tr("RHF / RKSは多重度1で使用してください。開殻には自動または非制限を選択してください。");
    if ((method == "dft" || method == "mp2") && result.reference == "rohf")
        errors << tr("このタブのDFT・MP2はROHF参照に対応しません。自動または非制限を選択してください。");
    if (method.startsWith("ccsd") && (result.reference != "rhf" || mult != 1 || task != "energy"))
        errors << tr("CCSD / CCSD(T)は閉殻RHFの一点エネルギーに対応します。");
    if (method == "dft") {
        const auto *f = functional(str("functional"));
        const auto custom = str("xc_custom").simplified();
        if (f && QString(f->value) == "custom") {
            // One XC line inside the DFT block: names, numbers and signs only.
            if (custom.isEmpty() || custom.size() > 200 ||
                str("xc_custom").contains(QRegularExpression("[\\x00-\\x1f\\x7f]")) ||
                !QRegularExpression("^[A-Za-z0-9_.+\\- ]+$").match(custom).hasMatch())
                errors << tr("任意のxcは英数字・_・.・+・- と空白の200文字以内で指定してください（例: "
                             "xpbe96 0.75 cpbe96 1.0 hfexch 0.25）。");
            if (str("dispersion") != "none")
                errors << tr(
                    "任意のxcでは分散補正を選べません。NWChemにパラメータがない場合があるためです。");
        } else if (f && ((str("dispersion") == "d3" && !f->d3) || (str("dispersion") == "d3bj" && !f->d3bj)))
            errors << tr("%1 の%2はNWChemの公表パラメータと一致しないか、パラメータがありません。%3")
                          .arg(tr(f->label), str("dispersion") == "d3" ? "D3" : "D3(BJ)",
                               QString(f->value) == "wb97x-d3"
                                   ? tr("ωB97X-D3はD3を含むため、分散補正は「なし」にしてください。")
                               : f->d3   ? tr("D3または補正なしを選んでください。")
                               : f->d3bj ? tr("D3(BJ)または補正なしを選んでください。")
                                         : tr("補正なしを選んでください。"));
    }
    if (s["tddft"].toBool()) {
        const auto *f = functional(str("functional"));
        if (method != "dft")
            errors << tr("TDDFTはDFTで使用してください。");
        else if (f && f->metaGga)
            errors << tr("NWChemのTDDFTはメタGGA（%1）に対応しません。B3LYP・PBE0・CAM-"
                         "B3LYP等を選んでください。")
                          .arg(tr(f->label));
        if (task != "energy")
            errors << tr(
                "TDDFTは計算目的「一点エネルギー」で使用してください（励起状態の構造最適化は未対応）。");
    }
    if (correlated(method) && result.electrons < 2)
        errors << tr("相関計算には2個以上の電子が必要です。");
    if (result.frozenOrbitals &&
        (result.frozenOrbitals > result.beta || result.electrons - 2 * result.frozenOrbitals < 2))
        errors << tr("内殻凍結後の相関電子が不足します。内殻凍結を解除してください。");
    if (correlated(method) && (s["freeze_core"].toBool() || advanced.contains("corr_freeze")))
        warnings << tr("内殻凍結: %n 軌道。%1", nullptr, result.frozenOrbitals)
                        .arg(advanced.contains("corr_freeze")
                                 ? tr("詳細設定の明示的な軌道数を使用します。")
                                 : tr("ECP中心とゴーストはfreeze atomicの対象外です。"));
    const auto solvent = str("solvent"), solventName = str("solvent_name");
    if (solvent != "none") {
        const QString model = solvent == "smd" ? "SMD" : "COSMO";
        if (method != "hf" && method != "dft")
            errors << tr("%1はHFまたはDFTで使用してください。").arg(model);
        if (solvent == "smd" && method != "dft")
            errors << tr("NWChemのSMDはDFT専用です。HFにはCOSMOを使用してください。");
        if (solvent == "smd" && solventName == "dielectric")
            errors << tr("SMDでは溶媒名を選んでください（誘電率だけでは非静電項を決められません）。");
        if (ghosts)
            errors << tr("ゴーストを含む%1入力は未対応です。").arg(model);
        if (method == "hf" && result.reference == "uhf")
            errors << tr(
                "NWChemのCOSMOはUHFに対応しません。参照を自動（ROHF）にするか、DFTを選択してください。");
    }
    const auto relativistic = str("relativistic");
    if (relativistic != "none") {
        if (method != "hf" && method != "dft")
            errors << tr("相対論ハミルトニアンはHFまたはDFTで使用してください。");
        if (relativistic == "zora" && method != "dft")
            errors << tr("NWChemのZORAはDFT専用です。HFにはDKHまたはX2Cを選んでください。");
        if (result.ecpElectrons)
            errors << tr(
                "相対論ハミルトニアンはECPと併用できません。x2c-SVPall等の全電子基底を選んでください。");
        else if (nonRelativisticBasis)
            warnings << tr(
                "重元素に非相対論の基底を使用しています。x2c-SVPall・SARC等の相対論用の基底を推奨します。");
    }
    if (s["ri_j"].toBool()) {
        if (method != "dft")
            errors << tr("RI-J（Coulombフィッティング）はDFTで使用してください。");
        if (!unfitted.isEmpty())
            errors << tr("Weigendのフィッティング基底の対応元素に含まれません: %1")
                          .arg(sorted(unfitted).join(", "));
        warnings << tr("RI-J（weigend coulomb fitting）でCoulomb項を近似します。全エネルギーは通常の計算と"
                       "わずかに異なります。");
    }
    // Internal-coordinate constraints (ZCOORD), scans and reaction paths.
    const auto constraints = s["constraints"].toArray();
    if (constraints.size() > 50)
        errors << tr("内部座標の拘束は50個までです。");
    for (int i = 0; i < constraints.size(); ++i) {
        const auto c = constraints[i].toObject();
        const auto ids = idsFromJson(c["atoms"]);
        const auto error = c["value"].isDouble() ? coordinateError(d, ids, c["value"].toDouble())
                                                 : tr("値を数値で指定してください。");
        if (!error.isEmpty())
            errors << tr("拘束%1: %2").arg(QString::number(i + 1), error);
    }
    if (!constraints.isEmpty()) {
        if (!optimizationTask(task) && !(task == "scan" && s["scan_relaxed"].toBool()))
            errors << tr("内部座標の拘束は構造最適化・遷移状態探索・緩和スキャンで使用してください。");
        if (fixed || ghosts)
            errors << tr(
                "内部座標の拘束は固定原子・ゴーストと併用できません（NWChemはCartesianの固定と内部座標"
                "を同時に扱えません）。固定を解除してください。");
        warnings << tr("拘束付き最適化は内部座標（autoz）で行います。NWChemが内部座標を生成できない構造では"
                       "拘束を適用できません。");
        if (frequencyTask(task))
            warnings << tr(
                "拘束した構造は停留点ではないため、振動数には虚振動や大きな誤差が出ることがあります。");
    }
    const bool scan = task == "scan";
    if (scan) {
        const auto ids = idsFromJson(s["scan_atoms"]);
        integer(s, "scan_steps", 2, 50);
        if (ids.isEmpty())
            errors << tr("スキャンする座標を「1 分子」で2〜4原子選択して指定してください。");
        else
            for (const char *key : {"scan_start", "scan_end"}) {
                const auto error = coordinateError(d, ids, s[key].toDouble());
                if (!error.isEmpty())
                    errors << (key == QString("scan_start") ? tr("スキャン開始値: %1")
                                                            : tr("スキャン終了値: %1"))
                                  .arg(error);
            }
        if (errors.isEmpty()) {
            try {
                for (int step = 0; step < s["scan_steps"].toInt(); ++step)
                    scanGeometry(d, s, step);
            } catch (const std::exception &e) {
                errors << tr("スキャン: %1").arg(QString::fromUtf8(e.what()));
            }
        }
        if (s["scan_relaxed"].toBool() && (fixed || ghosts))
            errors << tr(
                "緩和スキャンは固定原子・ゴーストと併用できません。固定を解除するか、固定スキャンにして"
                "ください。");
        if (s["scan_relaxed"].toBool())
            warnings << tr("緩和スキャンでは各点でスキャン座標を固定して最適化します（内部座標・autoz）。");
        else
            warnings << tr(
                "固定スキャンでは、後に選択した原子の側を動かした構造で一点エネルギーを計算します。");
    }
    if (task == "irc") {
        if (fixed || ghosts)
            errors << tr("IRCは固定原子・ゴーストを含む構造に対応しません。");
        if (realAtoms < 3)
            errors << tr("IRCには3個以上の実原子が必要です。");
        warnings << (str("irc_direction") == "both"
                         ? tr("IRCは遷移状態の構造から始めてください（遷移状態探索と振動数で虚振動が1つである"
                              "ことを確認）。"
                              "初めに遷移状態のHessianを計算し、両方向へ経路を追跡します。")
                         : tr("IRCは遷移状態の構造から始めてください（遷移状態探索と振動数で虚振動が1つである"
                              "ことを確認）。"
                              "初めに遷移状態のHessianを計算し、指定した方向だけ経路を追跡します。"));
    }
    integer(s, "irc_points", 1, 500);
    const double stride = s["irc_stride"].toDouble();
    if (!std::isfinite(stride) || stride < 0.01 || stride > 1)
        errors << tr("IRCの刻み幅は0.01〜1で指定してください。");
    if (s["tddft"].toBool() && (scan || task == "irc"))
        errors << tr("TDDFTはスキャン・IRCと併用できません。");
    // Counterpoise correction of the bonded molecules as monomers.
    if (s["bsse"].toBool()) {
        const auto monomers = bsseMonomers(d);
        if (!QStringList{"energy", "gradient", "optimize"}.contains(task))
            errors << tr("BSSE補正は一点エネルギー・勾配・構造最適化で使用してください。");
        if (monomers.size() < 2)
            errors << tr("BSSE補正には結合していない2個以上の分子（単量体）が必要です。");
        if (monomers.size() > 20)
            errors << tr("BSSE補正の単量体は20個までです。");
        if (ghosts)
            errors << tr("BSSE補正は単量体ごとのゴーストを自動で作るため、ゴースト中心と併用できません。");
        if (solvent != "none" || s["tddft"].toBool() || anyProperty(s) || s["followup"].toBool())
            errors << tr("BSSE補正は溶媒・TDDFT・物性計算・後続の一点計算と併用できません。");
        if (!d.data["point_charges"].toArray().isEmpty() && s["include_point_charges"].toBool())
            errors << tr("BSSE補正は外部点電荷と併用できません。");
        if (advanced["cube_output"].toBool() || advanced["molden_output"].toBool())
            errors << tr("BSSE補正の計算ではCUBE・MOLDENを出力できません。");
        int total = 0;
        bool openShell = false;
        for (const auto &monomer : monomers) {
            const int q = monomerCharge(d, monomer);
            total += q;
            int electrons = -q;
            for (const auto &id : monomer)
                try {
                    const int z = element(d.atom(id).element).number;
                    electrons += z - ecpCore(elementBasis(s, d.atom(id).element), z);
                } catch (const std::exception &) {
                    // Document validation reports unknown elements.
                }
            openShell |= electrons % 2 != 0;
        }
        if (monomers.size() >= 2 && total != charge)
            errors << tr("BSSE補正の単量体の電荷は原子の形式電荷から決めます。全電荷 %1 を形式電荷の合計 %2 "
                         "に合わせてください。")
                          .arg(charge)
                          .arg(total);
        if (openShell || mult != 1)
            errors << tr("BSSE補正は閉殻の単量体からなる一重項に対応します。");
        if (monomers.size() >= 2)
            warnings << tr("BSSE補正（counterpoise）: 単量体 %n 個。単量体ごとに相手の基底をゴーストとして"
                           "計算します。",
                           nullptr, int(monomers.size()));
    }
    // Molecular properties after the last calculation (the follow-up one if present).
    if (anyProperty(s)) {
        const auto level = s["followup"].toBool() ? s["followup_method"].toString() : method;
        if (level != "hf" && level != "dft")
            errors << tr("物性計算は最後の計算レベルがHFまたはDFTのときに使用してください。");
        if (scan || task == "irc")
            errors << tr("物性計算はスキャン・IRCと併用できません。");
        if (ghosts && s["prop_esp"].toBool())
            errors << tr("ESP電荷はゴースト中心を含む構造に対応しません。");
        if ((s["prop_nmr"].toBool() || s["prop_polarizability"].toBool()) && result.reference == "rohf")
            errors << tr("NMR・分極率はROHFに対応しません。自動（UHF）またはDFTを選んでください。");
        if (s["prop_nmr"].toBool())
            warnings << tr(
                "NMR遮蔽定数（GIAO）を計算します。化学シフトには同じ条件の基準物質（TMS等）の値との差を"
                "使ってください。");
    }
    if (s["followup"].toBool()) {
        if (!optimizationTask(task))
            errors << tr("後続の一点計算は構造最適化・遷移状態探索の後に使用してください。");
        if (s["tddft"].toBool())
            errors << tr("後続の一点計算とTDDFTは併用できません。");
    }
    const QString extraInput = str("extra_input"), extraTasks = str("extra_tasks");
    for (const auto &[label, text] :
         {std::pair{tr("追加入力（タスク前）: %1"), extraInput}, {tr("追加入力（末尾）: %1"), extraTasks}})
        if (const auto error = extraInputError(text); !error.isEmpty())
            errors << label.arg(error);
    if (!extraInput.trimmed().isEmpty() || !extraTasks.trimmed().isEmpty())
        warnings << tr(
            "追加入力はMolFormaが検査せずにそのまま出力します。入力プレビューで内容を確認してください。");
    if (frequencyTask(task) && (ghosts || fixed))
        errors << tr(
            "固定原子・ゴーストを含む振動数解析は未対応です。固定を解除し、実原子のみで設定してください。");
    if (frequencyTask(task) && realAtoms < 2)
        errors << tr("振動数解析には2個以上の実原子が必要です。");
    if (optimizationTask(task) && realAtoms && !movable)
        errors << tr("全原子が固定されています。最適化する原子の固定を解除してください。");
    if (optimizationTask(task) && ghosts)
        warnings << tr("ゴースト中心は核を持たないため、最適化中は固定します（constraints の fix atom）。");
    const bool points = !d.data["point_charges"].toArray().isEmpty();
    if (frequencyTask(task) && points && s["include_point_charges"].toBool())
        errors << tr("外部点電荷中の振動数解析は未対応です。");
    result.numericalFrequencies = nwchemNumericalFrequencies(d);
    if (frequencyTask(task) && result.numericalFrequencies)
        warnings << tr("振動数には数値Hessianを使用します。複数回の勾配計算を行います。");
    if (frequencyTask(task) && s["scf_convergence"] == "default")
        warnings << tr("SCF収束がNWChem既定のため、振動数（特に数値Hessian）の精度が不足することがあります。"
                       "「厳密」を推奨します。");
    if (task == "frequencies")
        warnings << tr("現在の座標で振動数を計算します。極小構造の確認には先に最適化してください。");
    if (saddleTask(task))
        warnings << tr("遷移状態探索は、反応座標に沿って1方向だけ負の曲率を持つ初期構造から始めてください。"
                       "振動数で虚振動が1つであることを確認してください。");
    if (result.ecpElectrons)
        warnings << tr("ECP（%1）を自動指定します（除去する内殻電子: %2）。")
                        .arg(sorted(ecpLibraries).join(tr("・")), QString::number(result.ecpElectrons));
    QSet<bool> conventions{cartesianByConvention(basis)};
    for (const auto &a : d.atoms)
        conventions.insert(cartesianByConvention(elementBasis(s, a.element)));
    if (conventions.size() > 1)
        warnings << tr("元素別の基底に、共通の基底とd関数の慣例（Cartesian / 球面）が異なるものがあります。"
                       "basis指示は全元素に共通のため、d・f関数の形式を確認してください。");
    const auto functions = str("basis_functions");
    if (functions != "auto" && basis != "sto-3g" &&
        (functions == "cartesian") != cartesianByConvention(basis))
        warnings
            << (cartesianByConvention(basis)
                    ? tr("基底 %1 は通常Cartesian（6d）で使用します。文献値と比べる場合はd・f関数の形式を"
                         "「自動」にしてください。")
                    : tr("基底 %1 は通常球面調和関数（5d）で使用します。文献値と比べる場合はd・f関数の形式を"
                         "「自動」にしてください。"))
                   .arg(basis);
    if (s["tddft"].toBool() && method == "dft" && result.reference == "uhf" && s["tddft_triplets"].toBool())
        warnings << tr("開殻（UKS）のTDDFTは一重項・三重項を分けずに非制限の励起状態を求めます。");
    if (frequencyTask(task))
        if (const int sigma = rotationalSymmetryNumber(d); sigma > 1)
            warnings << tr("回転対称数σ=%1と推定されます。noautosymのためNWChemの熱化学はσ=1で計算されます。"
                           "エントロピーを R ln σ = %2 cal/(mol·K) 小さく、自由エネルギーを RT ln σ = %3 "
                           "kcal/mol（298.15 K）大きく補正してください。")
                            .arg(sigma)
                            .arg(1.987204 * std::log(sigma), 0, 'f', 3)
                            .arg(0.0019872043 * 298.15 * std::log(sigma), 0, 'f', 3);
    if (d.hasCenters() && charge != d.expectedCharge())
        warnings
            << tr("全電荷 %1 は配位中心の酸化数（補助）と配位子の形式電荷から見込まれる %2 と異なります。")
                   .arg(charge)
                   .arg(d.expectedCharge());
    else if (!d.hasCenters() && charge != d.formalCharge())
        warnings
            << tr("全電荷 %1 は原子の形式電荷の合計 %2 と異なります。").arg(charge).arg(d.formalCharge());
    if (points && !s["include_point_charges"].toBool())
        warnings << tr("保存された点電荷は入力に含めません。必要なら外部点電荷を有効にしてください。");
    if (!d.data["references"].toArray().isEmpty() || !d.data["invalid_settings"].toArray().isEmpty() ||
        !d.data["assignments"].toObject().isEmpty() || !d.job()["sections"].toObject().isEmpty())
        warnings << tr("旧形式の詳細・拘束・断片設定は変換しません。NWChem計算条件と原子の固定フラグを使用し"
                       "ます。");
    try {
        if (!d.atoms.isEmpty())
            nwchemGeometry(d,
                           CoordinateUnit::Angstrom); // Also validate isotope masses before enabling export.
    } catch (const std::exception &e) {
        errors << QString::fromUtf8(e.what());
    }
    validateNwchemAdvanced(d, result);
    if (s["followup"].toBool() && errors.isEmpty()) {
        const auto next = followupDocument(d);
        const auto check = validateNwchem(next);
        for (const auto &error : check.errors)
            if (!errors.contains(error))
                errors << tr("後続の一点計算: %1").arg(error);
        const auto ns = nwchemSettings(next);
        QSet<QString> mainEcp, nextEcp;
        for (const auto &a : d.atoms)
            if (!a.ghost) {
                if (ecpCore(elementBasis(s, a.element), element(a.element).number))
                    mainEcp.insert(a.element);
                if (ecpCore(elementBasis(ns, a.element), element(a.element).number))
                    nextEcp.insert(a.element);
            }
        if (!mainEcp.isEmpty() && nextEcp.isEmpty())
            errors << tr(
                "主計算のECPはNWChemの実行中に残るため、後続の一点計算にもECPを使う基底（def2系・LANL2DZ）"
                "を選んでください。");
        warnings << tr("最適化した構造で %1 / %2 の一点エネルギーを計算します。")
                        .arg(ns["method"] == "dft" ? nwchemFunctionalLabel(ns)
                                                   : ns["method"].toString().toUpper(),
                             ns["basis"].toString());
    }
    errors.removeDuplicates();
    return result;
}
namespace {
// BASIS (and RI-J fitting) blocks for the real and ghost centers, and the ECP block. With
// allGhosts every element also gets a "bq" entry, for the ghosts NWChem's BSSE module creates.
QString basisBlocks(const Document &d, const QJsonObject &s, bool allGhosts) {
    const auto basis = s["basis"].toString();
    const bool spherical = s["basis_functions"] == "spherical" ||
                           (s["basis_functions"] == "auto" && !cartesianByConvention(basis));
    QSet<QString> real, ghosts, ecps;
    for (const auto &a : d.atoms) {
        (a.ghost ? ghosts : real).insert(a.element);
        if (!a.ghost && ecpCore(elementBasis(s, a.element), element(a.element).number))
            ecps.insert(a.element);
    }
    if (allGhosts)
        ghosts.unite(real);
    const auto type = spherical ? QString(" spherical\n") : QString(" cartesian\n");
    QString text = "\nbasis" + type;
    // With per-element bases every element gets its own line: NWChem would add, not replace,
    // the shells of a second library entry for an element already covered by "*".
    const bool perElement = std::any_of(real.begin(), real.end(),
                                        [&](const auto &symbol) { return elementBasis(s, symbol) != basis; });
    if (perElement)
        for (const auto &symbol : sorted(real))
            text += QString("  %1 library \"%2\"\n").arg(symbol, elementBasis(s, symbol));
    else
        text += "  * library \"" + basis + "\"\n";
    for (const auto &symbol : sorted(ghosts))
        text += QString("  bq%1 library %1 \"%2\"\n").arg(symbol, elementBasis(s, symbol));
    text += "end\n";
    if (s["method"] == "dft" && s["ri_j"].toBool()) {
        text += "\nbasis \"cd basis\"" + type + "  * library \"weigend coulomb fitting\"\n";
        for (const auto &symbol : sorted(ghosts))
            text += QString("  bq%1 library %1 \"weigend coulomb fitting\"\n").arg(symbol);
        text += "end\n";
    }
    if (!ecps.isEmpty()) {
        text += "\necp\n";
        for (const auto &symbol : sorted(ecps))
            text += QString("  %1 library \"%2\"\n").arg(symbol, libraryBasis(elementBasis(s, symbol))->ecp);
        text += "end\n";
    }
    return text;
}
// The SCF/DFT block and, for correlated methods, the MP2/CCSD block.
QString methodBlocks(const Document &d, const QJsonObject &s, const NwchemValidation &check) {
    const auto advanced = nwchemAdvanced(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    auto integer = [&](const char *key) { return QString::number(s[key].toInt()); };
    const auto method = str("method");
    // "default" leaves NWChem's own thresholds, apart from those set in the details.
    const bool own = str("scf_convergence") == "default";
    const QString threshold = nwchemScfThreshold(s);
    QString text;
    if (method == "dft") {
        const auto *f = functional(str("functional"));
        text += "\ndft\n  xc " +
                (QString(f->value) == "custom" ? str("xc_custom").simplified() : QString(f->xc)) + '\n';
        if (*f->extra)
            text += QString("  ") + f->extra + '\n';
        text += "  mult " + QString::number(d.job()["multiplicity"].toInt()) + '\n';
        if (check.reference == "uhf")
            text += "  odft\n";
        QString convergence;
        for (const auto &[key, keyword] :
             {std::pair{"dft_energy", "energy"}, std::pair{"dft_density", "density"},
              std::pair{"dft_gradient", "gradient"}})
            if (!own || advanced.contains(key))
                convergence += QString(" ") + keyword + ' ' + nwchemAdvancedNumber(d, key, threshold);
        text += "  iterations " + integer("scf_maxiter") + '\n';
        if (!convergence.isEmpty())
            text += "  convergence" + convergence + '\n';
        text += "  grid " + str("grid") + (advanced["dft_grid_nodisk"].toBool() ? " nodisk" : "") + '\n';
        text += nwchemAdvancedLines(d, "dft") + nwchemAdvancedLines(d, "vectors");
        if (str("dispersion") != "none")
            text += str("dispersion") == "d3" ? "  disp vdw 3\n" : "  disp vdw 4\n";
        text += "end\n";
    } else {
        text += "\nscf\n  " + check.reference + "\n  nopen " +
                QString::number(d.job()["multiplicity"].toInt() - 1) + "\n  maxiter " +
                integer("scf_maxiter") + '\n';
        if (!own || advanced.contains("scf_thresh"))
            text += "  thresh " + nwchemAdvancedNumber(d, "scf_thresh", threshold) + '\n';
        text += nwchemAdvancedLines(d, "scf") + nwchemAdvancedLines(d, "vectors") + "end\n";
        if (correlated(method)) {
            text += method == "mp2" ? "\nmp2\n" : "\nccsd\n";
            text += "  freeze " +
                    nwchemAdvancedNumber(d, "corr_freeze", s["freeze_core"].toBool() ? "atomic" : "0") + '\n';
            if (method != "mp2")
                text += "  maxiter " + integer("cc_maxiter") + "\n  thresh " +
                        nwchemAdvancedNumber(d, "cc_thresh", "1e-7") + '\n';
            text += nwchemAdvancedLines(d, "correlation");
            text += "end\n";
        }
    }
    return text;
}
QString theoryOf(const QString &method) {
    return method == "hf" ? "scf" : method;
}
} // namespace
QString nwchemInput(const Document &d) {
    const auto check = validateNwchem(d);
    if (!check.errors.isEmpty())
        fail(check.errors.join('\n'));
    const auto s = nwchemSettings(d);
    const auto advanced = nwchemAdvanced(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    auto integer = [&](const char *key) { return QString::number(s[key].toInt()); };
    const auto method = str("method"), task = str("task");
    const auto unit = str("geometry_units") == "bohr" ? CoordinateUnit::Bohr : CoordinateUnit::Angstrom;
    const double scale = coordinateScale(unit);
    const bool scan = task == "scan", relaxedScan = scan && s["scan_relaxed"].toBool();
    QString text = "# Generated by MolForma for NWChem\nstart " + str("start") + '\n';
    if (!str("title").isEmpty())
        text += "title \"" + str("title") + "\"\n";
    text += "memory total " + integer("memory_mb") + " mb\n";
    text += nwchemAdvancedLines(d, "files");
    text += "charge " + QString::number(d.job()["charge"].toInt()) + "\n\n";
    if (!scan) {
        Document geometry = d;
        if (optimizationTask(task))
            for (auto &a : geometry.atoms)
                a.fixed |= a.ghost; // Ghost centers carry no nucleus; the driver would pull them around.
        NwchemGeometryOptions options;
        if (optimizationTask(task))
            options.zcoord = constraintLines(d, s, scale);
        options.autoz = nwchemAutoz(d);
        text += nwchemGeometry(geometry, unit, false, options);
    }
    text += basisBlocks(d, s, s["bsse"].toBool());
    if (s["include_point_charges"].toBool() && !d.data["point_charges"].toArray().isEmpty()) {
        // Point-charge coordinates always remain in angstrom, independently of GEOMETRY units.
        text += "\nbq units angstrom\n";
        for (const auto &value : d.data["point_charges"].toArray()) {
            const auto p = value.toObject();
            const auto xyz = vectorFromJson(p["position"]);
            text += QString("  %1 %2 %3 %4\n")
                        .arg(shortest(xyz.x()), shortest(xyz.y()), shortest(xyz.z()),
                             shortest(p["charge"].toDouble()));
        }
        text += "end\n";
    }
    if (const auto relativistic = str("relativistic"); relativistic != "none")
        text += "\nrelativistic\n" +
                QString(relativistic == "zora"  ? "  zora on\n"
                        : relativistic == "dkh" ? "  douglas-kroll dkh\n"
                                                : "  x2c on\n") +
                "end\n";
    text += methodBlocks(d, s, check);
    if (str("solvent") != "none") {
        text += "\ncosmo\n";
        if (str("solvent") == "smd")
            text += "  do_cosmo_smd true\n";
        if (str("solvent_name") == "dielectric")
            text += "  dielec " + shortest(s["dielectric"].toDouble()) + '\n';
        else
            text += "  solvent " + str("solvent_name") + '\n';
        text += nwchemAdvancedLines(d, "cosmo") + "end\n";
    }
    if (method == "dft" && s["tddft"].toBool()) {
        // Vertical excitations from the ground state: closed shells give singlets (and triplets on
        // request), open shells unrestricted states. "cis" selects the Tamm-Dancoff approximation.
        text += "\ntddft\n  nroots " + integer("tddft_roots") + '\n';
        if (check.reference == "rhf" && !s["tddft_triplets"].toBool())
            text += "  notriplet\n";
        if (s["tddft_tda"].toBool())
            text += "  cis\n";
        text += "end\n";
    }
    if (nwchemDriverTask(s))
        text += "\ndriver\n  maxiter " + integer("opt_maxiter") + "\n  " + str("opt_convergence") + '\n' +
                nwchemAdvancedLines(d, "driver") + "end\n";
    if (frequencyTask(task)) {
        const auto lines = nwchemAdvancedLines(d, "frequency");
        if (!lines.isEmpty())
            text += "\nfreq\n" + lines + "end\n";
        if (!check.numericalFrequencies && advanced.contains("hessian_thresh"))
            text += "\nhessian\n  thresh " + nwchemAdvancedNumber(d, "hessian_thresh", "1e-6") + "\nend\n";
    }
    if (task == "irc") {
        // Gonzalez–Schlegel steps along the transition vector, forward and backward from the start
        // unless one direction is chosen.
        const auto direction = s["irc_direction"].toString();
        text += "\nmepgs\n  maxmep " + integer("irc_points") + "\n  maxiter " + integer("opt_maxiter") +
                "\n  stride " + shortest(s["irc_stride"].toDouble()) +
                (direction == "both" ? QString() : "\n  " + direction) + "\nend\n";
    }
    if (s["bsse"].toBool()) {
        text += "\nbsse\n";
        const auto monomers = bsseMonomers(d);
        for (int i = 0; i < monomers.size(); ++i) {
            QStringList numbers;
            for (const auto &id : monomers[i])
                numbers << QString::number(d.index(id) + 1);
            // Continue long atom lists with a backslash, as for the fixed atoms.
            QStringList lines;
            for (int k = 0; k < numbers.size(); k += 16)
                lines << numbers.mid(k, 16).join(' ');
            text += QString("  mon m%1 %2\n  charge %3\n  mult 1\n")
                        .arg(i + 1)
                        .arg(lines.join(" \\\n    "))
                        .arg(monomerCharge(d, monomers[i]));
        }
        text += "end\n";
    }
    if (!str("extra_input").trimmed().isEmpty())
        text += "\n# Additional input (not checked by MolForma)\n" + str("extra_input").trimmed() + '\n';
    const QString theory = theoryOf(method);
    text += '\n';
    if (scan) {
        const auto ids = idsFromJson(s["scan_atoms"]);
        const int points = s["scan_steps"].toInt();
        for (int step = 0; step < points; ++step) {
            const double value = scanValue(s, step);
            NwchemGeometryOptions options;
            options.name = QString("scan%1").arg(step + 1);
            options.constraints = false;
            if (relaxedScan)
                options.zcoord = constraintLines(d, s, scale) << zcoordLine(d, ids, value, scale, "mfscan");
            // The title marks each point in the log, so results can be read back per point.
            text += QString("\ntitle \"MolForma scan %1/%2 %3 = %4\"\n")
                        .arg(step + 1)
                        .arg(points)
                        .arg(zcoordLine(d, ids, value, 1, "").section(' ', 0, ids.size()), shortest(value));
            text += nwchemGeometry(scanGeometry(d, s, step), unit, false, options);
            text += "set geometry " + options.name + "\ntask " + theory +
                    (relaxedScan ? " optimize\n" : " energy\n");
        }
    } else if (task == "irc")
        text += "task " + theory + " mepgs\n";
    else {
        if (optimizationTask(task))
            text += "task " + theory + (saddleTask(task) ? " saddle\n" : " optimize\n");
        if (frequencyTask(task))
            text +=
                "task " + theory + " frequencies" + (check.numericalFrequencies ? " numerical" : "") + '\n';
        else if (method == "dft" && s["tddft"].toBool())
            text += "task tddft energy\n";
        else if (!optimizationTask(task))
            text += "task " + theory + ' ' + task + '\n';
    }
    Document last = d;
    auto lastCheck = check;
    if (s["followup"].toBool()) {
        // NWChem keeps every setting in its runtime database: clear the method blocks so options of
        // the first level (CAM parameters, dispersion, convergence) do not leak into the second.
        last = followupDocument(d);
        const auto ns = nwchemSettings(last);
        lastCheck = validateNwchem(last);
        const auto &next = lastCheck;
        text += "\n# Follow-up single point at the optimized structure\nunset scf:*\nunset dft:*\n";
        if (correlated(ns["method"].toString()))
            text += ns["method"] == "mp2" ? "unset mp2:*\n" : "unset ccsd:*\n";
        text += basisBlocks(last, ns, false) + methodBlocks(last, ns, next);
        text += "\ntask " + theoryOf(ns["method"].toString()) + " energy\n";
    }
    if (anyProperty(s)) {
        const auto level = theoryOf(nwchemSettings(last)["method"].toString());
        if (s["prop_mulliken"].toBool() || s["prop_nmr"].toBool() || s["prop_polarizability"].toBool()) {
            text += "\nproperty\n";
            if (s["prop_mulliken"].toBool())
                text += "  mulliken\n";
            if (s["prop_nmr"].toBool())
                text += "  shielding\n";
            if (s["prop_polarizability"].toBool())
                text += "  aoresponse 1 0.0\n"; // Static dipole polarizability.
            text += "end\ntask " + level + " property\n";
        }
        if (s["prop_esp"].toBool())
            text += "\nesp\n  restrain\nend\ntask esp\n";
    }
    text += nwchemOutputLines(last, lastCheck);
    if (!str("extra_tasks").trimmed().isEmpty())
        text += "\n# Additional input (not checked by MolForma)\n" + str("extra_tasks").trimmed() + '\n';
    return text;
}
namespace {
// Lines of `input` that a shortest edit script from `base` inserts (Myers' O(ND) diff, after the
// common head and tail). Settings change a few lines; a larger difference marks nothing.
QVector<bool> insertedLines(const QStringList &input, const QStringList &base) {
    QVector<bool> inserted(input.size(), false);
    int start = 0, endA = input.size(), endB = base.size();
    while (start < endA && start < endB && input[start] == base[start])
        ++start;
    while (endA > start && endB > start && input[endA - 1] == base[endB - 1])
        --endA, --endB;
    const int n = endB - start, m = endA - start; // x walks the base, y the input.
    if (m == 0)
        return inserted;
    if (n == 0) {
        std::fill(inserted.begin() + start, inserted.begin() + endA, true);
        return inserted;
    }
    const int offset = n + m, limit = std::min(n + m, 2000);
    QVector<int> v(2 * offset + 2, 0);
    QVector<QVector<int>> trace;
    for (int d = 0; d <= limit; ++d) {
        trace.append(v);
        for (int k = -d; k <= d; k += 2) {
            const bool down = k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1]);
            int x = down ? v[offset + k + 1] : v[offset + k - 1] + 1, y = x - k;
            while (x < n && y < m && base[start + x] == input[start + y])
                ++x, ++y;
            v[offset + k] = x;
            if (x < n || y < m)
                continue;
            // Back from the end: each step down of the path inserts one input line.
            for (int step = d; step > 0; --step) {
                const auto &before = trace[step];
                const int key = x - y;
                const bool fromAbove =
                    key == -step || (key != step && before[offset + key - 1] < before[offset + key + 1]);
                const int previous = fromAbove ? key + 1 : key - 1;
                const int px = before[offset + previous], py = px - previous;
                if (fromAbove)
                    inserted[start + py] = true;
                x = px;
                y = py;
            }
            return inserted;
        }
    }
    return QVector<bool>(input.size(), false);
}
QStringList generatedLines(const Document &d) {
    try {
        return nwchemInput(d).split('\n');
    } catch (const std::exception &) {
        return {};
    }
}
} // namespace
NwchemInputOutline nwchemInputOutline(const Document &d, const QString &input) {
    NwchemInputOutline outline;
    if (input.isEmpty())
        return outline;
    const auto lines = input.split('\n');
    auto plain = d;
    resetNwchemAdvanced(plain);
    auto bare = d;
    auto s = bare.data["nwchem"].toObject();
    s.remove("extra_input");
    s.remove("extra_tasks");
    bare.data["nwchem"] = s;
    for (auto [document, marks] : {std::pair{&plain, &outline.advanced}, std::pair{&bare, &outline.extra}}) {
        const auto base = generatedLines(*document);
        if (base.isEmpty())
            continue;
        const auto inserted = insertedLines(lines, base);
        for (int i = 0; i < inserted.size(); ++i)
            if (inserted[i] && !lines[i].trimmed().isEmpty())
                marks->insert(i);
    }
    // Where each keyword is set: a tab and a setting of it, or a group of the details of "2 計算条件".
    static const QHash<QString, std::pair<int, const char *>> sources{{"start", {2, "start"}},
                                                                      {"title", {2, "title"}},
                                                                      {"memory", {3, "memory_mb"}},
                                                                      {"charge", {2, "charge"}},
                                                                      {"geometry", {1, ""}},
                                                                      {"constraints", {1, ""}},
                                                                      {"basis", {2, "basis"}},
                                                                      {"ecp", {2, "basis"}},
                                                                      {"bq", {1, ""}},
                                                                      {"relativistic", {2, "relativistic"}},
                                                                      {"scf", {2, "method"}},
                                                                      {"dft", {2, "method"}},
                                                                      {"mp2", {2, "method"}},
                                                                      {"ccsd", {2, "method"}},
                                                                      {"cosmo", {2, "solvent"}},
                                                                      {"tddft", {2, "tddft"}},
                                                                      {"driver", {2, "opt_maxiter"}},
                                                                      {"mepgs", {2, "irc_points"}},
                                                                      {"bsse", {2, "bsse"}},
                                                                      {"property", {2, "prop_mulliken"}},
                                                                      {"esp", {2, "prop_esp"}},
                                                                      {"unset", {2, "followup"}},
                                                                      {"set", {2, "task"}},
                                                                      {"task", {2, "task"}}};
    static const QHash<QString, QString> groups{{"scf", "scf"},
                                                {"dft", "dft"},
                                                {"mp2", "correlation"},
                                                {"ccsd", "correlation"},
                                                {"driver", "driver"},
                                                {"freq", "frequency"},
                                                {"hessian", "frequency"},
                                                {"cosmo", "cosmo"},
                                                {"geometry", "driver"},
                                                {"property", "visualization"},
                                                {"dplot", "visualization"},
                                                {"task", "visualization"},
                                                {"echo", "files"},
                                                {"scratch_dir", "files"},
                                                {"permanent_dir", "files"},
                                                {"memory", "files"}};
    static const QRegularExpression atomLine(R"(^\s+\S+(?:\s+[-+0-9.eE]+){3}\s*$)");
    for (int i = 0; i < lines.size(); ++i) {
        const auto &line = lines[i];
        if (line.isEmpty() || line[0].isSpace() || line.startsWith('#') || line == "end")
            continue;
        NwchemInputBlock block;
        block.line = i;
        block.keyword = line.section(' ', 0, 0).toLower();
        int last = i;
        // A block: indented lines up to an "end" of its own.
        if (i + 1 < lines.size() && !lines[i + 1].isEmpty() && lines[i + 1][0].isSpace())
            for (int j = i + 1; j < lines.size(); ++j) {
                if (lines[j] == "end") {
                    last = j;
                    break;
                }
                if (lines[j].isEmpty() || !lines[j][0].isSpace())
                    break;
                last = j;
            }
        block.lines = last - i + 1;
        block.title = line.trimmed();
        if (block.keyword == "geometry") {
            int atoms = 0;
            for (int j = i + 1; j <= last; ++j)
                atoms += atomLine.match(lines[j]).hasMatch();
            block.title = tr("geometry（%n 原子）", nullptr, atoms);
        } else if (block.keyword == "basis") // The library of the common basis, if there is one.
            for (int j = i + 1; j <= last; ++j)
                if (lines[j].startsWith("  * library "))
                    block.title = "basis " + lines[j].mid(12).trimmed();
        int advanced = 0, extra = 0;
        for (int j = i; j <= last; ++j) {
            advanced += outline.advanced.contains(j);
            extra += outline.extra.contains(j);
        }
        const auto source = sources.value(block.keyword, {2, ""});
        if (extra == block.lines) {
            block.tabs = {2};
            block.setting = "extra_input";
        } else if (advanced == block.lines) {
            block.tabs = {2};
            block.group = groups.value(block.keyword, "files");
        } else {
            block.tabs = {source.first};
            block.setting = source.second;
            if (block.keyword == "bq" || (advanced && source.first != 2))
                block.tabs << 2; // Point charges are included there; the details are part of it.
            if (advanced)
                block.group = groups.value(block.keyword, "files");
        }
        outline.blocks << block;
        i = last;
    }
    return outline;
}
} // namespace mf
