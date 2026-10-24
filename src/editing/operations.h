#pragma once
#include "editing/library.h"
#include "model/geometry.h"
namespace mf {
Vec freeDirection(const Document &d, const QString &id);
void completeHydrogens(Document &d, const Ids &selection = {});
QString addAtom(Document &d, const QString &symbol, const Vec &position, const QString &anchor = {},
                const QString &kind = "single", bool hydrogen = true);
void replaceElement(Document &d, const Ids &ids, const QString &symbol, bool hydrogen);
PlacedPart attachPart(Document &d, const QString &anchor, const QJsonObject &t, double twist = 0);
PlacedPart replacePart(Document &d, Ids ids, const QJsonObject &t, double twist = 0);
Ids duplicate(Document &d, const Ids &ids, const Vec &offset);
// Adds another molecule (SMILES, structure file) beside the current one, 3 Å beyond it along +X.
// Atoms, bonds, residues, parts and coordination centers are merged; point charges and settings of
// the fragment are not. A total charge equal to the formal charges follows the new sum.
Ids insertFragment(Document &d, const Document &fragment);
void mirror(Document &d, const Ids &ids, int axis);
void invertCenter(Document &d, const QString &id);
void switchEZ(Document &d, const Ids &ids);
void proposeFragments(Document &d, const QString &target, const QString &mode);
void mergeIonContacts(Document &d);
void assignFragments(Document &d, const QString &target, const QList<int> &members, const QList<int> &charges,
                     const QList<int> &multiplicities, const QList<QPair<int, int>> &cuts);
void waterToCharges(Document &d, const Ids &ids);
void addReference(Document &d, const Ids &ids, const QString &kind, double value = 0, double end = 0,
                  int steps = 10);
QString structureReport(const Document &d);
} // namespace mf
