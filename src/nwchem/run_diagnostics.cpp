// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/run_diagnostics.h"
#include "model/i18n.h"
#include "model/setting_target.h"
#include <QJsonObject>
#include <QRegularExpression>

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
    struct Pattern {
        const char *code, *setting, *expression;
        SettingTarget target;
    };
    const Pattern patterns[]{
        {"scf_convergence", "scf_maxiter",
         "(?:scf|dft).*?(?:failed to converge|convergence failure|not converged)|dft energy failed",
         SettingTarget::Calculation},
        {"optimization_limit", "opt_maxiter",
         "maximum number of (?:optimization )?steps exceeded|failed to converge in.*steps|driver:.*too many "
         "steps",
         SettingTarget::Calculation},
        {"memory", "memory_mb",
         "out of memory|insufficient memory|not enough memory|ma_alloc.*fail|failed to allocate",
         SettingTarget::Execution},
        {"basis", "basis", "failed opening basis file|error in the specified basis set",
         SettingTarget::Calculation}};
    for (const auto &pattern : patterns) {
        const QRegularExpression expression(QString::fromLatin1(pattern.expression),
                                            QRegularExpression::CaseInsensitiveOption);
        bool found = false;
        for (const auto &source :
             {qMakePair(QString("stdout"), output), qMakePair(QString("stderr"), errors)}) {
            const auto lines = source.second.split('\n');
            for (auto line = lines.crbegin(); line != lines.crend(); ++line)
                if (expression.match(*line).hasMatch()) {
                    add(pattern.code, pattern.setting, pattern.target, source.first, line->trimmed());
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
