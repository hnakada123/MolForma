// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <optional>
namespace mf {
// CUBE coordinates have already been converted to angstrom. Values retain the file's units.
std::optional<double> sampleField(const ResultData &field, const Vec &position);
void validateFieldOverlay(const ResultData &surface, const ResultData &field);
struct PrincipalTensor {
    Vec values;
    Mat axes;
};
PrincipalTensor polarizabilityAxes(const QJsonArray &tensor);
} // namespace mf
