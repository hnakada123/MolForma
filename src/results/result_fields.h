// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
namespace mf::result_detail {
// The values of a grid: a dataset of a CUBE, or an orbital evaluated on the MOLDEN grid.
struct GridValues {
    QVector<double> values;
    Vec origin = Vec::Zero();
    Mat axes = Mat::Identity();
};
// What a MOLDEN file holds for its orbital shapes, besides the result: the blocks of the basis
// (all but [MO]) and the MO coefficients.
struct MoldenData {
    QMap<QString, QStringList> blocks;
    QVector<QVector<double>> coefficients;
};
class ResultCache;
MoldenData moldenData(ResultData &result, const QStringList &lines);
void moldenSurface(ResultData &result, const MoldenData &data, int orbital, const ResultCache &cache);
GridValues cubeValues(ResultData &result, QString text);
void cubeSurface(ResultData &result, const GridValues &grid);
void mesh(ResultData &result, const QVector<double> &values, const Vec &origin, const Mat &axes);
} // namespace mf::result_detail
