// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace mf {
inline constexpr double BohrToAngstrom = 0.529177210903;
// Energies per Hartree, and the photon wavelength (nm) times its energy (eV).
inline constexpr double HartreeToKcal = 627.509474, HartreeToKj = 2625.499639, HartreeToEv = 27.211386245988,
                        HartreeToWavenumber = 219474.6313632, EvNanometer = 1239.84198;
// CODATA 2018 (exact SI values): the Planck constant / J s, the Boltzmann constant / J K⁻¹, the
// Avogadro constant / mol⁻¹ and the speed of light / cm s⁻¹; kJ per thermochemical kcal and Pa per atm.
inline constexpr double PlanckConstant = 6.62607015e-34, BoltzmannConstant = 1.380649e-23,
                        AvogadroConstant = 6.02214076e23, LightSpeedCm = 2.99792458e10, KcalToKj = 4.184,
                        AtmosphereToPascal = 101325;
// The gas constant / kcal mol⁻¹ K⁻¹, and hc/k: the temperature (K) of a wavenumber of 1 cm⁻¹.
inline constexpr double GasConstantKcal = BoltzmannConstant * AvogadroConstant / (1000 * KcalToKj),
                        WavenumberToKelvin = PlanckConstant * LightSpeedCm / BoltzmannConstant;
enum class CoordinateUnit { Angstrom, Bohr };
inline double coordinateScale(CoordinateUnit unit) {
    return unit == CoordinateUnit::Bohr ? BohrToAngstrom : 1.;
}
} // namespace mf
