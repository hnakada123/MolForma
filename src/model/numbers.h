// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <cmath>
#include <optional>

namespace mf {
// A finite number in C notation, also with the Fortran exponent of NWChem and MOLDEN files
// ("1.5D-03"); nullopt for anything else.
inline std::optional<double> fortranNumber(QString text) {
    text.replace(QChar('D'), QChar('e'), Qt::CaseInsensitive);
    bool ok;
    const double value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value))
        return std::nullopt;
    return value;
}
} // namespace mf
