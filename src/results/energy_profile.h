// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
namespace mf {
// An explicitly sourced electronic energy, thermal corrections and geometry. Settings are the
// resolved NWChem settings supplied by the caller (results has no dependency on the input generator).
QJsonObject energyRecord(const ResultData &result, const Document &original, const QJsonObject &settings);
// A molforma-energy-profile v1: records, stages [{name, terms:[{record, thermal, coefficient}]}],
// metric E/ZPE/H/G, temperature, unit hartree/kcal/kj, reference (stage index), standard_state label.
QJsonObject evaluateEnergyProfile(const QJsonObject &profile);
// Equal statistical weight per distinct minimum after atom-order-preserving RMSD grouping.
QJsonArray conformerPopulations(const QJsonArray &records, const QString &metric, double temperature,
                                double tolerance);
} // namespace mf
