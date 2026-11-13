// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <optional>
class QWidget;
namespace mf {
class UiServices;
// A sum of CUBE files times their factors (a difference density), and the file it is written to.
struct CubeCombination {
    QVector<CubeTerm> terms;
    QString output;
};
// "CUBEの差・和" of the result viewer: asks which CUBE files to add to the shown one (`source`, with
// its shown data set as the first term) with which factors, and where to save the sum. Replacing an
// existing file is confirmed here; empty when cancelled. Reads and writes no CUBE itself.
std::optional<CubeCombination> askCubeCombination(QWidget *parent, UiServices &uiContext,
                                                  const QString &source, int dataset);
} // namespace mf
