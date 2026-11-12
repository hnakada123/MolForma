// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <optional>
namespace mf {
// Semantic owners of settings and molecular input, independent of workspace layout.
enum class SettingTarget { Calculation, Execution, Molecule };
inline QString settingTargetKey(SettingTarget target) {
    switch (target) {
    case SettingTarget::Molecule:
        return "molecule";
    case SettingTarget::Calculation:
        return "calculation";
    case SettingTarget::Execution:
        return "execution";
    }
    return {};
}
inline std::optional<SettingTarget> settingTargetFromKey(const QString &key) {
    for (const auto target : {SettingTarget::Molecule, SettingTarget::Calculation, SettingTarget::Execution})
        if (settingTargetKey(target) == key)
            return target;
    return std::nullopt;
}
// The numbers that stood for the owners before semantic targets: the workspace order of that time,
// which version-1 job records stored ("tab") and MCP responses still carry. They are not today's
// workspace tabs (only the UI knows those) and never change.
inline int legacySettingTab(SettingTarget target) {
    switch (target) {
    case SettingTarget::Molecule:
        return 0;
    case SettingTarget::Calculation:
        return 1;
    case SettingTarget::Execution:
        return 2;
    }
    return 2;
}
// Any other number is the execution's, as the readers of those records have taken it.
inline SettingTarget settingTargetFromLegacyTab(int tab) {
    for (const auto target : {SettingTarget::Molecule, SettingTarget::Calculation})
        if (legacySettingTab(target) == tab)
            return target;
    return SettingTarget::Execution;
}
} // namespace mf
