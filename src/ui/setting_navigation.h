// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/setting_target.h"
namespace mf {
// Only the UI translates semantic owners into the current workspace order (zero-based): the window
// for the tab it shows (MainWindow::showSetting), a panel for the number it writes beside a link
// ("2 計算条件"). No tab number crosses WorkspaceActions; callers name the owner.
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
} // namespace mf
