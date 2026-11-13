// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/i18n.h"
#include "nwchem/history.h"
#include "nwchem/remote_run.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace mf {
HistoryPresence historyPresence(const HistoryEntry &e, bool owned) {
    if (!runStateActive(e.status) || owned)
        return HistoryPresence::Recorded;
    return e.orphaned ? HistoryPresence::Orphaned : HistoryPresence::Unknown;
}
QString historyStateName(const HistoryEntry &e, bool owned) {
    switch (historyPresence(e, owned)) {
    case HistoryPresence::Orphaned:
        return remoteRun(e.manifest) ? tr("未接続") : tr("プロセスなし");
    case HistoryPresence::Unknown:
        return tr("稼働未確認");
    case HistoryPresence::Recorded:
        break;
    }
    return runStateName(e.status);
}
QString historyStateGroup(const HistoryEntry &e, bool owned) {
    if (e.status == RunState::Prepared || e.status == RunState::Finished)
        return runStateKey(e.status);
    return runStateActive(e.status) && (owned || !e.orphaned) ? "active" : "stopped";
}
bool historyReference(const HistoryEntry &e) {
    return e.energy && !runStateActive(e.status);
}
HistoryDifference historyDifference(const HistoryEntry &e, const HistoryEntry &reference) {
    if (e.formula.isEmpty() || e.formula != reference.formula)
        return HistoryDifference::Formula;
    if (e.method != reference.method)
        return HistoryDifference::Method;
    if (e.basis != reference.basis)
        return HistoryDifference::Basis;
    if (e.energyMethod != reference.energyMethod)
        return HistoryDifference::EnergyKind;
    return HistoryDifference::None;
}
QString historyIncomparable(const HistoryEntry &e, const HistoryEntry &reference) {
    switch (historyDifference(e, reference)) {
    case HistoryDifference::Formula:
        return tr("組成が基準と異なる");
    case HistoryDifference::Method:
        return tr("手法が基準と異なる");
    case HistoryDifference::Basis:
        return tr("基底が基準と異なる");
    case HistoryDifference::EnergyKind:
        return tr("エネルギーの種類が基準と異なる");
    case HistoryDifference::None:
        break;
    }
    return {};
}
QString historyComparisonNote(const HistoryEntry &e, const HistoryEntry &reference) {
    if (runStateActive(e.status))
        return e.orphaned ? tr("記録の修復後に表示") : tr("終了後に表示");
    if (!e.energy)
        return tr("エネルギーの記録なし");
    return historyIncomparable(e, reference);
}
HistoryList historyList(const QVector<HistoryEntry> &entries, const HistoryQuery &query) {
    HistoryList list;
    const HistoryEntry *reference = nullptr;
    if (!query.reference.isEmpty())
        for (const auto &e : entries)
            if (e.directory == query.reference && historyReference(e)) {
                reference = &e;
                break;
            }
    for (int i = 0; i < entries.size(); ++i) {
        const auto &e = entries[i];
        const bool owned = !query.active.isEmpty() && e.directory == query.active;
        const auto words = QStringList{e.name,    e.method, e.basis,     e.task,
                                       e.formula, e.memo,   e.directory, historyStateName(e, owned)}
                               .join(' ');
        if (!words.contains(query.search, Qt::CaseInsensitive))
            continue;
        const auto group = historyStateGroup(e, owned);
        ++list.counts[group];
        ++list.counts[""];
        if (!query.state.isEmpty() && group != query.state)
            continue;
        HistoryRow row{i, owned, {}, {}};
        if (reference) {
            row.note = historyComparisonNote(e, *reference);
            if (row.note.isEmpty())
                row.delta = *e.energy - *reference->energy;
        }
        list.rows << row;
    }
    // Values first in the chosen order, then the rows without one; the newest first among equals.
    const auto missing = [&](const HistoryRow &r) {
        const auto &e = entries[r.entry];
        switch (query.sort) {
        case HistorySort::Created:
            return !e.created.isValid();
        case HistorySort::Duration:
            return e.seconds < 0;
        case HistorySort::Energy:
            return !e.energy.has_value();
        case HistorySort::Delta:
            return !r.delta.has_value();
        case HistorySort::Memo:
            return e.memo.isEmpty();
        default:
            return false;
        }
    };
    const auto compare = [&](const HistoryRow &a, const HistoryRow &b) -> int {
        const auto &x = entries[a.entry], &y = entries[b.entry];
        const auto order = [](auto u, auto v) { return u < v ? -1 : v < u ? 1 : 0; };
        switch (query.sort) {
        case HistorySort::State:
            return QString::localeAwareCompare(historyStateName(x, a.owned), historyStateName(y, b.owned));
        case HistorySort::Name:
            return QString::localeAwareCompare(x.name, y.name);
        case HistorySort::Level:
            return QString::localeAwareCompare(x.level, y.level);
        case HistorySort::Task:
            return QString::localeAwareCompare(x.task, y.task);
        case HistorySort::Created:
            return order(x.created.toMSecsSinceEpoch(), y.created.toMSecsSinceEpoch());
        case HistorySort::Duration:
            return order(x.seconds, y.seconds);
        case HistorySort::Energy:
            return order(*x.energy, *y.energy);
        case HistorySort::Delta:
            return order(*a.delta, *b.delta);
        case HistorySort::Memo:
            break;
        }
        return QString::localeAwareCompare(x.memo, y.memo);
    };
    const auto time = [](const HistoryEntry &e) {
        return e.created.isValid() ? e.created.toMSecsSinceEpoch() : std::numeric_limits<qint64>::min();
    };
    std::stable_sort(list.rows.begin(), list.rows.end(), [&](const HistoryRow &a, const HistoryRow &b) {
        const bool ma = missing(a), mb = missing(b);
        if (ma != mb)
            return mb;
        if (!ma)
            if (const int c = compare(a, b))
                return query.ascending ? c < 0 : c > 0;
        return time(entries[a.entry]) > time(entries[b.entry]);
    });
    for (const auto &r : std::as_const(list.rows))
        if (r.delta)
            list.largest = std::max(list.largest, std::abs(*r.delta));
    return list;
}
} // namespace mf
