// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/run_state.h"
#include "model/i18n.h"
#include <utility>

namespace mf {
namespace {
constexpr std::pair<RunState, const char *> keys[]{
    {RunState::Idle, "idle"},          {RunState::Prepared, "prepared"},
    {RunState::Starting, "starting"},  {RunState::Running, "running"},
    {RunState::Stopping, "stopping"},  {RunState::Finished, "finished"},
    {RunState::Failed, "failed"},      {RunState::Cancelled, "cancelled"},
    {RunState::TimedOut, "timed_out"}, {RunState::Interrupted, "interrupted"},
    {RunState::Invalid, "invalid"}};
} // namespace
QString runStateKey(RunState state) {
    for (const auto &[value, key] : keys)
        if (value == state)
            return key;
    return "invalid";
}
RunState runStateFromKey(const QString &key) {
    for (const auto &[value, name] : keys)
        if (key == name && value != RunState::Idle)
            return value;
    return RunState::Invalid;
}
bool runStateActive(RunState state) {
    return state == RunState::Starting || state == RunState::Running || state == RunState::Stopping;
}
QString runStateName(RunState state) {
    switch (state) {
    case RunState::Idle:
        return {};
    case RunState::Prepared:
        return tr("保存のみ", "state");
    case RunState::Starting:
        return tr("起動中");
    case RunState::Running:
        return tr("実行中");
    case RunState::Stopping:
        return tr("停止処理中");
    case RunState::Finished:
        return tr("終了", "state");
    case RunState::Failed:
        return tr("失敗");
    case RunState::Cancelled:
        return tr("停止", "state");
    case RunState::TimedOut:
        return tr("時間制限", "state");
    case RunState::Interrupted:
        return tr("中断");
    case RunState::Invalid:
        return tr("読めません");
    }
    return {};
}
} // namespace mf
