// SPDX-License-Identifier: GPL-2.0-or-later
#include "mcp/execution_trust.h"
#include "mcp/policy.h"

namespace mf {
void ExecutionTrust::documentChanged(const Document &before, const Document &after, Origin origin) {
    // Only the values an edit made in MolForma itself changes become trusted: never those of loads,
    // undo/redo or MCP requests, nor the other values kept beside an edited one.
    if (origin != Origin::User)
        return;
    const auto old = protectedExecution(before), now = protectedExecution(after);
    QJsonObject changed;
    for (auto it = now.begin(); it != now.end(); ++it)
        if (old[it.key()] != it.value())
            changed[it.key()] = it.value();
    trust(changed);
}
void ExecutionTrust::confirm(const Document &document) {
    trust(protectedExecution(document));
}
void ExecutionTrust::trust(const QJsonObject &values) {
    for (auto it = values.begin(); it != values.end(); ++it)
        if (auto &known = trusted[it.key()]; !known.contains(it.value()))
            known.append(it.value());
}
QStringList ExecutionTrust::untrusted(const Document &document, const QJsonObject &preferences) const {
    const auto current = protectedExecution(document);
    QStringList keys;
    for (auto it = current.begin(); it != current.end(); ++it)
        if (it.value() != preferences[it.key()] && !trusted.value(it.key()).contains(it.value()))
            keys << it.key();
    return keys;
}
} // namespace mf
