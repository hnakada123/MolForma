// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"

namespace mf {
// A project replaces all fields. Other formats carry a structure; NWChem/Gaussian also carry
// charge and multiplicity, and NWChem calculation settings. A multi-frame XYZ requires xyzFrame.
Document importStructure(const QString &path, int pdbModel = 1, const QString &altloc = {},
                         int xyzFrame = -1);
void exportStructure(const Document &document, const QString &path);
QStringList xyzFrames(const QString &text);
Document gaussianStructure(const QString &text);
} // namespace mf
