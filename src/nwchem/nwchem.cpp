// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem.h"
#include "editing/coordinates.h"
#include "editing/site.h"
#include "foundation/i18n.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_catalog.h"
#include "nwchem/nwchem_paths.h"
#include "nwchem/nwchem_plan.h"
#include <cmath>
namespace mf {
using namespace nwchem_detail;
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
QJsonObject withBasisFunctions(QJsonObject settings) {
    if (!settings.contains("basis_functions") && settings["spherical"].isBool())
        settings["basis_functions"] = settings["spherical"].toBool() ? "spherical" : "cartesian";
    settings.remove("spherical");
    return settings;
}
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
} // namespace
QString nwchemScfThreshold(const QJsonObject &s) {
    const auto convergence = s["scf_convergence"].toString();
    return convergence == "default"  ? QString()
           : convergence == "normal" ? "1e-5"
           : convergence == "tight"  ? "1e-7"
                                     : "1e-9";
}
QString nwchemSectionKey(NwchemSection section) {
    switch (section) {
    case NwchemSection::Path:
        return "path";
    case NwchemSection::Constraints:
        return "constraints";
    case NwchemSection::Optimization:
        return "optimization";
    case NwchemSection::Followup:
        return "followup";
    case NwchemSection::Bsse:
        return "bsse";
    case NwchemSection::Excited:
        return "excited";
    }
    return {};
}
std::optional<NwchemSection> nwchemSectionFromKey(const QString &key) {
    for (const auto section : {NwchemSection::Path, NwchemSection::Constraints, NwchemSection::Optimization,
                               NwchemSection::Followup, NwchemSection::Bsse, NwchemSection::Excited})
        if (nwchemSectionKey(section) == key)
            return section;
    return std::nullopt;
}
NwchemSectionUse nwchemSectionUse(const Document &d, NwchemSection section) {
    // As the scopes of the section's settings: a section takes part when one of them does.
    const auto s = nwchemSettings(d);
    const auto applies = [&](NwchemScope scope) { return nwchemScopeApplies(scope, s, &d); };
    // Used, or not used for a reason (none: there is no more to say).
    const auto use = [](bool used, const QString &reason = {}) {
        return NwchemSectionUse{used, used ? QString() : reason};
    };
    switch (section) {
    case NwchemSection::Path:
        return use(applies(NwchemScope::Irc) || applies(NwchemScope::Scan) || applies(NwchemScope::Neb));
    case NwchemSection::Constraints:
        return use(applies(NwchemScope::Driver), tr("最適化のみ"));
    case NwchemSection::Optimization:
        return use(applies(NwchemScope::DriverOrIrc) || applies(NwchemScope::Frequency));
    case NwchemSection::Followup:
        return use(applies(NwchemScope::Optimization), tr("最適化のみ"));
    case NwchemSection::Bsse:
        return use(bsseMonomers(d).size() >= 2, tr("単量体なし"));
    case NwchemSection::Excited:
        return !NwchemCalculation(s).dft() ? use(false, tr("DFTのみ"))
                                           : use(applies(NwchemScope::ExcitedToggle), tr("一点・最適化"));
    }
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
    // Atom IDs: the settings that the project adapter follows through removals and renamings.
    const auto atoms = [](NwchemSettingSpec spec) {
        spec.atomReferences = true;
        spec.preset = false;
        return spec;
    };
    // Ranges of path settings are checked when the path is used; type checks always apply.
    const auto whenUsed = [](NwchemSettingSpec spec) {
        spec.validateUnused = false;
        return spec;
    };
    // Can still be switched off when the calculation does not use it.
    const auto disableable = [](NwchemSettingSpec spec) {
        spec.allowDisable = true;
        return spec;
    };
    // Set with the resources of a run, not with the calculation.
    const auto execution = [](NwchemSettingSpec spec) {
        spec.target = SettingTarget::Execution;
        return spec;
    };
    static const QVector<NwchemSettingSpec> specs = [=] {
        QVector<NwchemSettingSpec> list{
            project({"start", Kind::String, "molforma"}),
            project({"title", Kind::String, "MolForma calculation"}),
            execution(project({"memory_mb", Kind::Integer, 1024, 64, 1048576})),
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
            atoms({"fragment_guess", Kind::Array, QJsonArray()}),
            disableable({"tddft", Kind::Flag, false, 0, 0, 0, Scope::ExcitedToggle}),
            {"tddft_roots", Kind::Integer, 10, 1, 100, 0, Scope::Excited},
            {"tddft_triplets", Kind::Flag, false, 0, 0, 0, Scope::Excited},
            {"tddft_tda", Kind::Flag, false, 0, 0, 0, Scope::Excited},
            {"tddft_cdspectrum", Kind::Flag, false, 0, 0, 0, Scope::Excited},
            {"tddft_root", Kind::Integer, 1, 1, 100, 0, Scope::ExcitedOptimization},
            atoms({"constraints", Kind::Array, QJsonArray(), 0, 0, 0, Scope::Driver}),
            atoms({"scan_atoms", Kind::Array, QJsonArray(), 0, 0, 0, Scope::Scan}),
            whenUsed(project({"scan_start", Kind::Real, 1.0, -180, 180, 4, Scope::Scan})),
            whenUsed(project({"scan_end", Kind::Real, 2.0, -180, 180, 4, Scope::Scan})),
            // Checked only for scans: a range of points that other tasks do not use.
            whenUsed({"scan_steps", Kind::Integer, 6, 2, 50, 0, Scope::Scan}),
            {"scan_relaxed", Kind::Flag, true, 0, 0, 0, Scope::Scan},
            // The second coordinate of a two-dimensional scan (none: one-dimensional).
            atoms({"scan2_atoms", Kind::Array, QJsonArray(), 0, 0, 0, Scope::Scan}),
            whenUsed(project({"scan2_start", Kind::Real, 1.0, -180, 180, 4, Scope::Scan2})),
            whenUsed(project({"scan2_end", Kind::Real, 2.0, -180, 180, 4, Scope::Scan2})),
            whenUsed({"scan2_steps", Kind::Integer, 6, 2, 50, 0, Scope::Scan2}),
            {"irc_points", Kind::Integer, 30, 1, 500, 0, Scope::Irc},
            {"irc_stride", Kind::Real, 0.1, .01, 1, 3, Scope::Irc},
            {"irc_direction", Kind::Choice, "both", 0, 0, 0, Scope::Irc},
            {"neb_beads", Kind::Integer, 10, 3, 100, 0, Scope::Neb},
            whenUsed({"neb_spring", Kind::Real, 1.0, .001, 100, 3, Scope::Neb}),
            {"neb_maxiter", Kind::Integer, 50, 1, 10000, 0, Scope::Neb},
            whenUsed({"neb_stepsize", Kind::Real, .1, .001, 10, 3, Scope::Neb}),
            // The end of the path: atom positions by atom ID, kept with the atoms of the project.
            whenUsed(atoms({"neb_end", Kind::Object, QJsonObject(), 0, 0, 0, Scope::Neb})),
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
SettingTarget nwchemSettingTarget(const QString &key) {
    for (const auto &spec : nwchemSettingSpecs())
        if (spec.key == key)
            return spec.target;
    return SettingTarget::Calculation;
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
// The point charges an edit added (the charged groups around a site) belong to the calculation.
static void includeAddedPointCharges(Document &d, qsizetype before, qsizetype after) {
    if (after > before)
        setNwchemSetting(d, "include_point_charges", true);
}
Ids cutOutNwchemSite(Document &d, const Ids &kept, const SiteCutOptions &options) {
    const auto before = d.pointCharges.size();
    const auto added = cutOutSite(d, kept, options);
    includeAddedPointCharges(d, before, d.pointCharges.size());
    return added;
}
void replaceWithNwchemSite(Document &d, Document structure, const Ids &kept, const SiteCutOptions &options) {
    const auto before = structure.pointCharges.size();
    cutOutSite(structure, kept, options);
    replaceCoordinateStructure(d, structure);
    d.charge = structure.charge;
    if (options.charges)
        d.pointCharges = structure.pointCharges;
    includeAddedPointCharges(d, before, structure.pointCharges.size());
}
void deriveNwchemCalculation(Document &d, const QString &task, const QString &suffix, bool keepLongName) {
    setNwchemSetting(d, "task", task);
    const auto name = nwchemSettings(d)["start"].toString() + suffix;
    if (!keepLongName || name.size() <= MaximumNwchemNameLength)
        setNwchemSetting(d, "start", name.left(MaximumNwchemNameLength));
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
QJsonObject nwchemFinalSettings(const Document &d) {
    return nwchemSettings(nwchemSettings(d)["followup"].toBool() ? followupDocument(d) : d);
}
} // namespace mf
