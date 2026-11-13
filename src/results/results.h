// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <array>
#include <limits>

namespace mf {
struct SurfaceTriangle {
    Vec a, b, c;
    bool positive = true;
    // Unit normals at a, b and c from the gradient of the field, pointing out of the lobe, for
    // smooth shading; zero when unknown (the face normal is used then).
    Vec na = Vec::Zero(), nb = Vec::Zero(), nc = Vec::Zero();
};
struct ResultFrame {
    Document molecule;
    QString label;
};
struct ResultEnergy {
    QString method;
    double hartree = 0;
    int line = 0;
};
struct ResultOrbital {
    QString symmetry, spin;
    double energy = 0, occupation = 0;
    // The molecular orbital's number within its spin, counted from 1 as NWChem and MOLDEN do (a log
    // prints only some orbitals, from "Vector N").
    int number = 0;
};
struct ResultMode {
    double frequency = 0, intensity = std::numeric_limits<double>::quiet_NaN();
    QVector<Vec> displacement;
};
// One amplitude of an excited state, as NWChem prints those above 0.05: from an occupied orbital to a
// virtual one (their numbers within the spin, counted from 1 like ResultOrbital::number; a frozen core
// does not shift them). spin is "alpha" / "beta", or empty for restricted states. An X amplitude
// (excitation), or a Y one (deexcitation); Tamm–Dancoff states have X only.
struct ResultContribution {
    int from = 0, to = 0;
    QString spin;
    double coefficient = 0;
    bool deexcitation = false;
};
// One TDDFT excited state. spin is "singlet" / "triplet", or empty for unrestricted states.
struct ResultExcitation {
    static constexpr int maxContributions = 50;
    int root = 0;
    QString spin, symmetry;
    double ev = 0, oscillator = 0, s2 = std::numeric_limits<double>::quiet_NaN();
    // The largest amplitudes (at most maxContributions) in NWChem's order; none when the log ends
    // within them.
    QVector<ResultContribution> contributions = {};
};
// One DRIVER optimization step ("@" lines): energy / Hartree, gradients / Hartree·Bohr⁻¹, steps / Bohr.
struct ResultStep {
    int step = 0;
    double energy = 0, deltaE = 0, gmax = 0, grms = 0, xrms = 0, xmax = 0, seconds = 0;
    bool operator==(const ResultStep &) const = default;
};
enum class ResultOutcome { NotApplicable, Incomplete, Normal, Error };
// A bond index of this size or more (in magnitude) is listed as a bond; the reader keeps at most
// maxBondIndices atom pairs of a log.
inline constexpr double NotableBondIndex = .05;
inline constexpr int maxBondIndices = 20000;
struct ResultData {
    ResultOutcome outcome = ResultOutcome::NotApplicable;
    QString source, format, status, log;
    QStringList warnings;
    QVector<ResultFrame> frames;
    QVector<ResultEnergy> energies;
    QVector<ResultOrbital> orbitals;
    QVector<ResultMode> modes;
    QVector<ResultExcitation> excitations;
    QVector<ResultStep> steps; // Steps of the last optimization in the log.
    // What the readers summarize, by key. Every key a reader writes is listed here; a fact that more
    // than one file reads has a typed reader (result_properties.h, result_path.h for the paths).
    // Energies are in Hartree and atom numbers count from 1 unless said otherwise.
    // The calculation and its optimization:
    //   "final_energy" {hartree, method, source: final|frequency_reference|bsse_corrected}: the energy
    //     of the calculation (NwchemEnergySummary::energy, results/nwchem_log.h); absent without one.
    //   "bsse" {error, supermolecular, corrected, nwchem_error?, interaction?,
    //     interaction_uncorrected?}: the last counterpoise correction (NwchemEnergySummary::bsse).
    //   "optimization" "converged" | "not_converged": the last DRIVER optimization; absent without one.
    //   "thresholds" {gmax, grms, xmax, xrms} of that DRIVER.
    //   "thermochemistry" [{temperature, zpe_kcal, energy_kcal, enthalpy_kcal, entropy_cal, cv_cal,
    //     entropy_translational_cal, entropy_rotational_cal, entropy_vibrational_cal,
    //     nwchem_frequency_scale, symmetry_number}]: one object per temperature (K; kcal/mol and
    //     cal/(mol K)), each with the fields the log printed.
    //   "solvation" {gas, solution} of COSMO, {smd_dg, smd_total} of SMD.
    //   "spin_multiplicity" of the wavefunction, as last printed.
    // Paths (read them through result_path.h):
    //   "scan" [{point, points, coordinate, value, coordinate2?, value2?, energy, frame?}]: the points
    //     with an energy; a two-dimensional scan names its second coordinate.
    //   "neb" {points [{bead, energy, frame?, distance?}] of the last path read (distance: the length
    //     of the path up to the bead, Å; neither without the structures), iteration, steps of "@neb",
    //     thresholds, converged?}.
    //   "irc" {points [{direction: forward|backward, point, energy, s, frame}], directions [those
    //     traced], complete (each of them printed its convergence), ts_energy?, ts_frame}.
    // Properties:
    //   "dipole" {x, y, z, debye, geometry?, origin} in a.u./Debye, with the structure it belongs to
    //     as XYZ.
    //   "polarizability" {isotropic, anisotropy, frequency?, tensor [9, by rows], geometry?} in a.u.
    //   Of the Mulliken analysis: "mulliken" (charges), "valency" and "free_electrons" per atom, and
    //     "bond_indices" [{a, b, value}] with a < b: every pair, or with "bond_indices_omitted" (their
    //     number) without the pairs below NotableBondIndex of a molecule with more than
    //     maxBondIndices pairs.
    //   "esp" and "resp": the charges of the ESP module per atom ("resp" only when restrained).
    //   "shielding" [{atom, element, isotropic, anisotropy?}]: the NMR shieldings of the last
    //     calculation, ppm.
    //   "spinspin" [{a, b, isotope_a, isotope_b, hz}]: the isotropic NMR spin–spin couplings (isotopes
    //     as "13-C").
    //   "epr_gshift_ppt" [9]: the g-shift tensor; "epr_hyperfine" [{atom, element, tensor_mhz [9]}].
    //   "tddft" {alpha_electrons, beta_electrons} of the excited states' reference.
    //   "ecd" [{root, spin, ev, strength, unit, gauge, excitation}]: the rotatory strengths of the
    //     excited states (excitation: the index in excitations).
    //   "ord" [{frequency_au, wavelength_nm, isotropic, tensor [9], unit, gauge}]: optical rotation.
    //   "raman" [{raman_index, frequency, activity, mode?}] (mode: the index in modes), from a log or a
    //     Raman plot file, which also gives "raman_wavelength_nm".
    QJsonObject properties;
    Document vibrationGeometry;
    QVector<SurfaceTriangle> surface;
    QVector<int> datasets;
    std::array<int, 3> grid{};
    QVector<double> values; // Selected CUBE dataset; bounded to two million values.
    Vec gridOrigin = Vec::Zero();
    Mat gridAxes = Mat::Identity();
    int dataset = 0;
    double isovalue = .03, minimum = 0, maximum = 0;
    bool orbitalBasis = false;
    int selectedOrbital = -1, orbitalFrame = -1, orbitalDivisions = 40;
    QString orbitalError, orbitalNormalization, normalizationRequest = "auto";
};
// Parsing/meshing is bounded and runs in a cancellable native process in the UI. With a cache folder
// (the result loader's, for its session), a CUBE or MOLDEN file read before is not parsed again for
// another dataset, orbital or isovalue, nor an orbital evaluated again for another isovalue.
ResultData readResult(const QString &path, double isovalue = .03, int dataset = 0, int orbital = -1,
                      const QString &normalization = "auto", int orbitalDivisions = 40,
                      const QString &cacheFolder = {});
// A term of a combination of CUBE files: factor × a dataset of a CUBE file.
struct CubeTerm {
    QString path;
    double factor = 1;
    int dataset = 0;
};
// Writes the sum of the terms to output, a Gaussian CUBE with one dataset: a difference density
// ρ(AB) − ρ(A) − ρ(B) is +1 × AB, −1 × A, −1 × B. The files must share their grid (points, origin and
// spacing); the atoms written are those of the first. Fails otherwise, or when output is one of them.
void combineCubes(const QVector<CubeTerm> &terms, const QString &output);
void saveResultData(const QString &path, const ResultData &data);
ResultData loadResultData(const QString &path);
int resultWorker(const QString &request, const QString &response);
// The process of a result loader's worker: with the request and response paths as arguments
// (--result-worker <request> <response>), or started ahead of its request (--result-worker-ready) and
// given them as a JSON line on standard input; the end of input ends it. Its exit code, or -1 when
// argv starts neither (prepare runs once the application exists, e.g. for the translations).
int resultWorkerMain(int argc, char **argv, void (*prepare)() = nullptr);
Document resultModeGeometry(const ResultData &data, int mode, double phase, double amplitude);
// Translations and rotations among the normal modes: the 6 (linear: 5, atom: 3) modes with the
// smallest |frequency| of a complete 3N-mode set. Other mode sets are all treated as vibrations.
QVector<bool> resultRigidModes(const ResultData &data);
// How an NWChem log ended: "normal" (the termination marker), "incomplete" (no marker: running,
// stopped or cut short) or "error"; empty for other files, whose outcome is not judged.
QString resultOutcome(const ResultData &data);
} // namespace mf
