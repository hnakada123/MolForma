#pragma once
namespace mf {
inline constexpr double BohrToAngstrom = 0.529177210903;
// Energies per Hartree, and the photon wavelength (nm) times its energy (eV).
inline constexpr double HartreeToKcal = 627.509474, HartreeToKj = 2625.499639, HartreeToEv = 27.211386245988,
                        HartreeToWavenumber = 219474.6313632, EvNanometer = 1239.84198;
enum class CoordinateUnit { Angstrom, Bohr };
inline double coordinateScale(CoordinateUnit unit) {
    return unit == CoordinateUnit::Bohr ? BohrToAngstrom : 1.;
}
} // namespace mf
