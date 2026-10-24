#include "nwchem/nwchem.h"
#include "editing/coordinates.h"
#include "model/geometry.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_output.h"
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>

namespace mf {
namespace {
bool frequencyTask(const QString &task) {
    return task == "frequencies" || task == "optimize_frequencies";
}
bool optimizationTask(const QString &task) {
    return task == "optimize" || task == "optimize_frequencies";
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
        {"lanl2dz ecp", "LANL2DZ（Na以降ECP）", {{1, 1}, {3, 57}, {72, 83}, {92, 94}}, "lanl2dz ecp"}};
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
        {"tpss", "TPSS（meta-GGA）", "xtpss03 ctpss03", "", true, true, true, true},
        {"scan", "SCAN（meta-GGA）", "scan", "", false, true, true, true},
        {"r2scan", "r²SCAN（meta-GGA）", "r2scan", "", false, true, true, true},
        {"m06-l", "M06-L（meta-GGA）", "m06-l", "", true, false, true, true},
        {"m06", "M06", "m06", "", true, false, true, true},
        {"m06-2x", "M06-2X", "m06-2x", "", true, false, true, true},
        {"pw6b95", "PW6B95", "pw6b95", "", false, true, true, true},
        {"cam-b3lyp", "CAM-B3LYP（長距離補正）", "xcamb88 1.00 lyp 0.81 vwn_5 0.19 hfexch 1.00",
         "cam 0.33 cam_alpha 0.19 cam_beta 0.46", true, true, true},
        {"wb97x-d3", "ωB97X-D3（D3を含む長距離補正）", "wb97x-d3", "", false, false, true},
        {"custom", "任意のxc指定", "", "", false, false, true}};
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
QString number(double n) {
    return QString::number(n, 'g', 17);
}
QStringList sorted(const QSet<QString> &set) {
    auto list = set.values();
    std::sort(list.begin(), list.end());
    return list;
}
} // namespace
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
            {"opt_maxiter", 100},
            {"opt_convergence", "default"},
            {"numerical_frequencies", false},
            {"freeze_core", false},
            {"cc_maxiter", 50},
            {"solvent", "none"},
            {"dielectric", 78.4},
            {"include_point_charges", false},
            {"tddft", false},
            {"tddft_roots", 10},
            {"tddft_triplets", false},
            {"tddft_tda", false}};
}
QJsonObject nwchemSettings(const Document &d) {
    auto settings = nwchemDefaults();
    const auto stored = withBasisFunctions(d.data["nwchem"].toObject());
    for (auto it = stored.begin(); it != stored.end(); ++it)
        settings[it.key()] = it.value();
    return settings;
}
QVector<NwchemChoice> nwchemChoices(const QString &key) {
    if (key == "method")
        return {
            {"hf", "Hartree–Fock"}, {"dft", "DFT"}, {"mp2", "MP2"}, {"ccsd", "CCSD"}, {"ccsd(t)", "CCSD(T)"}};
    if (key == "task")
        return {{"energy", "一点エネルギー"},
                {"gradient", "勾配"},
                {"optimize", "構造最適化"},
                {"frequencies", "振動数"},
                {"optimize_frequencies", "最適化 → 振動数"}};
    if (key == "reference")
        return {{"auto", "自動（多重度・手法から選択）"},
                {"rhf", "RHF / RKS（制限）"},
                {"uhf", "UHF / UKS（非制限）"},
                {"rohf", "ROHF（制限開殻HF）"}};
    if (key == "functional") {
        QVector<NwchemChoice> result;
        for (const auto &f : functionals())
            result << NwchemChoice{f.value, f.label};
        return result;
    }
    if (key == "dispersion")
        return {{"none", "なし"}, {"d3", "Grimme D3"}, {"d3bj", "Grimme D3(BJ)"}};
    if (key == "basis") {
        QVector<NwchemChoice> result;
        for (const auto &b : libraryBases())
            result << NwchemChoice{b.name, b.label};
        return result;
    }
    if (key == "basis_functions")
        return {{"auto", "自動（6-31G系はCartesian、他は球面）"},
                {"spherical", "球面調和関数（5d・7f）"},
                {"cartesian", "Cartesian（6d・10f）"}};
    if (key == "scf_convergence")
        return {{"normal", "標準"}, {"tight", "厳密"}, {"verytight", "さらに厳密"}};
    if (key == "opt_convergence")
        return {{"default", "標準"}, {"tight", "厳密"}, {"loose", "緩い"}};
    if (key == "grid")
        return {{"fine", "fine"}, {"xfine", "xfine"}};
    if (key == "solvent")
        return {{"none", "気相"}, {"cosmo", "COSMO（誘電率指定）"}};
    if (key == "geometry_units")
        return {{"angstrom", "Å"}, {"bohr", "Bohr"}};
    return {};
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
    return {{"HF / STO-3G・エネルギー", nwchemPresetSettings(hf)},
            {"B3LYP-D3(BJ) / def2-SVP・最適化＋振動数", nwchemPresetSettings(dft)},
            {"MP2 / cc-pVDZ・エネルギー（内殻凍結）", nwchemPresetSettings(mp2)}};
}
QJsonObject nwchemPresetSettings(const QJsonObject &stored) {
    const auto settings = withBasisFunctions(stored);
    QJsonObject preset;
    const auto defaults = nwchemDefaults();
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        if (!QStringList{"start", "title", "memory_mb", "geometry_units"}.contains(it.key()))
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
    QString label = settings["basis"].toString();
    const auto overrides = settings["basis_overrides"].toObject();
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        label += "、" + it.key() + ": " + it.value().toString();
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
    if (s["numerical_frequencies"].toBool() || s["method"] == "mp2" || s["reference"] == "rohf" ||
        (s["method"] == "dft" && f && f->numericalHessian) ||
        (s["reference"] == "auto" && s["method"] == "hf" && d.job()["multiplicity"].toInt(1) > 1) ||
        s["solvent"] == "cosmo")
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
        errors << "NWChem設定はオブジェクトである必要があります。";
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto value = s[it.key()];
        if (value.type() != it.value().type())
            errors << QString("設定 %1 の型が不正です。").arg(it.key());
        const auto choices = nwchemChoices(it.key());
        if (!choices.isEmpty() && std::none_of(choices.begin(), choices.end(),
                                               [&](const auto &c) { return c.value == value.toString(); }))
            errors << QString("設定 %1 の値「%2」は未対応です。").arg(it.key(), value.toString());
    }
    auto integer = [&](const QJsonObject &object, const char *key, int minimum, int maximum) {
        const auto value = object[key];
        double n = value.toDouble();
        if (!value.isDouble() || !std::isfinite(n) || n < minimum || n > maximum || std::floor(n) != n)
            errors << QString("%1 は %2〜%3 の整数で指定してください。").arg(key).arg(minimum).arg(maximum);
    };
    integer(s, "memory_mb", 64, 1048576);
    integer(s, "scf_maxiter", 1, 10000);
    integer(s, "opt_maxiter", 1, 10000);
    integer(s, "cc_maxiter", 1, 10000);
    integer(s, "tddft_roots", 1, 100);
    integer(d.job(), "charge", -1000, 1000);
    integer(d.job(), "multiplicity", 1, 1000);
    if (!QRegularExpression("^[A-Za-z][A-Za-z0-9_-]{0,63}$").match(str("start")).hasMatch())
        errors << "計算名は英字で始まる英数字・_・- の64文字以内で指定してください。";
    if (str("title").size() > 120 || str("title").contains(QRegularExpression("[\\x00-\\x1f\\x7f\"\\\\;#]")))
        errors << "タイトルは120文字以内で、改行・引用符・\\・;・#を含めず指定してください。";
    double dielectric = s["dielectric"].toDouble();
    if (!std::isfinite(dielectric) || dielectric < 1 || dielectric > 1000)
        errors << "誘電率は1〜1000で指定してください。";
    const auto method = str("method"), task = str("task"), basis = str("basis");
    const int mult = d.job()["multiplicity"].toInt(1);
    const int charge = d.job()["charge"].toInt();
    int realAtoms = 0, fixed = 0, ghosts = 0, movable = 0;
    QSet<QString> unsupported, ecpLibraries;
    const auto overrides = s["basis_overrides"].toObject();
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        bool known = true;
        try {
            element(it.key());
        } catch (const std::exception &) {
            known = false;
        }
        if (!known || !libraryBasis(it.value().toString()))
            errors << QString("元素別の基底「%1: %2」は未対応です。").arg(it.key(), it.value().toString());
    }
    for (const auto &a : d.atoms) {
        int z;
        try {
            z = element(a.element).number;
        } catch (const std::exception &) {
            continue; // Document validation already reports unknown elements.
        }
        const auto own = elementBasis(s, a.element);
        if (!basisCovers(own, z))
            unsupported.insert(own == basis ? a.element : a.element + "（" + own + "）");
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
        errors << QString("基底 %1 の対応元素に含まれません: %2").arg(basis, sorted(unsupported).join(", "));
    if (!realAtoms)
        errors << "実原子を追加してください（ゴーストのみの計算はできません）。";
    result.electrons -= charge;
    if (correlated(method) && advanced.contains("corr_freeze"))
        result.frozenOrbitals = advanced["corr_freeze"].toInt();
    if (result.electrons < 1 || mult > result.electrons + 1 || (result.electrons - mult + 1) % 2 != 0)
        errors << QString("電子数 %1 と多重度 %2 が整合しません。全電荷または多重度を変更してください。")
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
        errors << "RHF / RKSは多重度1で使用してください。開殻には自動または非制限を選択してください。";
    if ((method == "dft" || method == "mp2") && result.reference == "rohf")
        errors << "このタブのDFT・MP2はROHF参照に対応しません。自動または非制限を選択してください。";
    if (method.startsWith("ccsd") && (result.reference != "rhf" || mult != 1 || task != "energy"))
        errors << "CCSD / CCSD(T)は閉殻RHFの一点エネルギーに対応します。";
    if (method == "dft") {
        const auto *f = functional(str("functional"));
        const auto custom = str("xc_custom").simplified();
        if (f && QString(f->value) == "custom") {
            // One XC line inside the DFT block: names, numbers and signs only.
            if (custom.isEmpty() || custom.size() > 200 ||
                str("xc_custom").contains(QRegularExpression("[\\x00-\\x1f\\x7f]")) ||
                !QRegularExpression("^[A-Za-z0-9_.+\\- ]+$").match(custom).hasMatch())
                errors << "任意のxcは英数字・_・.・+・- と空白の200文字以内で指定してください（例: "
                          "xpbe96 0.75 cpbe96 1.0 hfexch 0.25）。";
            if (str("dispersion") != "none")
                errors << "任意のxcでは分散補正を選べません。NWChemにパラメータがない場合があるためです。";
        } else if (f && ((str("dispersion") == "d3" && !f->d3) || (str("dispersion") == "d3bj" && !f->d3bj)))
            errors << QString("%1 の%2はNWChemの公表パラメータと一致しないか、パラメータがありません。%3")
                          .arg(f->label, str("dispersion") == "d3" ? "D3" : "D3(BJ)",
                               QString(f->value) == "wb97x-d3"
                                   ? "ωB97X-D3はD3を含むため、分散補正は「なし」にしてください。"
                                   : QString("%1を選んでください。")
                                         .arg(f->d3     ? "D3または補正なし"
                                              : f->d3bj ? "D3(BJ)または補正なし"
                                                        : "補正なし"));
    }
    if (s["tddft"].toBool()) {
        const auto *f = functional(str("functional"));
        if (method != "dft")
            errors << "TDDFTはDFTで使用してください。";
        else if (f && f->metaGga)
            errors << QString("NWChemのTDDFTはメタGGA（%1）に対応しません。B3LYP・PBE0・CAM-"
                              "B3LYP等を選んでください。")
                          .arg(f->label);
        if (task != "energy")
            errors << "TDDFTは計算目的「一点エネルギー」で使用してください（励起状態の構造最適化は未対応）。";
    }
    if (correlated(method) && result.electrons < 2)
        errors << "相関計算には2個以上の電子が必要です。";
    if (result.frozenOrbitals &&
        (result.frozenOrbitals > result.beta || result.electrons - 2 * result.frozenOrbitals < 2))
        errors << "内殻凍結後の相関電子が不足します。内殻凍結を解除してください。";
    if (correlated(method) && (s["freeze_core"].toBool() || advanced.contains("corr_freeze")))
        warnings << QString("内殻凍結: %1 軌道。%2")
                        .arg(result.frozenOrbitals)
                        .arg(advanced.contains("corr_freeze")
                                 ? "詳細設定の明示的な軌道数を使用します。"
                                 : "ECP中心とゴーストはfreeze atomicの対象外です。");
    if (str("solvent") == "cosmo" && method != "hf" && method != "dft")
        errors << "COSMOはHFまたはDFTで使用してください。";
    if (str("solvent") == "cosmo" && ghosts)
        errors << "ゴーストを含むCOSMO入力は未対応です。";
    if (str("solvent") == "cosmo" && method == "hf" && result.reference == "uhf")
        errors << "NWChemのCOSMOはUHFに対応しません。参照を自動（ROHF）にするか、DFTを選択してください。";
    if (frequencyTask(task) && (ghosts || fixed))
        errors
            << "固定原子・ゴーストを含む振動数解析は未対応です。固定を解除し、実原子のみで設定してください。";
    if (frequencyTask(task) && realAtoms < 2)
        errors << "振動数解析には2個以上の実原子が必要です。";
    if (optimizationTask(task) && realAtoms && !movable)
        errors << "全原子が固定されています。最適化する原子の固定を解除してください。";
    if (optimizationTask(task) && ghosts)
        warnings << "ゴースト中心は核を持たないため、最適化中は固定します（constraints の fix atom）。";
    const bool points = !d.data["point_charges"].toArray().isEmpty();
    if (frequencyTask(task) && points && s["include_point_charges"].toBool())
        errors << "外部点電荷中の振動数解析は未対応です。";
    result.numericalFrequencies = nwchemNumericalFrequencies(d);
    if (frequencyTask(task) && result.numericalFrequencies)
        warnings << "振動数には数値Hessianを使用します。複数回の勾配計算を行います。";
    if (task == "frequencies")
        warnings << "現在の座標で振動数を計算します。極小構造の確認には先に最適化してください。";
    if (result.ecpElectrons)
        warnings << QString("ECP（%1）を自動指定します（除去する内殻電子: %2）。")
                        .arg(sorted(ecpLibraries).join("・"))
                        .arg(result.ecpElectrons);
    QSet<bool> conventions{cartesianByConvention(basis)};
    for (const auto &a : d.atoms)
        conventions.insert(cartesianByConvention(elementBasis(s, a.element)));
    if (conventions.size() > 1)
        warnings << "元素別の基底に、共通の基底とd関数の慣例（Cartesian / 球面）が異なるものがあります。"
                    "basis指示は全元素に共通のため、d・f関数の形式を確認してください。";
    const auto functions = str("basis_functions");
    if (functions != "auto" && basis != "sto-3g" &&
        (functions == "cartesian") != cartesianByConvention(basis))
        warnings << QString(
                        "基底 %1 "
                        "は通常%2で使用します。文献値と比べる場合はd・f関数の形式を「自動」にしてください。")
                        .arg(basis, cartesianByConvention(basis) ? "Cartesian（6d）" : "球面調和関数（5d）");
    if (s["tddft"].toBool() && method == "dft" && result.reference == "uhf" && s["tddft_triplets"].toBool())
        warnings << "開殻（UKS）のTDDFTは一重項・三重項を分けずに非制限の励起状態を求めます。";
    if (frequencyTask(task))
        if (const int sigma = rotationalSymmetryNumber(d); sigma > 1)
            warnings << QString(
                            "回転対称数σ=%1と推定されます。noautosymのためNWChemの熱化学はσ=1で計算されます。"
                            "エントロピーを R ln σ = %2 cal/(mol·K) 小さく、自由エネルギーを RT ln σ = %3 "
                            "kcal/mol（298.15 K）大きく補正してください。")
                            .arg(sigma)
                            .arg(1.987204 * std::log(sigma), 0, 'f', 3)
                            .arg(0.0019872043 * 298.15 * std::log(sigma), 0, 'f', 3);
    if (charge != d.formalCharge())
        warnings << QString("全電荷 %1 は原子の形式電荷の合計 %2 と異なります。")
                        .arg(charge)
                        .arg(d.formalCharge());
    if (points && !s["include_point_charges"].toBool())
        warnings << "保存された点電荷は入力に含めません。必要なら外部点電荷を有効にしてください。";
    if (!d.data["references"].toArray().isEmpty() || !d.data["invalid_settings"].toArray().isEmpty() ||
        !d.data["assignments"].toObject().isEmpty() || !d.job()["sections"].toObject().isEmpty())
        warnings << "KAGAMI由来の詳細・拘束・断片設定は変換しません。NWChem計算条件と原子の固定フラグを使用し"
                    "ます。";
    try {
        if (!d.atoms.isEmpty())
            nwchemGeometry(d,
                           CoordinateUnit::Angstrom); // Also validate isotope masses before enabling export.
    } catch (const std::exception &e) {
        errors << QString::fromUtf8(e.what());
    }
    validateNwchemAdvanced(d, result);
    errors.removeDuplicates();
    return result;
}
QString nwchemInput(const Document &d) {
    const auto check = validateNwchem(d);
    if (!check.errors.isEmpty())
        fail(check.errors.join('\n'));
    const auto s = nwchemSettings(d);
    const auto advanced = nwchemAdvanced(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    auto integer = [&](const char *key) { return QString::number(s[key].toInt()); };
    const auto method = str("method"), task = str("task"), basis = str("basis");
    QString text = "# Generated by MolForma for NWChem\nstart " + str("start") + '\n';
    if (!str("title").isEmpty())
        text += "title \"" + str("title") + "\"\n";
    text += "memory total " + integer("memory_mb") + " mb\n";
    text += nwchemAdvancedLines(d, "files");
    text += "charge " + QString::number(d.job()["charge"].toInt()) + "\n\n";
    Document geometry = d;
    if (optimizationTask(task))
        for (auto &a : geometry.atoms)
            a.fixed |= a.ghost; // Ghost centers carry no nucleus; the driver would pull them around.
    text += nwchemGeometry(
        geometry, str("geometry_units") == "bohr" ? CoordinateUnit::Bohr : CoordinateUnit::Angstrom, false);
    const bool spherical = str("basis_functions") == "spherical" ||
                           (str("basis_functions") == "auto" && !cartesianByConvention(basis));
    text += spherical ? "\nbasis spherical\n" : "\nbasis cartesian\n";
    QSet<QString> real, ghosts, ecps;
    for (const auto &a : d.atoms) {
        (a.ghost ? ghosts : real).insert(a.element);
        if (!a.ghost && ecpCore(elementBasis(s, a.element), element(a.element).number))
            ecps.insert(a.element);
    }
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
    if (!ecps.isEmpty()) {
        text += "\necp\n";
        for (const auto &symbol : sorted(ecps))
            text += QString("  %1 library \"%2\"\n").arg(symbol, libraryBasis(elementBasis(s, symbol))->ecp);
        text += "end\n";
    }
    if (s["include_point_charges"].toBool() && !d.data["point_charges"].toArray().isEmpty()) {
        // Point-charge coordinates always remain in angstrom, independently of GEOMETRY units.
        text += "\nbq units angstrom\n";
        for (const auto &value : d.data["point_charges"].toArray()) {
            const auto p = value.toObject();
            const auto xyz = vectorFromJson(p["position"]);
            text +=
                QString("  %1 %2 %3 %4\n")
                    .arg(number(xyz.x()), number(xyz.y()), number(xyz.z()), number(p["charge"].toDouble()));
        }
        text += "end\n";
    }
    const QString threshold = str("scf_convergence") == "normal"  ? "1e-5"
                              : str("scf_convergence") == "tight" ? "1e-7"
                                                                  : "1e-9";
    if (method == "dft") {
        const auto *f = functional(str("functional"));
        text += "\ndft\n  xc " +
                (QString(f->value) == "custom" ? str("xc_custom").simplified() : QString(f->xc)) + '\n';
        if (*f->extra)
            text += QString("  ") + f->extra + '\n';
        text += "  mult " + QString::number(d.job()["multiplicity"].toInt()) + '\n';
        if (check.reference == "uhf")
            text += "  odft\n";
        text += "  iterations " + integer("scf_maxiter") + "\n  convergence energy " +
                nwchemAdvancedNumber(d, "dft_energy", threshold) + " density " +
                nwchemAdvancedNumber(d, "dft_density", threshold) + " gradient " +
                nwchemAdvancedNumber(d, "dft_gradient", threshold) + "\n  grid " + str("grid") +
                (advanced["dft_grid_nodisk"].toBool() ? " nodisk" : "") + '\n';
        text += nwchemAdvancedLines(d, "dft") + nwchemAdvancedLines(d, "vectors");
        if (str("dispersion") != "none")
            text += str("dispersion") == "d3" ? "  disp vdw 3\n" : "  disp vdw 4\n";
        text += "end\n";
    } else {
        text += "\nscf\n  " + check.reference + "\n  nopen " +
                QString::number(d.job()["multiplicity"].toInt() - 1) + "\n  maxiter " +
                integer("scf_maxiter") + "\n  thresh " + nwchemAdvancedNumber(d, "scf_thresh", threshold) +
                '\n';
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
    if (str("solvent") == "cosmo")
        text += "\ncosmo\n  dielec " + number(s["dielectric"].toDouble()) + '\n' +
                nwchemAdvancedLines(d, "cosmo") + "end\n";
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
    if (optimizationTask(task))
        text += "\ndriver\n  maxiter " + integer("opt_maxiter") + "\n  " + str("opt_convergence") + '\n' +
                nwchemAdvancedLines(d, "driver") + "end\n";
    if (frequencyTask(task)) {
        const auto lines = nwchemAdvancedLines(d, "frequency");
        if (!lines.isEmpty())
            text += "\nfreq\n" + lines + "end\n";
        if (!check.numericalFrequencies && advanced.contains("hessian_thresh"))
            text += "\nhessian\n  thresh " + nwchemAdvancedNumber(d, "hessian_thresh", "1e-6") + "\nend\n";
    }
    const QString theory = method == "hf" ? "scf" : method;
    text += '\n';
    if (optimizationTask(task))
        text += "task " + theory + " optimize\n";
    if (frequencyTask(task))
        text += "task " + theory + " frequencies" + (check.numericalFrequencies ? " numerical" : "") + '\n';
    else if (method == "dft" && s["tddft"].toBool()) {
        text += "task tddft energy\n";
    } else if (!optimizationTask(task))
        text += "task " + theory + ' ' + task + '\n';
    return text + nwchemOutputLines(d, check);
}
} // namespace mf
