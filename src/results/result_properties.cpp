// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_properties.h"

namespace mf {
std::optional<ResultFinalEnergy> resultFinalEnergy(const ResultData &data) {
    const auto stored = data.properties.value("final_energy").toObject();
    if (stored.isEmpty())
        return std::nullopt;
    const auto source = stored["source"].toString();
    return ResultFinalEnergy{stored["hartree"].toDouble(), stored["method"].toString(),
                             source == "frequency_reference" ? ResultEnergySource::FrequencyReference
                             : source == "bsse_corrected"    ? ResultEnergySource::BsseCorrected
                                                             : ResultEnergySource::Final,
                             stored};
}
std::optional<bool> resultOptimizationConverged(const ResultData &data) {
    if (!data.properties.contains("optimization"))
        return std::nullopt;
    return data.properties.value("optimization") == "converged";
}
std::optional<ResultDipole> resultDipole(const ResultData &data) {
    const auto stored = data.properties.value("dipole").toObject();
    if (stored.isEmpty())
        return std::nullopt;
    return ResultDipole{Vec(stored["x"].toDouble(), stored["y"].toDouble(), stored["z"].toDouble()),
                        stored["debye"].toDouble(), stored["geometry"].toString()};
}
} // namespace mf
