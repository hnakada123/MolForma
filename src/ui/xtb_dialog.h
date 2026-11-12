// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/xtb.h"
#include <optional>
class QWidget;
namespace mf {
class EditorContext;
class UiServices;
// "xTBで構造を最適化": asks for the method, the convergence, the solvent and whether only the selected
// atoms move, and returns what was chosen (nothing when cancelled). The program is the one of Preferences
// ("xtb/executable"), named in the dialog; without it there is nothing to accept. Fails while there is a
// candidate or a chemistry job. The caller runs the optimization.
std::optional<XtbOptions> chooseXtbOptions(QWidget *parent, const EditorContext &context,
                                           const UiServices &services);
} // namespace mf
