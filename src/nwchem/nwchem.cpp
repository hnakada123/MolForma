#include "nwchem/nwchem.h"
#include "model/geometry.h"
#include "model/i18n.h"
#include "nwchem/input_builder.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_geometry.h"
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
    QJsonObject defaults;
    for (const auto &spec : nwchemSettingSpecs())
        if (!spec.job)
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
const QVector<NwchemSettingSpec> &nwchemSettingSpecs() {
    using Kind = NwchemSettingKind;
    using Scope = NwchemSettingScope;
    static const QVector<NwchemSettingSpec> specs = [] {
        QVector<NwchemSettingSpec> list{
            {"start", Kind::String, "molforma"},
            {"title", Kind::String, "MolForma calculation"},
            {"memory_mb", Kind::Integer, 1024, 64, 1048576},
            {"charge", Kind::Integer, 0, -1000, 1000},
            {"multiplicity", Kind::Integer, 1, 1, 1000},
            {"method", Kind::Choice, "hf"},
            {"task", Kind::Choice, "energy"},
            {"basis", Kind::Choice, "sto-3g"},
            {"reference", Kind::Choice, "auto"},
            {"functional", Kind::Choice, "b3lyp", 0, 0, 0, Scope::Dft},
            {"xc_custom", Kind::String, "", 0, 0, 0, Scope::CustomXc},
            {"dispersion", Kind::Choice, "none", 0, 0, 0, Scope::Dft},
            {"basis_functions", Kind::Choice, "auto"},
            {"basis_overrides", Kind::Object, QJsonObject()},
            {"geometry_units", Kind::Choice, "angstrom"},
            {"scf_maxiter", Kind::Integer, 100, 1, 10000},
            {"scf_convergence", Kind::Choice, "tight"},
            {"grid", Kind::Choice, "fine", 0, 0, 0, Scope::Dft},
            {"relativistic", Kind::Choice, "none", 0, 0, 0, Scope::Relativistic},
            {"ri_j", Kind::Flag, false, 0, 0, 0, Scope::Dft},
            {"opt_maxiter", Kind::Integer, 100, 1, 10000, 0, Scope::Driver},
            {"opt_convergence", Kind::Choice, "default", 0, 0, 0, Scope::Driver},
            {"numerical_frequencies", Kind::Flag, false, 0, 0, 0, Scope::Frequency},
            {"freeze_core", Kind::Flag, false, 0, 0, 0, Scope::Correlation},
            {"cc_maxiter", Kind::Integer, 50, 1, 10000, 0, Scope::Ccsd},
            {"solvent", Kind::Choice, "none"},
            {"solvent_name", Kind::Choice, "dielectric", 0, 0, 0, Scope::Solvent},
            {"dielectric", Kind::Real, 78.4, 1, 1000, 4, Scope::Dielectric},
            {"include_point_charges", Kind::Flag, false},
            {"tddft", Kind::Flag, false, 0, 0, 0, Scope::ExcitedToggle},
            {"tddft_roots", Kind::Integer, 10, 1, 100, 0, Scope::Excited},
            {"tddft_triplets", Kind::Flag, false, 0, 0, 0, Scope::Excited},
            {"tddft_tda", Kind::Flag, false, 0, 0, 0, Scope::Excited},
            {"constraints", Kind::Array, QJsonArray(), 0, 0, 0, Scope::Driver},
            {"scan_atoms", Kind::Array, QJsonArray(), 0, 0, 0, Scope::Scan},
            {"scan_start", Kind::Real, 1.0, -180, 180, 4, Scope::Scan},
            {"scan_end", Kind::Real, 2.0, -180, 180, 4, Scope::Scan},
            {"scan_steps", Kind::Integer, 6, 2, 50, 0, Scope::Scan},
            {"scan_relaxed", Kind::Flag, true, 0, 0, 0, Scope::Scan},
            {"irc_points", Kind::Integer, 30, 1, 500, 0, Scope::Irc},
            {"irc_stride", Kind::Real, 0.1, .01, 1, 3, Scope::Irc},
            {"irc_direction", Kind::Choice, "both", 0, 0, 0, Scope::Irc},
            {"bsse", Kind::Flag, false},
            {"followup", Kind::Flag, false, 0, 0, 0, Scope::FollowupToggle},
            {"followup_method", Kind::Choice, "dft", 0, 0, 0, Scope::Followup},
            {"followup_functional", Kind::Choice, "b3lyp", 0, 0, 0, Scope::FollowupDft},
            {"followup_dispersion", Kind::Choice, "none", 0, 0, 0, Scope::FollowupDft},
            {"followup_basis", Kind::Choice, "def2-tzvp", 0, 0, 0, Scope::Followup},
            {"prop_mulliken", Kind::Flag, false},
            {"prop_esp", Kind::Flag, false},
            {"prop_nmr", Kind::Flag, false},
            {"prop_polarizability", Kind::Flag, false},
            {"extra_input", Kind::String, ""},
            {"extra_tasks", Kind::String, ""}};
        for (auto &spec : list) {
            spec.job = spec.key == "charge" || spec.key == "multiplicity";
            spec.allowDisable = QStringList{"relativistic", "ri_j", "tddft", "followup"}.contains(spec.key);
            spec.validateUnused = spec.key != "scan_steps";
            // Atom references and free input belong to a project, not a reusable method.
            spec.preset =
                !spec.job &&
                !QStringList{"start",      "title",      "memory_mb", "geometry_units", "constraints",
                             "scan_atoms", "scan_start", "scan_end",  "extra_input",    "extra_tasks"}
                     .contains(spec.key);
            for (const auto &choice : choiceSources(spec.key))
                spec.choices << NwchemChoice{choice.value, choice.label};
        }
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
bool nwchemSettingApplies(const NwchemSettingSpec &spec, const QJsonObject &s) {
    const auto method = s["method"].toString(), task = s["task"].toString();
    using Scope = NwchemSettingScope;
    switch (spec.scope) {
    case Scope::Always:
        return true;
    case Scope::Dft:
        return method == "dft";
    case Scope::CustomXc:
        return method == "dft" && s["functional"] == "custom";
    case Scope::Correlation:
        return correlated(method);
    case Scope::Ccsd:
        return method.startsWith("ccsd");
    case Scope::Driver:
        return nwchemDriverTask(s) || task == "irc";
    case Scope::Frequency:
        return frequencyTask(task);
    case Scope::Irc:
        return task == "irc";
    case Scope::Scan:
        return task == "scan";
    case Scope::FollowupToggle:
        return optimizationTask(task);
    case Scope::Followup:
        return s["followup"].toBool();
    case Scope::FollowupDft:
        return s["followup"].toBool() && s["followup_method"] == "dft";
    case Scope::Solvent:
        return s["solvent"] != "none";
    case Scope::Dielectric:
        return s["solvent"] == "cosmo" && s["solvent_name"] == "dielectric";
    case Scope::ExcitedToggle:
        return method == "dft" && task == "energy";
    case Scope::Excited:
        return method == "dft" && s["tddft"].toBool();
    case Scope::Relativistic:
        return method == "hf" || method == "dft";
    }
    return false;
}
bool nwchemSettingEditable(const NwchemSettingSpec &spec, const QJsonObject &settings) {
    return nwchemSettingApplies(spec, settings) || (spec.allowDisable && settings[spec.key] != spec.initial);
}
void setNwchemSetting(Document &d, const QString &key, const QJsonValue &value) {
    const auto &spec = nwchemSettingSpec(key);
    if (spec.job)
        d.setJobValue(key, value);
    else {
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
    for (const auto &message : d.validate())
        result.error("geometry.validation", {}, message, {}, 0);
    auto &errors = result.errors;
    const auto s = nwchemSettings(d);
    const auto advanced = nwchemAdvanced(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    if (d.data.contains("nwchem") && !d.data["nwchem"].isObject())
        result.error("settings.object", "nwchem", tr("NWChem設定はオブジェクトである必要があります。"));
    for (const auto &spec : nwchemSettingSpecs()) {
        const auto value = spec.job ? d.job()[spec.key] : s[spec.key];
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
            result.error("basis.override", "basis_overrides",
                         tr("元素別の基底「%1: %2」は未対応です。").arg(it.key(), it.value().toString()));
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
        result.error("basis.coverage", "basis",
                     tr("基底 %1 の対応元素に含まれません: %2").arg(basis, sorted(unsupported).join(", ")));
    if (!realAtoms)
        result.error("geometry.empty", QString(),
                     tr("実原子を追加してください（ゴーストのみの計算はできません）。"), {}, 0);
    result.electrons -= charge;
    if (correlated(method) && advanced.contains("corr_freeze"))
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
        result.reference = mult == 1 ? "rhf" : (method == "hf" ? "rohf" : "uhf");
    if (result.reference == "rhf" && mult != 1)
        result.error("reference.closed_shell", "reference",
                     tr("RHF / RKSは多重度1で使用してください。開殻には自動または非制限を選択してください。"),
                     DiagnosticFix{"reference", tr("参照波動関数を「自動」にする"), "auto"});
    if ((method == "dft" || method == "mp2") && result.reference == "rohf")
        result.error("reference.rohf", "reference",
                     tr("このタブのDFT・MP2はROHF参照に対応しません。自動または非制限を選択してください。"));
    if (method.startsWith("ccsd") && (result.reference != "rhf" || mult != 1 || task != "energy"))
        result.error("correlation.reference", "method",
                     tr("CCSD / CCSD(T)は閉殻RHFの一点エネルギーに対応します。"));
    if (method == "dft") {
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
        if (method != "dft")
            result.error("tddft.method", "tddft", tr("TDDFTはDFTで使用してください。"));
        else if (f && f->metaGga)
            result.error("tddft.functional", "functional",
                         tr("NWChemのTDDFTはメタGGA（%1）に対応しません。B3LYP・PBE0・CAM-"
                            "B3LYP等を選んでください。")
                             .arg(tr(f->label)));
        if (task != "energy")
            result.error(
                "tddft.task", "task",
                tr("TDDFTは計算目的「一点エネルギー」で使用してください（励起状態の構造最適化は未対応）。"),
                DiagnosticFix{"task", tr("計算目的を「一点エネルギー」にする"), "energy"});
    }
    if (correlated(method) && result.electrons < 2)
        result.error("correlation.electrons", "method", tr("相関計算には2個以上の電子が必要です。"));
    if (result.frozenOrbitals &&
        (result.frozenOrbitals > result.beta || result.electrons - 2 * result.frozenOrbitals < 2))
        result.error("correlation.frozen", "freeze_core",
                     tr("内殻凍結後の相関電子が不足します。内殻凍結を解除してください。"));
    if (correlated(method) && (s["freeze_core"].toBool() || advanced.contains("corr_freeze")))
        result.warning("correlation.freeze_notice", "freeze_core",
                       tr("内殻凍結: %n 軌道。%1", nullptr, result.frozenOrbitals)
                           .arg(advanced.contains("corr_freeze")
                                    ? tr("詳細設定の明示的な軌道数を使用します。")
                                    : tr("ECP中心とゴーストはfreeze atomicの対象外です。")));
    const auto solvent = str("solvent"), solventName = str("solvent_name");
    if (solvent != "none") {
        const QString model = solvent == "smd" ? "SMD" : "COSMO";
        if (method != "hf" && method != "dft")
            result.error("solvent.method", "solvent", tr("%1はHFまたはDFTで使用してください。").arg(model));
        if (solvent == "smd" && method != "dft")
            result.error("solvent.smd_method", "solvent",
                         tr("NWChemのSMDはDFT専用です。HFにはCOSMOを使用してください。"),
                         DiagnosticFix{"solvent", tr("溶媒モデルをCOSMOにする"), "cosmo"});
        if (solvent == "smd" && solventName == "dielectric")
            result.error("solvent.smd_name", "solvent_name",
                         tr("SMDでは溶媒名を選んでください（誘電率だけでは非静電項を決められません）。"),
                         DiagnosticFix{"solvent_name", tr("溶媒を水にする"), "h2o"});
        if (ghosts)
            result.error("solvent.ghosts", "solvent", tr("ゴーストを含む%1入力は未対応です。").arg(model));
        if (method == "hf" && result.reference == "uhf")
            result.error(
                "solvent.uhf", "reference",
                tr("NWChemのCOSMOはUHFに対応しません。参照を自動（ROHF）にするか、DFTを選択してください。"));
    }
    const auto relativistic = str("relativistic");
    if (relativistic != "none") {
        if (method != "hf" && method != "dft")
            result.error("relativistic.method", "relativistic",
                         tr("相対論ハミルトニアンはHFまたはDFTで使用してください。"));
        if (relativistic == "zora" && method != "dft")
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
        if (method != "dft")
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
        result.error("constraints.count", "constraints", tr("内部座標の拘束は50個までです。"));
    for (int i = 0; i < constraints.size(); ++i) {
        const auto c = constraints[i].toObject();
        const auto ids = idsFromJson(c["atoms"]);
        const auto error = c["value"].isDouble() ? coordinateError(d, ids, c["value"].toDouble())
                                                 : tr("値を数値で指定してください。");
        if (!error.isEmpty())
            result.error("constraints.coordinate", "constraints",
                         tr("拘束%1: %2").arg(QString::number(i + 1), error));
    }
    if (!constraints.isEmpty()) {
        if (!optimizationTask(task) && !(task == "scan" && s["scan_relaxed"].toBool()))
            result.error("constraints.task", "constraints",
                         tr("内部座標の拘束は構造最適化・遷移状態探索・緩和スキャンで使用してください。"));
        if (fixed || ghosts)
            result.error(
                "constraints.fixed", "constraints",
                tr("内部座標の拘束は固定原子・ゴーストと併用できません（NWChemはCartesianの固定と内部座標"
                   "を同時に扱えません）。固定を解除してください。"));
        result.warning(
            "constraints.autoz", "constraints",
            tr("拘束付き最適化は内部座標（autoz）で行います。NWChemが内部座標を生成できない構造では"
               "拘束を適用できません。"));
        if (frequencyTask(task))
            result.warning(
                "constraints.frequencies", "constraints",
                tr("拘束した構造は停留点ではないため、振動数には虚振動や大きな誤差が出ることがあります。"));
    }
    const bool scan = task == "scan";
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
        if (errors.isEmpty()) {
            try {
                for (int step = 0; step < s["scan_steps"].toInt(); ++step)
                    scanGeometry(d, s, step);
            } catch (const std::exception &e) {
                result.error("scan.geometry", "scan_atoms",
                             tr("スキャン: %1").arg(QString::fromUtf8(e.what())));
            }
        }
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
    if (task == "irc") {
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
    if (outsideRange("irc_stride"))
        result.error("irc.stride", "irc_stride", tr("IRCの刻み幅は0.01〜1で指定してください。"));
    if (s["tddft"].toBool() && (scan || task == "irc"))
        result.error("tddft.path", "tddft", tr("TDDFTはスキャン・IRCと併用できません。"));
    // Counterpoise correction of the bonded molecules as monomers.
    if (s["bsse"].toBool()) {
        const auto monomers = bsseMonomers(d);
        if (!QStringList{"energy", "gradient", "optimize"}.contains(task))
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
        if (!d.data["point_charges"].toArray().isEmpty() && s["include_point_charges"].toBool())
            result.error("bsse.point_charges", "bsse", tr("BSSE補正は外部点電荷と併用できません。"));
        if (advanced["cube_output"].toBool() || advanced["molden_output"].toBool())
            result.error("bsse.output", "bsse", tr("BSSE補正の計算ではCUBE・MOLDENを出力できません。"));
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
        const auto level = s["followup"].toBool() ? s["followup_method"].toString() : method;
        if (level != "hf" && level != "dft")
            result.error("properties.method", "prop_mulliken",
                         tr("物性計算は最後の計算レベルがHFまたはDFTのときに使用してください。"));
        if (scan || task == "irc")
            result.error("properties.path", "prop_mulliken", tr("物性計算はスキャン・IRCと併用できません。"));
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
    if (s["followup"].toBool()) {
        if (!optimizationTask(task))
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
    if (frequencyTask(task) && (ghosts || fixed))
        result.error("frequencies.fixed", "task",
                     tr("固定原子・ゴーストを含む振動数解析は未対応です。固定を解除し、実原子のみで設定してく"
                        "ださい。"));
    if (frequencyTask(task) && realAtoms < 2)
        result.error("frequencies.atoms", "task", tr("振動数解析には2個以上の実原子が必要です。"));
    if (optimizationTask(task) && realAtoms && !movable)
        result.error("optimization.fixed", "task",
                     tr("全原子が固定されています。最適化する原子の固定を解除してください。"));
    if (optimizationTask(task) && ghosts)
        result.warning(
            "optimization.ghosts", "task",
            tr("ゴースト中心は核を持たないため、最適化中は固定します（constraints の fix atom）。"));
    const bool points = !d.data["point_charges"].toArray().isEmpty();
    if (frequencyTask(task) && points && s["include_point_charges"].toBool())
        result.error("frequencies.point_charges", "include_point_charges",
                     tr("外部点電荷中の振動数解析は未対応です。"));
    result.numericalFrequencies = nwchemNumericalFrequencies(d);
    if (frequencyTask(task) && result.numericalFrequencies)
        result.warning("frequencies.numerical", "numerical_frequencies",
                       tr("振動数には数値Hessianを使用します。複数回の勾配計算を行います。"));
    if (frequencyTask(task) && s["scf_convergence"] == "default")
        result.warning(
            "frequencies.convergence", "scf_convergence",
            tr("SCF収束がNWChem既定のため、振動数（特に数値Hessian）の精度が不足することがあります。"
               "「厳密」を推奨します。"));
    if (task == "frequencies")
        result.warning("frequencies.geometry", "task",
                       tr("現在の座標で振動数を計算します。極小構造の確認には先に最適化してください。"));
    if (saddleTask(task))
        result.warning(
            "saddle.initial", "task",
            tr("遷移状態探索は、反応座標に沿って1方向だけ負の曲率を持つ初期構造から始めてください。"
               "振動数で虚振動が1つであることを確認してください。"));
    if (result.ecpElectrons)
        result.warning("basis.ecp", "basis",
                       tr("ECP（%1）を自動指定します（除去する内殻電子: %2）。")
                           .arg(sorted(ecpLibraries).join(tr("・")), QString::number(result.ecpElectrons)));
    QSet<bool> conventions{cartesianByConvention(basis)};
    for (const auto &a : d.atoms)
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
    if (s["tddft"].toBool() && method == "dft" && result.reference == "uhf" && s["tddft_triplets"].toBool())
        result.warning("tddft.open_shell", "tddft_triplets",
                       tr("開殻（UKS）のTDDFTは一重項・三重項を分けずに非制限の励起状態を求めます。"));
    if (frequencyTask(task))
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
    if (!d.data["references"].toArray().isEmpty() || !d.data["invalid_settings"].toArray().isEmpty() ||
        !d.data["assignments"].toObject().isEmpty() || !d.job()["sections"].toObject().isEmpty())
        result.warning(
            "legacy.settings", QString(),
            tr("旧形式の詳細・拘束・断片設定は変換しません。NWChem計算条件と原子の固定フラグを使用し"
               "ます。"),
            {}, 0);
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
        QSet<QString> mainEcp, nextEcp;
        for (const auto &a : d.atoms)
            if (!a.ghost) {
                if (ecpCore(elementBasis(s, a.element), element(a.element).number))
                    mainEcp.insert(a.element);
                if (ecpCore(elementBasis(ns, a.element), element(a.element).number))
                    nextEcp.insert(a.element);
            }
        if (!mainEcp.isEmpty() && nextEcp.isEmpty())
            result.error(
                "followup.ecp", "followup_basis",
                tr("主計算のECPはNWChemの実行中に残るため、後続の一点計算にもECPを使う基底（def2系・LANL2DZ）"
                   "を選んでください。"));
        result.warning(
            "followup.notice", "followup",
            tr("最適化した構造で %1 / %2 の一点エネルギーを計算します。")
                .arg(ns["method"] == "dft" ? nwchemFunctionalLabel(ns) : ns["method"].toString().toUpper(),
                     ns["basis"].toString()));
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
    out.block("basis", text, "basis", {}, {2}, perElement ? QString() : "basis \"" + basis + '"');
    if (s["method"] == "dft" && s["ri_j"].toBool()) {
        text = "\nbasis \"cd basis\"" + type + "  * library \"weigend coulomb fitting\"\n";
        for (const auto &symbol : sorted(ghosts))
            text += QString("  bq%1 library %1 \"weigend coulomb fitting\"\n").arg(symbol);
        out.block("basis", text + "end\n", "ri_j");
    }
    if (!ecps.isEmpty()) {
        text = "\necp\n";
        for (const auto &symbol : sorted(ecps))
            text += QString("  %1 library \"%2\"\n").arg(symbol, libraryBasis(elementBasis(s, symbol))->ecp);
        out.block("ecp", text + "end\n", "basis");
    }
}
void methodBlocks(NwchemInputBuilder &out, const Document &d, const QJsonObject &s,
                  const NwchemValidation &check) {
    const auto advanced = nwchemAdvanced(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    auto integer = [&](const char *key) { return QString::number(s[key].toInt()); };
    const auto method = str("method");
    const bool own = str("scf_convergence") == "default";
    const QString threshold = nwchemScfThreshold(s);
    NwchemInputBuilder body;
    if (method == "dft") {
        const auto *f = functional(str("functional"));
        body.append("\ndft\n  xc " +
                    (QString(f->value) == "custom" ? str("xc_custom").simplified() : QString(f->xc)) + '\n');
        if (*f->extra)
            body.append(QString("  ") + f->extra + '\n');
        body.append("  mult " + QString::number(d.job()["multiplicity"].toInt()) + '\n');
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
        body.append("\nscf\n  " + check.reference + "\n  nopen " +
                    QString::number(d.job()["multiplicity"].toInt() - 1) + "\n  maxiter " +
                    integer("scf_maxiter") + '\n');
        if (!own || advanced.contains("scf_thresh"))
            body.sourced("  thresh " + nwchemAdvancedNumber(d, "scf_thresh", threshold) + '\n',
                         {"scf_thresh"}, advanced.contains("scf_thresh"));
        body.append(nwchemAdvancedLines(d, "scf"));
        body.append(nwchemAdvancedLines(d, "vectors"));
        body.append("end\n");
        out.block("scf", body.result, "method", "scf");
        if (correlated(method)) {
            NwchemInputBuilder correlation;
            correlation.append(method == "mp2" ? "\nmp2\n" : "\nccsd\n");
            correlation.sourced(
                "  freeze " +
                    nwchemAdvancedNumber(d, "corr_freeze", s["freeze_core"].toBool() ? "atomic" : "0") + '\n',
                {"corr_freeze"}, advanced.contains("corr_freeze"));
            if (method != "mp2") {
                correlation.append("  maxiter " + integer("cc_maxiter") + '\n');
                correlation.sourced("  thresh " + nwchemAdvancedNumber(d, "cc_thresh", "1e-7") + '\n',
                                    {"cc_thresh"}, advanced.contains("cc_thresh"));
            }
            correlation.append(nwchemAdvancedLines(d, "correlation"));
            correlation.append("end\n");
            out.block(method == "mp2" ? "mp2" : "ccsd", correlation.result, "method", "correlation");
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
} // namespace
NwchemGeneratedInput generateNwchemInput(const Document &d) {
    const auto check = validateNwchem(d);
    if (!check.errors.isEmpty())
        fail(check.errors.join('\n'));
    const auto s = nwchemSettings(d), advanced = nwchemAdvanced(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    auto integer = [&](const char *key) { return QString::number(s[key].toInt()); };
    const auto method = str("method"), task = str("task");
    const auto unit = str("geometry_units") == "bohr" ? CoordinateUnit::Bohr : CoordinateUnit::Angstrom;
    const double scale = coordinateScale(unit);
    const bool scan = task == "scan", relaxedScan = scan && s["scan_relaxed"].toBool();
    NwchemInputBuilder out;
    out.append("# Generated by MolForma for NWChem\n");
    out.block("start", "start " + str("start") + '\n', "start");
    if (!str("title").isEmpty())
        out.block("title", "title \"" + str("title") + "\"\n", "title");
    out.block("memory", "memory total " + integer("memory_mb") + " mb\n", "memory_mb", {}, {3});
    out.append(nwchemAdvancedLines(d, "files"));
    out.block("charge", "charge " + QString::number(d.job()["charge"].toInt()) + '\n', "charge");
    out.append("\n");
    if (!scan) {
        Document geometry = d;
        if (optimizationTask(task))
            for (auto &a : geometry.atoms)
                a.fixed |= a.ghost;
        NwchemGeometryOptions options;
        if (optimizationTask(task))
            options.zcoord = constraintLines(d, s, scale);
        options.autoz = nwchemAutoz(d);
        geometryBlocks(out, geometry, unit, options,
                       advanced["driver_cartesian"].toBool() && optimizationTask(task));
    }
    basisBlocks(out, d, s, s["bsse"].toBool());
    if (s["include_point_charges"].toBool() && !d.data["point_charges"].toArray().isEmpty()) {
        QString text = "\nbq units angstrom\n";
        for (const auto &value : d.data["point_charges"].toArray()) {
            const auto p = value.toObject();
            const auto xyz = vectorFromJson(p["position"]);
            text += QString("  %1 %2 %3 %4\n")
                        .arg(shortest(xyz.x()), shortest(xyz.y()), shortest(xyz.z()),
                             shortest(p["charge"].toDouble()));
        }
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
    if (method == "dft" && s["tddft"].toBool()) {
        QString text = "\ntddft\n  nroots " + integer("tddft_roots") + '\n';
        if (check.reference == "rhf" && !s["tddft_triplets"].toBool())
            text += "  notriplet\n";
        if (s["tddft_tda"].toBool())
            text += "  cis\n";
        out.block("tddft", text + "end\n", "tddft");
    }
    if (nwchemDriverTask(s)) {
        NwchemInputBuilder body;
        body.append("\ndriver\n  maxiter " + integer("opt_maxiter") + "\n  " + str("opt_convergence") + '\n');
        body.append(nwchemAdvancedLines(d, "driver"));
        body.append("end\n");
        out.block("driver", body.result, "opt_maxiter", "driver");
    }
    if (frequencyTask(task)) {
        const auto lines = nwchemAdvancedLines(d, "frequency");
        if (!lines.text.isEmpty()) {
            NwchemInputBuilder body;
            body.sourced("\nfreq\n", {"frequency"}, true);
            body.append(lines);
            body.sourced("end\n", {"frequency"}, true);
            out.block("freq", body.result, {}, "frequency");
        }
        if (!check.numericalFrequencies && advanced.contains("hessian_thresh"))
            out.advancedBlock("hessian",
                              "\nhessian\n  thresh " + nwchemAdvancedNumber(d, "hessian_thresh", "1e-6") +
                                  "\nend\n",
                              {"hessian_thresh"}, "frequency");
    }
    if (task == "irc") {
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
            for (const auto &id : monomers[i])
                numbers << QString::number(d.index(id) + 1);
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
    out.append("\n");
    auto taskLine = [&](const QString &value) { out.block("task", value, "task"); };
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
            out.block("title",
                      QString("\ntitle \"MolForma scan %1/%2 %3 = %4\"\n")
                          .arg(step + 1)
                          .arg(points)
                          .arg(zcoordLine(d, ids, value, 1, "").section(' ', 0, ids.size()), shortest(value)),
                      "scan_steps");
            geometryBlocks(out, scanGeometry(d, s, step), unit, options, false);
            out.block("set", "set geometry " + options.name + '\n', "scan_steps");
            taskLine("task " + theory + (relaxedScan ? " optimize\n" : " energy\n"));
        }
    } else if (task == "irc")
        taskLine("task " + theory + " mepgs\n");
    else {
        if (optimizationTask(task))
            taskLine("task " + theory + (saddleTask(task) ? " saddle\n" : " optimize\n"));
        if (frequencyTask(task)) {
            NwchemInputBuilder body;
            body.sourced("task " + theory + " frequencies" +
                             (check.numericalFrequencies ? " numerical" : "") + '\n',
                         {"numerical_frequencies"});
            out.block("task", body.result, "task");
        } else if (method == "dft" && s["tddft"].toBool())
            taskLine("task tddft energy\n");
        else if (!optimizationTask(task))
            taskLine("task " + theory + ' ' + task + '\n');
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
        if (correlated(ns["method"].toString()))
            out.block("unset", ns["method"] == "mp2" ? QString("unset mp2:*\n") : QString("unset ccsd:*\n"),
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
        if (s["prop_mulliken"].toBool() || s["prop_nmr"].toBool() || s["prop_polarizability"].toBool()) {
            QString text = "\nproperty\n";
            if (s["prop_mulliken"].toBool())
                text += "  mulliken\n";
            if (s["prop_nmr"].toBool())
                text += "  shielding\n";
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
    out.append(nwchemOutputLines(last, lastCheck));
    extraInput(out, str("extra_tasks"), "extra_tasks");
    return out.result;
}
QString nwchemInput(const Document &d) {
    return generateNwchemInput(d).text;
}
} // namespace mf
