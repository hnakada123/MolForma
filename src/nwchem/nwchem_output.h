#pragma once
#include "nwchem/nwchem.h"
#include <array>

namespace mf {
struct NwchemCubeGrid {
    std::array<double, 3> minimum{}, maximum{};
    std::array<int, 3> divisions{};
    qint64 points() const;
};
// NWChem prefixes orbital filenames with permanent_dir, except absolute and explicit ./ or ../ paths.
QString nwchemPermanentFile(const Document &document, const QString &name);
NwchemCubeGrid nwchemCubeGrid(const Document &document);
void validateNwchemOutput(const Document &document, NwchemValidation &validation);
QStringList nwchemOutputFiles(const Document &document, const NwchemValidation &validation);
NwchemGeneratedInput nwchemOutputLines(const Document &document, const NwchemValidation &validation);
} // namespace mf
