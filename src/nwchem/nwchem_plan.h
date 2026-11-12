// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem.h"

namespace mf {
// Ordered task commands, before removing completed tasks for a restart. Validation compares
// this plan with the saved job; the input writer consumes the same commands with source locations.
struct NwchemTaskPlan {
    QStringList before, fragments, main, followup, properties, esp, grids, output, raman, ord, spinspin,
        after;
    int frequency = -1; // The main task sourced to numerical_frequencies.
    QStringList commands() const;
};
// Requires valid setting types/ranges; callers validate before generating or comparing a plan.
NwchemTaskPlan nwchemTaskPlan(const Document &document);
QStringList nwchemOutputTasks(const Document &document);
namespace nwchem_detail {
Document followupDocument(const Document &document);
QString theoryOf(const QString &method);
} // namespace nwchem_detail
} // namespace mf
