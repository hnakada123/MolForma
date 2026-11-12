// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ui/settings_widgets.h"
#include <functional>

namespace mf {
struct Document;
// The calculation in brief, the same card at the top of the right column of "2 計算条件" and
// "4 入力プレビュー": method / basis, the task, then composition, electrons, charge and memory.
// The memory is set in "3 実行設定": its row links there (openLink receives "memory").
class CalculationSummary : public SettingsCard {
  public:
    // prefix: of the object names ("calculation" → calculationSummary, calculationSummaryTask, …).
    CalculationSummary(const QString &prefix, const QString &caption,
                       const std::function<void(const QString &)> &openLink, QWidget *parent = nullptr);
    void update(const Document &document);

  private:
    QLabel *title, *task, *formula, *electrons, *charge, *memory;
};
// "HF / STO-3G · 一点エネルギー" (and the solvent): the one line of the calculation in the summaries
// and "3 実行設定".
QString calculationBrief(const Document &document);
} // namespace mf
