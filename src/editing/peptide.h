// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/operations.h"
namespace mf {
QStringList parseSequence(const QString &text);
Residue residue(const Document &d, const QString &id);
QString residueForAtom(const Document &d, const QString &atom);
// The residue of every atom in one (the first residue that holds it) by atom ID: residueForAtom of
// all atoms at once, for loops over many atoms.
QHash<QString, QString> atomResidues(const Document &d);
// An atom's place in its residue, "GLY 2 · CA" (empty outside residues); atomPlaces gives the
// places of all atoms at once by atom ID, for lists of many atoms.
QString atomPlace(const Document &d, const QString &atom);
QHash<QString, QString> atomPlaces(const Document &d);
void updateResidue(Document &d, const Residue &residue); // Replaces the residue with its ID, or adds it.
QString appendResidue(Document &d, const QString &code, const QString &previous = {},
                      const QString &state = "default", const QString &chirality = "L",
                      const QString &n = "charged", const QString &c = "charged",
                      const Vec &position = Vec::Zero());
QString prependResidue(Document &d, const QString &code, const QString &following,
                       const QString &state = "default", const QString &chirality = "L",
                       const QString &n = "charged");
// The edits of residues that change the formal charges keep the total charge following them
// (followingCharge): adding a residue at a terminus, building or mutating residues and setting the
// terminal states of a chain. The lower-level appendResidue, prependResidue and setTerminals leave it.
QString addTerminalResidue(Document &d, const QString &residueId, bool nTerminus, const QString &code,
                           const QString &state, const QString &chirality, const QString &n,
                           const QString &c);
void buildPeptide(Document &d, const QStringList &codes, const QString &n, const QString &c,
                  const QString &chirality = "L", const QString &state = "default");
void setTerminals(Document &d, const QString &residueId, const QString &n = {}, const QString &c = {});
// Sets the N terminus of the chain of a residue to n and its C terminus to c.
void setChainTerminals(Document &d, const QString &residueId, const QString &n, const QString &c);
void addCap(Document &d, const QString &residueId, const QString &kind);
void mutateResidue(Document &d, const QString &residueId, const QString &code, const QString &state,
                   const QString &chirality);
void deleteResidue(Document &d, const QString &residueId);
void setResidueTorsion(Document &d, const QString &residueId, const QString &kind, double angle);
void registerResidue(Document &d, const Ids &ids, const QString &code);
QStringList residueDiagnostics(const Document &d, const QString &residueId);
void repairResidue(Document &d, const QString &residueId);
void reconnectResidues(Document &d, const QString &left, const QString &right);
} // namespace mf
