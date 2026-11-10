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
// kind nmr/raman/ecd/ord/epr, and ir (IR intensities of the real vibrations) and uv (oscillator
// strengths over eV) as the result viewer's own charts draw them. NMR requires an explicit
// reference_ppm; width is the FWHM in x-axis units of a Lorentzian (a Gaussian for uv).
SpectrumData calculateSpectrum(const ResultData &result, const QJsonObject &parameters);
// What a conformer adds to the averaged spectra of its set: the peaks of the kinds ir, raman, uv and
// ecd its result holds, and "shielding" [{atom, element, isotropic}] of a singlet. Kinds without data
// are left out.
QJsonObject spectrumContributions(const ResultData &result);
// The spectrum of conformers in fast exchange. members: [{name, weight, spectra}] with the
// contributions above; parameters as for calculateSpectrum (kind ir/raman/uv/ecd/nmr). Members with
// a weight but without data of the kind are left out (parameters "omitted"; "included" are the
// others) and the remaining weights scaled to a sum of 1. Sticks are those of every included
// conformer times its weight, broadened once; peaks name their "conformer". NMR is the weighted mean
// shielding of each atom. Members without weight (duplicates) add nothing.
SpectrumData averagedSpectrum(const QJsonArray &members, const QJsonObject &parameters);
// Supported magnetic isotopes are those used by NWChem's default hyperfine calculation.
QString eprIsotope(const QString &element);
QJsonArray eprNuclei(const ResultData &result);
// Peaks and curve; peaks of an averaged spectrum end with the name of their conformer.
QByteArray spectrumCsv(const SpectrumData &spectrum);
} // namespace mf
