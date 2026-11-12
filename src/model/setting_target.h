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
} // namespace mf
