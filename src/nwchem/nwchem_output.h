// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem.h"
#include <array>

namespace mf {
struct NwchemCubeGrid {
    std::array<double, 3> minimum{}, maximum{};
    std::array<int, 3> divisions{};
    qint64 points() const;
};
// The CUBE contents of an excited state of TDDFT ("cube_type"): transition_density (from the CI vectors),
// excited_density and difference_density (excited − ground, from the density matrices a TDDFT gradient
// writes).
bool nwchemCubeExcited(const QString &type);
// Whether the input runs "task tddft gradient" for an excited-state density of the CUBE output.
bool nwchemCubeGradient(const Document &document);
// The excited state of those CUBE contents (cube_root, from 1).
int nwchemCubeRoot(const Document &document);
// NWChem prefixes orbital filenames with permanent_dir, except absolute and explicit ./ or ../ paths.
QString nwchemPermanentFile(const Document &document, const QString &name);
NwchemCubeGrid nwchemCubeGrid(const Document &document);
void validateNwchemOutput(const Document &document, NwchemValidation &validation);
QStringList nwchemOutputFiles(const Document &document, const NwchemValidation &validation);
NwchemGeneratedInput nwchemOutputLines(const Document &document, const NwchemValidation &validation);
} // namespace mf
