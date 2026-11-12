// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/setting_target.h"
#include <QJsonObject>
namespace mf {
// Only the UI translates semantic owners into the current workspace order (zero-based).
inline int workspaceTab(SettingTarget target) {
    switch (target) {
    case SettingTarget::Molecule:
        return 0;
    case SettingTarget::Calculation:
        return 1;
    case SettingTarget::Execution:
        return 2;
    }
    return 1;
}
inline int runDiagnosticTab(const QJsonObject &diagnostic) {
    if (const auto target = settingTargetFromKey(diagnostic["target"].toString()))
        return workspaceTab(*target);
    // Version-1 job records written before semantic targets stored the old workspace index.
    switch (diagnostic["tab"].toInt(2)) {
    case 0:
        return workspaceTab(SettingTarget::Molecule);
    case 1:
        return workspaceTab(SettingTarget::Calculation);
    default:
        return workspaceTab(SettingTarget::Execution);
    }
}
} // namespace mf
