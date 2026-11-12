// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/setting_target.h"
#include "nwchem/run_record.h"
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
// Version-1 job records written before semantic targets name the owner by its legacy number
// (RunDiagnostic::owner).
inline int runDiagnosticTab(const RunDiagnostic &diagnostic) {
    return workspaceTab(diagnostic.owner());
}
} // namespace mf
