// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <array>
#include <cmath>

namespace mf {
// What a loaded result means, shared by the result viewer, the run panel and MCP.

// Below this frequency (cm⁻¹) an imaginary frequency is significant; smaller negative frequencies of
// vibrations are often numerical noise of near-zero modes.
inline constexpr double SignificantImaginaryFrequency = -10;
// Translations and rotations are rigid (see resultRigidModes); a vibration with a negative frequency
// is imaginary, significantly so below SignificantImaginaryFrequency.
enum class ResultModeKind { Vibration, Rigid, Imaginary, SignificantImaginary };
QVector<ResultModeKind> resultModeKinds(const ResultData &data);
inline bool imaginaryMode(ResultModeKind kind) {
    return kind == ResultModeKind::Imaginary || kind == ResultModeKind::SignificantImaginary;
}
struct ResultModeCounts {
    int vibrations = 0, rigid = 0; // Vibrations include the imaginary ones.
    int imaginary = 0, significant = 0;
};
ResultModeCounts resultModeCounts(const QVector<ResultModeKind> &kinds);
// "虚振動なし", or "虚振動 2個（−10 cm⁻¹未満 1個）".
QString resultImaginaryText(const ResultModeCounts &counts);

// A DRIVER convergence criterion of an optimization step.
struct ResultCriterion {
    const char *key, *name; // "gmax", "Gmax"
    double value = 0, threshold = std::numeric_limits<double>::quiet_NaN();
    bool measured = true; // The displacements are not measured at step 0.
    bool judged() const {
        return measured && std::isfinite(threshold);
    }
    bool met() const {
        return judged() && value <= threshold;
    }
};
// The four criteria of a step in NWChem's order (Gmax, Grms, Xrms, Xmax), against the thresholds the
// log printed ({gmax, grms, xmax, xrms}; a missing one is not judged).
std::array<ResultCriterion, 4> resultCriteria(const ResultStep &step, const QJsonObject &thresholds);

// An occupied → virtual orbital pair of an excited state: its X amplitude, its Y amplitude where
// NWChem printed one (else NaN), and its weight X² − Y² as a percentage of the sum over the pairs
// printed for the state (NaN when that sum is not positive). NWChem prints the amplitudes above 0.05,
// so the weights of the pairs it left out are shared among the printed ones.
struct ResultTransition {
    int from = 0, to = 0;
    QString spin; // "alpha" / "beta", or empty for restricted states.
    double x = 0, y = std::numeric_limits<double>::quiet_NaN(),
           weight = std::numeric_limits<double>::quiet_NaN();
};
// The pairs of a state, the heaviest first.
QVector<ResultTransition> resultTransitions(const ResultExcitation &state);
// The number of the highest occupied orbital of a spin ("beta"; anything else is alpha, which
// restricted states use) in the reference of the excited states: from the electrons the log counted
// ("tddft" of the properties), else from its orbital analysis; 0 when neither tells.
int resultHomoNumber(const ResultData &data, const QString &spin);
// "HOMO−1", "HOMO", "LUMO", "LUMO+2" for an orbital number against the HOMO's; the number without one.
QString resultFrontierLabel(int number, int homo);
// "HOMO−1 → LUMO" ("12 → 14" without the HOMO's number), after "α " / "β " for unrestricted states.
QString resultTransitionText(const ResultData &data, const ResultTransition &transition);

// One line: the optimization state, the calculation energy, imaginary frequencies and excited states.
QString resultBriefSummary(const ResultData &data);
// How a loaded result is reported: its reading status, then the brief summary; empty without one.
QString resultMessage(const ResultData &data);
} // namespace mf
