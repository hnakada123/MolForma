// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
namespace mf::result_detail {
// The versioned binary payload shared by result workers and their cache.
QByteArray resultBytes(const ResultData &result);
ResultData resultFromBytes(const QByteArray &bytes);
} // namespace mf::result_detail
