// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/library.h"
#include "model/geometry.h"
namespace mf {
// Runs edit on d. A total charge that equals the structure's expected charge (expectedCharge: the
// formal charges, with the oxidation states of coordination centers) before the edit follows that
// charge after it; a total charge set to anything else stays. Deleting atoms, inserting fragments and
// the edits of coordination and residues keep the total charge in step so; the edits of single atoms
// (attributes, elements, bonds) leave it.
template <class Edit> void followingCharge(Document &d, Edit &&edit) {
    const bool follows = d.charge == d.expectedCharge();
    edit();
    if (follows)
        d.charge = d.expectedCharge();
}
Vec freeDirection(const Document &d, const QString &id);
void completeHydrogens(Document &d, const Ids &selection = {});
// Changes a bond. Hydrogen adjustment is explicit; callers apply this to an EditorSession copy so
// failed valence checks leave the document intact.
void changeBond(Document &d, const QString &a, const QString &b, const QString &kind, bool hydrogen = false);
struct AtomDeletionOptions {
    bool includeAttachedHydrogens = false;
    bool capCutBonds = false;
};
// Removes the selected atoms and their references; the total charge follows (followingCharge).
// Optional hydrogen handling is explicit, independent of GUI preferences.
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
// the fragment are not; the total charge follows (followingCharge).
Ids insertFragment(Document &d, const Document &fragment);
// Reflects whole molecules through the plane normal to `axis` that passes through `origin`.
void mirror(Document &d, const Ids &ids, int axis, const Vec &origin = Vec::Zero());
void invertCenter(Document &d, const QString &id);
void switchEZ(Document &d, const Ids &ids);
void waterToCharges(Document &d, const Ids &ids);
// External point charges (Å, e), entered by hand.
QString addPointCharge(Document &d, const Vec &position, double charge);
void setPointCharge(Document &d, const QString &id, const Vec &position, double charge);
void removePointCharge(Document &d, const QString &id);
QString structureReport(const Document &d);
} // namespace mf
