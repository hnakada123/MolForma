// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "io/pubchem.h"
class QWidget;
namespace mf {
// Presents the shared PubChem transfer with progress and cancellation.
PubchemRecord downloadPubchem(QWidget *parent, const PubchemQuery &query, const QString &folder,
                              const QString &source = PubchemSource);
} // namespace mf
