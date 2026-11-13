// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/run_diagnostics.h"
#include "foundation/i18n.h"
#include "model/setting_target.h"
#include "nwchem/run_record.h"
#include "results/nwchem_log.h"
#include <QJsonObject>

namespace mf {
QString runDiagnosisText(const QString &code) {
    if (code == "scf_convergence")
        return tr("SCF未収束の記録があります。初期構造・電荷・多重度とSCFの収束設定を確認してください。");
    if (code == "optimization_limit")
        return tr(
            "最適化の反復上限に達した記録があります。最終構造と勾配を確認し、反復数を見直してください。");
    if (code == "memory")
        return tr(
            "メモリ不足の記録があります。MPIプロセスごとのメモリと利用可能な総メモリを確認してください。");
    if (code == "basis")
        return tr(
            "基底ファイルを読めない記録があります。基底名と実行環境の基底ライブラリを確認してください。");
    if (code == "time_limit")
        return tr("制限時間で停止しました。途中の構造とログを確認し、必要なら制限時間を変更してください。");
    if (code == "launch")
        return tr(
            "プログラムを起動できませんでした。実行ファイル・MPIランチャーと実行権限を確認してください。");
    return tr("プロセスが正常終了しませんでした。標準出力と標準エラーを確認してください。");
}
QStringList runDiagnosisLines(const QJsonObject &record) {
    QStringList lines;
    for (const auto &item : RunRecord(record).diagnostics()) {
        lines << runDiagnosisText(item.code);
        if (!item.evidence.isEmpty())
            lines << item.evidence;
    }
    return lines;
}
QJsonArray diagnoseRun(RunState state, const QString &output, const QString &errors,
                       const QString &launchError) {
    QJsonArray result;
    const auto add = [&](const QString &code, const QString &setting, SettingTarget target,
                         const QString &source, const QString &evidence) {
        result.append(QJsonObject{{"code", code},
                                  {"setting", setting},
                                  {"target", settingTargetKey(target)},
                                  {"source", source},
                                  {"evidence", evidence.left(512)}});
    };
    if (state == RunState::TimedOut)
        add("time_limit", "timeout_seconds", SettingTarget::Execution, "run", {});
    if (state == RunState::Failed && !launchError.isEmpty())
        add("launch", "executable", SettingTarget::Execution, "run", launchError);
    // What NWChem's messages say went wrong (the wording is the log reader's), with the setting each
    // failure points at: the last line that reports it, of the output first.
    struct Advice {
        NwchemFailure failure;
        const char *code, *setting;
        SettingTarget target;
    };
    const Advice advices[]{
        {NwchemFailure::ScfConvergence, "scf_convergence", "scf_maxiter", SettingTarget::Calculation},
        {NwchemFailure::OptimizationLimit, "optimization_limit", "opt_maxiter", SettingTarget::Calculation},
        {NwchemFailure::Memory, "memory", "memory_mb", SettingTarget::Execution},
        {NwchemFailure::Basis, "basis", "basis", SettingTarget::Calculation}};
    const std::pair<QString, QStringList> sources[]{{"stdout", output.split('\n')},
                                                    {"stderr", errors.split('\n')}};
    for (const auto &advice : advices) {
        bool found = false;
        for (const auto &[source, lines] : sources) {
            for (auto line = lines.crbegin(); line != lines.crend(); ++line)
                if (nwchemLineReports(*line, advice.failure)) {
                    add(advice.code, advice.setting, advice.target, source, line->trimmed());
                    found = true;
                    break;
                }
            if (found)
                break;
        }
    }
    if (state == RunState::Failed && result.isEmpty())
        add("process", "", SettingTarget::Execution, "stderr", errors.trimmed().right(512));
    return result;
}
} // namespace mf
