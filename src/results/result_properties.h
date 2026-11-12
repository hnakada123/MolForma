// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <optional>

namespace mf {
// Typed readers of the facts in ResultData::properties that more than one file reads. The keys and
// their JSON shapes stay with the readers that write them (results.h lists them): other code reads
// such a fact here, not by its key. The points of a path come from result_path.h; the tables that one
// view formats (charges, shieldings, spectra, bond indices) are read where they are shown.

// Which energy of a log is that of its calculation.
enum class ResultEnergySource {
    Final,              // That of the last task with one.
    FrequencyReference, // That of the reference structure of a frequency task.
    BsseCorrected       // The counterpoise-corrected one.
};
// The energy of the calculation ("final_energy", see NwchemEnergySummary::energy).
struct ResultFinalEnergy {
    double hartree = 0;
    QString method; // As the log names it ("DFT", "MP2").
    ResultEnergySource source = ResultEnergySource::Final;
    QJsonObject stored; // As read, for records that keep it whole (energyRecord).
};
// Nothing for a result without one: a file that is no log, or a log without an energy.
std::optional<ResultFinalEnergy> resultFinalEnergy(const ResultData &data);

// Whether the last optimization of a log printed its convergence ("optimization"); nothing for a
// result without an optimization.
std::optional<bool> resultOptimizationConverged(const ResultData &data);

// The dipole moment a log printed last ("dipole"): the vector in a.u., its length in Debye and the
// structure it belongs to as XYZ (empty when the log had printed none).
struct ResultDipole {
    Vec au = Vec::Zero();
    double debye = 0;
    QString geometry;
};
std::optional<ResultDipole> resultDipole(const ResultData &data);
} // namespace mf
