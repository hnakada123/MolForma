// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <optional>

namespace mf {
// Portable, versioned JSON containing the NWChem output and optional NMR reference/display settings.
// Embedded logs are data only: no paths are followed and no calculations are executed.
ResultData readResultBundle(const QByteArray &bytes, const QString &source);
struct NmrDisplay {
    QJsonObject references, parameters;
    QString referenceName;
    bool automaticWidth = true;
};
std::optional<NmrDisplay> resultNmrDisplay(const ResultData &result);
// Apply a saved NMR presentation, with explicit overrides (including a different nucleus).
QJsonObject resultSpectrumParameters(const ResultData &result, const QJsonObject &overrides = {});
struct ResultBundleRequest {
    QString sourceStamp, referencePath, referenceStamp;
    QJsonObject references, parameters;
    bool automaticWidth = true;
};
// Reads the original logs only if their stamps still match; validates before atomically saving.
void saveResultBundle(const QString &path, const ResultData &result, const ResultBundleRequest &request);
} // namespace mf
