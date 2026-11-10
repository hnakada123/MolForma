// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
namespace mf {
// How the thermochemistry NWChem printed (harmonic oscillators, ideal gas at 1 atm) is corrected.
// Fields of a comparison or conformer set; a missing one is the default, which changes nothing.
struct ThermalOptions {
    // frequency_scale (0.5–1.5): every vibrational wavenumber in the zero-point energy, the thermal
    // vibrational energy and the vibrational entropy is multiplied by it.
    double scale = 1;
    // entropy_model: "rrho" (harmonic), "grimme" (quasi-RRHO of Grimme 2012: each mode's entropy is
    // w·S_HO + (1 − w)·S_free rotor, w = 1/(1 + (cutoff/ν)⁴)) or "truhlar" (wavenumbers below the
    // cutoff are raised to it); both change the vibrational entropy only.
    QString model = "rrho";
    double cutoff = 100; // entropy_cutoff / cm⁻¹ (10–500).
    // standard_state_correction "1M": 1 mol/L instead of 1 atm, G + RT·ln(R′T·c°/p°); "none" otherwise.
    bool molar = false;
    bool vibrational() const {
        return scale != 1 || model != "rrho";
    }
};
ThermalOptions thermalOptions(const QJsonObject &specification);
// The vibrational zero-point energy and thermal energy (without the zero-point energy) / kcal mol⁻¹
// and entropy / cal mol⁻¹ K⁻¹ of real wavenumbers / cm⁻¹, with CODATA 2018 constants.
struct VibrationalTerms {
    double zpe = 0, thermal = 0, entropy = 0;
};
VibrationalTerms vibrationalTerms(const QVector<double> &wavenumbers, double temperature,
                                  const ThermalOptions &options = {});
// RT·ln(R′T) / kcal mol⁻¹: what G of one mole gains from 1 atm to 1 mol/L (1.894 at 298.15 K).
double standardStateShift(double temperature);
// exp(−ΔG/RT) for ΔG / kcal mol⁻¹, and the Eyring rate constant (k_B·T/h)·exp(−ΔG‡/RT) / s⁻¹ with a
// transmission coefficient of 1.
double boltzmannFactor(double energy, double temperature);
double eyringRate(double barrier, double temperature);

// An explicitly sourced electronic energy, thermal corrections and geometry. Settings are the
// resolved NWChem settings supplied by the caller (results has no dependency on the input generator).
// "vibrations" holds the wavenumbers NWChem's thermochemistry summed over (see energyRecord).
QJsonObject energyRecord(const ResultData &result, const Document &original, const QJsonObject &settings);
// A molforma-energy-profile v1: records, stages [{name, terms:[{record, thermal, coefficient}]}],
// metric E/ZPE/H/G, temperature, unit hartree/kcal/kj, reference (stage index), standard_state label
// and the ThermalOptions fields. NWChem's printed corrections are kept and only their vibrational
// parts replaced: + [model at scale·ν] − [harmonic at ν], so the defaults return the printed values.
// With metric G each stage gets boltzmann_factor and, above the reference, rate_constant (Eyring).
QJsonObject evaluateEnergyProfile(const QJsonObject &profile);
// Equal statistical weight per distinct minimum after atom-order-preserving RMSD grouping. thermal
// holds the ThermalOptions fields for metric G.
QJsonArray conformerPopulations(const QJsonArray &records, const QString &metric, double temperature,
                                double tolerance, const QJsonObject &thermal = {});
} // namespace mf
