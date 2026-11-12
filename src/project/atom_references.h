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
// The settings of "nwchem" that the two functions adapt, in their order here: every setting that holds
// atom IDs. A setting that is not listed keeps the IDs of removed and renamed atoms, so the
// specifications of the settings mark the same ones (NwchemSettingSpec::atomReferences; a test
// compares the two, since this layer knows no calculation code).
QStringList nwchemAtomReferenceKeys();
} // namespace mf::project
