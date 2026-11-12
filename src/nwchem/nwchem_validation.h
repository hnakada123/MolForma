// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem.h"

namespace mf {
// Basic value checks from the setting specifications, independent of chemical combinations.
// The settings are resolved once by the caller, including defaults.
void validateNwchemSettingValues(const Document &document, const QJsonObject &settings,
                                 ValidationReport &report);
} // namespace mf
