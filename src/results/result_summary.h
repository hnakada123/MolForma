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

// One line: the optimization state, the calculation energy, imaginary frequencies and excited states.
QString resultBriefSummary(const ResultData &data);
// How a loaded result is reported: its reading status, then the brief summary; empty without one.
QString resultMessage(const ResultData &data);
} // namespace mf
