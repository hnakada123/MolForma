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
    // A continued restart: how many of the generated tasks (those after `before`) the other job finished,
    // which the input leaves out. The tasks of the additional input (`before`, `after`) are the user's
    // text: they stay in the input and run again.
    int finished = 0;
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
