// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem.h"
// Internal geometry of constraints, scans and counterpoise fragments.
namespace mf::nwchem_detail {
QString coordinateError(const Document &d, const Ids &ids, double value);
QString zcoordLine(const Document &d, const Ids &ids, double value, double scale, const QString &name);
QStringList constraintLines(const Document &d, const QJsonObject &s, double scale);
bool constrained(const QJsonObject &s);
double scanValue(const QJsonObject &s, int step, int coordinate = 1);
bool twoDimensionalScan(const QJsonObject &s);
double coordinateDeviation(const Document &d, const Ids &ids, double value);
Document scanGeometry(const Document &d, const QJsonObject &s, int step, int step2 = 0);
QVector<QVector<int>> bsseMonomers(const Document &d);
int monomerCharge(const Document &d, const QVector<int> &atoms);
} // namespace mf::nwchem_detail
