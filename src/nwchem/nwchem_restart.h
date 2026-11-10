// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"

namespace mf {
// A way to go on from a saved job (see NwchemRestart) and, if it is not open to that job, why.
struct RestartChoice {
    QString mode, reason;
    bool available() const {
        return reason.isEmpty();
    }
};
// "continue", "hessian" and "reuse" for a job folder, from its record and files; the log is not read.
QVector<RestartChoice> restartChoices(const QString &jobDirectory);
// What a way to go on does, as a button says it ("続きから実行を準備").
QString restartLabel(const QString &mode);
// A new document that goes on from a job: its saved project with the last structure of its log and
// the "restart" settings. The whole log is read once (the tasks it finished, the last structure).
Document restartDocument(const QString &jobDirectory, const QString &mode);
// A new document that starts from the molecular orbitals of a job (its movecs as the input vectors),
// with the job's conditions and the last structure of its log: for another calculation at the same
// basis and atom order.
Document orbitalReuseDocument(const QString &jobDirectory);
// The file a job's NWChem database is kept in, or empty when the job has none MolForma can go on
// from (not run, files outside the job folder).
QString restartDatabase(const QString &jobDirectory);
// Where the database of the job a document continues must be (settings "restart"); empty when that
// job is no MolForma job folder or the document keeps NWChem's files outside the job.
QString restartSource(const Document &document);
// Copies the database, orbitals and Hessians a restart needs from the job named in the document's
// settings into a new job folder (NWChem's scratch files of integrals and grids are left behind).
void copyRestartFiles(const Document &document, const QString &targetDirectory);
} // namespace mf
