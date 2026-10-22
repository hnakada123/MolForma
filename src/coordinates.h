#pragma once
#include "document.h"

namespace mf {
inline constexpr double BohrToAngstrom = 0.529177210903;
enum class CoordinateUnit { Angstrom, Bohr };
enum class CoordinateColumn { Element, X, Y, Z, Mass, Ghost, Fixed };
double coordinateScale(CoordinateUnit unit);
QString coordinateText(const Atom &atom, CoordinateColumn column, CoordinateUnit unit);
// Change only the requested cell; callers edit a document copy and validate before committing.
void setCoordinateCell(Document &document, const QString &id, CoordinateColumn column, const QString &text,
                       CoordinateUnit unit);
Atom coordinateAtom(const QString &symbol, const QStringList &xyz, const QString &mass, CoordinateUnit unit,
                    bool ghost = false, bool fixed = false);
void replaceCoordinateStructure(Document &document, const Document &structure);
// NWChem input fragment, not a complete calculation (basis/method/task are configured separately).
QString nwchemGeometry(const Document &document, CoordinateUnit unit, bool fragmentComments = true);
} // namespace mf
