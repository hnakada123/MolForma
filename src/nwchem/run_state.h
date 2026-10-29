// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>

namespace mf {
// The state of an NWChem run, recorded in run.json as its key ("status"). Idle is a run that has not
// started; Invalid a record that cannot be read. Neither is ever recorded.
enum class RunState {
    Idle,
    Prepared,
    Starting,
    Running,
    Stopping,
    Finished,
    Failed,
    Cancelled,
    TimedOut,
    Interrupted,
    Invalid
};
QString runStateKey(RunState state);
// Invalid for keys that are not recorded.
RunState runStateFromKey(const QString &key);
// Starting, running or stopping: NWChem may still be running.
bool runStateActive(RunState state);
// The state in a word, as the run panel and the history show it ("実行中", "停止"); empty for Idle.
QString runStateName(RunState state);
} // namespace mf
