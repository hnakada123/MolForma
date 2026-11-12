// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_validation.h"
#include "foundation/i18n.h"
#include "model/geometry.h"
#include "model/memo.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_catalog.h"
#include "nwchem/nwchem_geometry.h"
#include "nwchem/nwchem_import.h"
#include "nwchem/nwchem_output.h"
#include "nwchem/nwchem_paths.h"
#include "nwchem/nwchem_plan.h"
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
namespace mf {
namespace {
void realRangeError(const NwchemSettingSpec &spec, ValidationReport &report) {
    // Preserve the stable codes and messages of the original checks. The bounds themselves always
    // come from the specifications, including for settings added in the future.
    struct Message {
        const char *key, *code, *text;
    };
    static const Message messages[] = {
        {"dielectric", "dielectric.range", QT_TR_NOOP("誘電率は1〜1000で指定してください。")},
        {"irc_stride", "irc.stride", QT_TR_NOOP("IRCの刻み幅は0.01〜1で指定してください。")},
        {"neb_spring", "neb.spring", QT_TR_NOOP("NEBのばね定数は0.001〜100で指定してください。")},
        {"neb_stepsize", "neb.stepsize", QT_TR_NOOP("NEBの刻み幅は0.001〜10で指定してください。")}};
    for (const auto &message : messages)
        if (spec.key == message.key) {
            report.error(message.code, spec.key, translated(message.text), {}, spec.target);
            return;
        }
    report.error(
        "settings.real", spec.key,
        tr("%1 は %2〜%3 の有限の数値で指定してください。").arg(spec.key).arg(spec.minimum).arg(spec.maximum),
        {}, spec.target);
}
} // namespace
void validateNwchemSettingValues(const Document &document, const QJsonObject &settings,
                                 ValidationReport &report) {
    if (document.data.contains("nwchem") && !document.data["nwchem"].isObject())
        report.error("settings.object", "nwchem", tr("NWChem設定はオブジェクトである必要があります。"));
    for (const auto &spec : nwchemSettingSpecs()) {
        const auto value = nwchemSettingValue(document, settings, spec.key);
        // Each error names the owner of its setting (the memory is the execution's).
        if (value.type() != spec.initial.type())
            report.error("settings.type", spec.key, tr("設定 %1 の型が不正です。").arg(spec.key), {},
                         spec.target);
        if (!spec.choices.isEmpty() &&
            std::none_of(spec.choices.begin(), spec.choices.end(),
                         [&](const auto &c) { return value.toString() == c.value; }))
            report.error("settings.choice", spec.key,
                         tr("設定 %1 の値「%2」は未対応です。").arg(spec.key, value.toString()), {},
                         spec.target);
        const bool integer = spec.kind == NwchemSettingKind::Integer;
        if ((!integer && spec.kind != NwchemSettingKind::Real) ||
            (!spec.validateUnused && !nwchemSettingApplies(spec, settings)))
            continue;
        const double n = value.toDouble();
        if (value.isDouble() && std::isfinite(n) && n >= spec.minimum && n <= spec.maximum &&
            (!integer || std::floor(n) == n))
            continue;
        if (integer)
            report.error("settings.integer", spec.key,
                         tr("%1 は %2〜%3 の整数で指定してください。")
                             .arg(spec.key)
                             .arg(spec.minimum)
                             .arg(spec.maximum),
                         {}, spec.target);
        else
            realRangeError(spec, report);
    }
}
using namespace nwchem_detail;
namespace {
QStringList sorted(const QSet<QString> &set) {
    auto list = set.values();
    std::sort(list.begin(), list.end());
    return list;
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
void validatePropertySettings(const Document &d, const QJsonObject &s, int ghosts, NwchemValidation &result) {
    const NwchemCalculation calc(s);
    const int mult = d.multiplicity;
    const auto str = [&](const char *key) { return s[key].toString(); };
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
            result.warning("nmr.reference", "prop_nmr",
                           tr("NMR遮蔽定数（GIAO）を計算します。化学シフトには同じ条件の基準物質（TMS等）"
                              "の値との差を"
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
}
void validatePathSettings(const Document &d, const QJsonObject &s, const QJsonObject &advanced, int realAtoms,
                          int fixed, int ghosts, NwchemValidation &result) {
    const NwchemCalculation calc(s);
    const auto str = [&](const char *key) { return s[key].toString(); };
    const auto &errors = result.errors;
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
            result.warning("constraints.frequencies", "constraints",
                           tr("拘束した構造は停留点ではないため、振動数には虚振動や大きな誤差が出ることが"
                              "あります。"));
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
                            fail(tr("座標1と2が同じ原子を動かすため、両方の値にできない点があります（点 "
                                    "%1, "
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
            result.error("scan.fixed", "scan_relaxed",
                         tr("緩和スキャンは固定原子・ゴーストと併用できません。固定を解除するか、固定スキ"
                            "ャンにして"
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
    if (s["tddft"].toBool() && calc.path())
        result.error("tddft.path", "tddft", tr("TDDFTはスキャン・IRC・NEBと併用できません。"));
}
} // namespace
static NwchemValidation validation(const Document &d);
NwchemValidation validateNwchem(const Document &d) {
    thread_local Memo<Document, NwchemValidation, 8> memo;
    return memo.get(d, [&] { return validation(d); });
}
static NwchemValidation validation(const Document &d) {
    NwchemValidation result;
    for (const auto &message : d.validate())
        result.error("geometry.validation", {}, message, {}, SettingTarget::Molecule);
    auto &errors = result.errors;
    const auto s = nwchemSettings(d);
    const auto advanced = nwchemAdvanced(d);
    auto str = [&](const char *key) { return s[key].toString(); };
    validateNwchemSettingValues(d, s, result);
    if (!QRegularExpression("^[A-Za-z][A-Za-z0-9_-]{0,63}$").match(str("start")).hasMatch())
        result.error("start.syntax", "start",
                     tr("計算名は英字で始まる英数字・_・- の64文字以内で指定してください。"));
    if (str("title").size() > 120 || str("title").contains(QRegularExpression("[\\x00-\\x1f\\x7f\"\\\\;#]")))
        result.error("title.syntax", "title",
                     tr("タイトルは120文字以内で、改行・引用符・\\・;・#を含めず指定してください。"));
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
                     tr("実原子を追加してください（ゴーストのみの計算はできません）。"), {},
                     SettingTarget::Molecule);
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
    validatePathSettings(d, s, advanced, realAtoms, fixed, ghosts, result);
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
    validatePropertySettings(d, s, ghosts, result);
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
        result.error("geometry.invalid", QString(), QString::fromUtf8(e.what()), {}, SettingTarget::Molecule);
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
                const auto tasks = nwchemTaskPlan(plain).commands();
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
} // namespace mf
