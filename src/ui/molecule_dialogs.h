// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "model/geometry.h"
#include <optional>
class QWidget;
namespace mf {
class EditorContext;
class UiServices;
// The dialogs of the tools of "1 分子", each built here and answered with what the user chose. The
// molecular workspace (MoleculePanel) decides when one may open and makes the edit of its answer; a
// dialog that edits while it is open does so through the editor context.

// A SMILES to build in 3D; empty when cancelled.
QString askSmiles(QWidget *parent, const QString &title, const QString &label, const QString &initial);
// "構造の確認": the report of a structure, to read.
void showStructureReport(QWidget *parent, const Document &document);
// "構造の比較": how the molecule fits the structure of the file `name`. Whether it shall be laid onto
// that structure (as a candidate).
bool askSuperposition(QWidget *parent, const QString &name, const Superposition &fit);
// "Z-matrix（内部座標）": the document in internal coordinates. A value edited in the table is an
// edit of the document, made at once.
void editZMatrix(QWidget *parent, EditorContext &editor, UiServices &uiContext);
// "点群・対称化": the point group of the shown structure within a tolerance (remembered in the
// preferences). The tolerance and the group to symmetrize onto, or empty when closed.
struct SymmetryChoice {
    double tolerance = 0; // Å
    QString group;        // Schoenflies symbol.
};
std::optional<SymmetryChoice> askSymmetrization(QWidget *parent, EditorContext &editor,
                                                UiServices &uiContext);
// "溶媒和殻を配置": the solvent (a template ID), how many molecules and whether around the selection
// (of `selected` atoms) only; the solvent and the number are remembered in the preferences. Empty when
// cancelled.
struct SolvationChoice {
    QString solvent;
    int count = 0;
    bool aroundSelection = false;
};
std::optional<SolvationChoice> askSolvation(QWidget *parent, UiServices &uiContext, int selected);
// "PDBの診断・修復": the diagnostics of each residue, with the repairs as candidates. Fails with a
// candidate pending or without atoms.
void preparePdb(QWidget *parent, EditorContext &editor, UiServices &uiContext);
} // namespace mf
