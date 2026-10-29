#pragma once
#include "model/document.h"
#include "model/units.h"

namespace mf {
struct NwchemGeometryOptions {
    QString name;            // Named geometry (scan points); empty for NWChem's default "geometry".
    QStringList zcoord;      // ZCOORD internal coordinates; switches AUTOZ on so they can be constrained.
    bool autoz = false;      // Leave AUTOZ on without ZCOORD (optimizations in internal coordinates).
    bool constraints = true; // CONSTRAINTS block with the fixed atoms.
};
struct NwchemGeometryFragment {
    QString geometry, constraints, comments;
};
NwchemGeometryFragment nwchemGeometryFragment(const Document &document, CoordinateUnit unit,
                                              const NwchemGeometryOptions &options = {});
// NWChem input fragment, not a complete calculation (basis/method/task are configured separately).
QString nwchemGeometry(const Document &document, CoordinateUnit unit, bool fragmentComments = true,
                       const NwchemGeometryOptions &options = {});
} // namespace mf
