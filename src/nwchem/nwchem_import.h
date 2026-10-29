// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"

namespace mf {
struct NwchemImport {
    Document document;
    QStringList notes; // Directives and options that were not converted.
};
// Reads the structure (Cartesian GEOMETRY), charge, multiplicity and the calculation settings
// MolForma offers from an existing NWChem input. Other directives are listed in notes.
NwchemImport importNwchemInput(const QString &text);
// Statements as NWChem reads them: comments removed, split at ";", "\\" continuation lines joined.
QStringList nwchemStatements(const QString &text);
} // namespace mf
