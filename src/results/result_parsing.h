// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <optional>
namespace mf::result_detail {
// Shared limits and lexical readers; independent of file formats, workers and caches.
inline constexpr qint64 maxBytes = 128 * 1024 * 1024;
inline constexpr int maxAtoms = 2000, maxFrames = 2000, maxPoints = 2000000, maxTriangles = 500000;
void warn(ResultData &result, const QString &text);
QStringList words(QStringView line);
std::optional<double> resultNumber(QStringView text);
double number(QStringView text);
double logNumber(const QString &text);
int integer(QStringView text, int minimum, int maximum);
Vec point(const QStringList &words, int start, double scale = 1);
QString symbol(int atomicNumber);
Atom atom(QString tag, const Vec &position);
void addFrame(ResultData &result, Document document, const QString &label);
QByteArray boundedRead(const QString &path);
// XYZ structures, also embedded in MOLDEN files.
void xyz(ResultData &result, const QStringList &lines);
} // namespace mf::result_detail
