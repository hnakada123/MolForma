#pragma once
#include "editing/library.h"
#include "model/geometry.h"
namespace mf {
Vec freeDirection(const Document &d, const QString &id);
void completeHydrogens(Document &d, const Ids &selection = {});
// Changes a bond and invalidates old fragment assignments. Hydrogen adjustment is explicit;
// callers apply this to an EditorSession copy so failed valence checks leave the document intact.
void changeBond(Document &d, const QString &a, const QString &b, const QString &kind, bool hydrogen = false);
struct AtomDeletionOptions {
    bool includeAttachedHydrogens = false;
    bool capCutBonds = false;
};
// Removes the selected atoms and their references. A total charge equal to the structure's expected
// charge follows the new sum. Optional hydrogen handling is explicit, independent of GUI preferences.
// Call on an editor-session copy, like other editing operations; fixed atoms cannot be removed.
void deleteAtoms(Document &d, const Ids &ids, AtomDeletionOptions options = {});
QString addAtom(Document &d, const QString &symbol, const Vec &position, const QString &anchor = {},
                const QString &kind = "single", bool hydrogen = true);
// Inserts a new atom into the bond a–b at covalent bond lengths, moving the smaller side outward.
QString insertAtom(Document &d, QString a, QString b, const QString &symbol, bool hydrogen = true);
void replaceElement(Document &d, const Ids &ids, const QString &symbol, bool hydrogen);
PlacedPart attachPart(Document &d, const QString &anchor, const QJsonObject &t, double twist = 0);
PlacedPart replacePart(Document &d, Ids ids, const QJsonObject &t, double twist = 0);
Ids duplicate(Document &d, const Ids &ids, const Vec &offset);
// Adds another molecule (SMILES, structure file) beside the current one, 3 Å beyond it along +X.
// Atoms, bonds, residues, parts and coordination centers are merged; point charges and settings of
// the fragment are not. A total charge equal to the formal charges follows the new sum.
Ids insertFragment(Document &d, const Document &fragment);
// Reflects whole molecules through the plane normal to `axis` that passes through `origin`.
void mirror(Document &d, const Ids &ids, int axis, const Vec &origin = Vec::Zero());
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
