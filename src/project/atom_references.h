// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QHash>
#include <QJsonObject>
#include <QStringList>

namespace mf::project {
// Adapter for atom references in the version-1 project settings. It owns the persisted field
// names and preserves unrecognized fields; neither the molecular model nor calculation code
// needs to duplicate this traversal. No dependency on Document or an execution engine.
void removeAtomReferences(QJsonObject &data, const QStringList &removed);
void renameAtomReferences(QJsonObject &data, const QHash<QString, QString> &renamed);
} // namespace mf::project
