// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem.h"

namespace mf {
// Where a task stands in the input, in the order the tasks run.
enum class NwchemTaskPhase {
    Before,     // The additional input before the generated tasks ("extra_input").
    Fragments,  // The orbitals of each fragment of an initial guess.
    Main,       // The calculation: one task, an optimization and its frequencies, or the points of a scan.
    Followup,   // The single point at another level.
    Properties, // The properties that share one property block.
    Esp,
    Grids,  // The potential and the field on a grid, a task each.
    Output, // CUBE and MOLDEN files.
    Raman,
    Ord, // A task for each wavelength.
    SpinSpin,
    After // The additional input after the generated tasks ("extra_tasks").
};
struct NwchemTaskStep {
    NwchemTaskPhase phase;
    QString command; // The task's line: "task dft optimize".
    // A continued restart leaves the task out of the input: the other job finished it.
    bool skipped = false;
    // Whether the input writer writes the task. The tasks of the additional input are the user's text:
    // they stay in the input with it, are never skipped and run again.
    bool generated() const {
        return phase != NwchemTaskPhase::Before && phase != NwchemTaskPhase::After;
    }
    bool operator==(const NwchemTaskStep &) const = default;
};
// The tasks of a calculation in the order NWChem runs them, and which of them the input leaves out.
// Validation compares the commands with a saved job; the input writer walks the generated steps with a
// NwchemTaskCursor and writes no task that is not a step.
struct NwchemTaskPlan {
    QList<NwchemTaskStep> steps;
    int frequency = -1; // Which of the Main tasks (from 0) is sourced to numerical_frequencies.
    int finished = 0;   // The number of skipped steps: the first generated ones of a continued restart.
    QStringList commands() const; // Of every step, skipped or not.
    QStringList commands(NwchemTaskPhase phase) const;
};
// Requires valid setting types/ranges; callers validate before generating or comparing a plan.
NwchemTaskPlan nwchemTaskPlan(const Document &document);
// The input writer's place among the generated steps of a plan. Every task the writer writes is taken
// here, in the plan's order; a writer that expects another phase than the plan, or leaves steps behind,
// fails instead of writing other tasks than the plan's.
class NwchemTaskCursor {
  public:
    explicit NwchemTaskCursor(NwchemTaskPlan plan);
    const NwchemTaskPlan plan;
    // Whether the next generated step is of this phase.
    bool at(NwchemTaskPhase phase) const;
    // The next generated step, which the writer expects in this phase.
    const NwchemTaskStep &take(NwchemTaskPhase phase);
    // The writer has written its last task: no generated step may be left.
    void finish() const;

  private:
    qsizetype next = 0; // In plan.steps: a generated step, or their end.
    void advance();
};
// What the input of a saved job says of its run, for the progress shown of it (a job made elsewhere has
// no plan, and the additional input of any job is the user's text): its tasks in their order, in words
// ("構造最適化", "振動数"), and the most steps its DRIVER takes (maxiter; 0 without a DRIVER block).
// Only lines as the generator writes them count: "task" and "driver" at the start of a line.
QStringList nwchemInputStages(const QString &input);
int nwchemInputDriverSteps(const QString &input);
namespace nwchem_detail {
Document followupDocument(const Document &document);
QString theoryOf(const QString &method);
} // namespace nwchem_detail
} // namespace mf
