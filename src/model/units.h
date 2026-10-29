#pragma once
namespace mf {
inline constexpr double BohrToAngstrom = 0.529177210903;
enum class CoordinateUnit { Angstrom, Bohr };
inline double coordinateScale(CoordinateUnit unit) {
    return unit == CoordinateUnit::Bohr ? BohrToAngstrom : 1.;
}
} // namespace mf
