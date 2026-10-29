#pragma once
#include "model/document.h"

namespace mf {
inline constexpr double BohrToAngstrom = 0.529177210903;
enum class CoordinateUnit { Angstrom, Bohr };
// Cells of the coordinate table; the atom attributes (formal charge, unpaired electrons, atom name)
// come after the columns of the NWChem geometry.
enum class CoordinateColumn { Element, X, Y, Z, Mass, Ghost, Fixed, Charge, Radical, Name };
double coordinateScale(CoordinateUnit unit);
QString coordinateText(const Atom &atom, CoordinateColumn column, CoordinateUnit unit);
// Change only the requested cell; callers edit a document copy and validate before committing.
void setCoordinateCell(Document &document, const QString &id, CoordinateColumn column, const QString &text,
                       CoordinateUnit unit);
Atom coordinateAtom(const QString &symbol, const QStringList &xyz, const QString &mass, CoordinateUnit unit,
                    bool ghost = false, bool fixed = false);
void replaceCoordinateStructure(Document &document, const Document &structure);
// What a candidate changes, atom by atom. Atoms are matched by stable ID and count as moved beyond
// 1e-4 Å. A replacement that gives every atom a new ID (a structure read from a file or pasted) is
// matched by order instead when one element sequence starts the other, so that reading the same
// structure again, or with atoms appended, shows only what changed; atoms matched by order count
// as unchanged within 0.001 Å, the precision of PDB files.
struct AtomChanges {
    QSet<QString> added, moved; // Atom IDs in the changed document.
    QVector<Atom> removed;      // Atoms of the original that the changed document no longer has.
    bool byOrder = false;
};
AtomChanges atomChanges(const Document &before, const Document &after);
// Moves the atoms to a calculation result with the same atoms in the same order (element and
// ghost flag). Fixed atoms must already sit at the result position (NWChem keeps them); IDs,
// bonds, residues and settings stay. Returns the RMSD and the largest displacement in Å.
std::pair<double, double> applyResultGeometry(Document &document, const Document &result);
// One Z-matrix line: references are earlier atoms (indices), bonded ones preferred.
struct ZMatrixRow {
    int bond = -1, angle = -1, dihedral = -1;
    double distance = 0, angleValue = 0, dihedralValue = 0;
};
QVector<ZMatrixRow> zMatrix(const Document &document);
// "O" / "H 1 0.96" / "H 1 0.96 2 104.5" / ... with 1-based references (Å, degrees).
QString zMatrixText(const Document &document);
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
