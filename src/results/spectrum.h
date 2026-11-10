// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <QPointF>

namespace mf {
struct SpectrumData {
    QJsonObject parameters;
    QJsonArray peaks;
    QVector<QPointF> sticks, curve;
    QString xUnit, yUnit;
    bool reversed = false;
};
// Display models only: finite, bounded input; peak records retain source atom/mode indices.
// NMR requires an explicit reference_ppm; width is Lorentzian FWHM in x-axis units.
SpectrumData calculateSpectrum(const ResultData &result, const QJsonObject &parameters);
// Supported magnetic isotopes are those used by NWChem's default hyperfine calculation.
QString eprIsotope(const QString &element);
QJsonArray eprNuclei(const ResultData &result);
QByteArray spectrumCsv(const SpectrumData &spectrum);
} // namespace mf
