// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonObject>
#include <QString>

namespace mf {
// The editing tools in the rail at the left of "1 分子".
enum class EditorTool { Select, Move, Bond, Delete, Place, Replace, Charge };
// The tool in object names ("tool_select", "toolAction_select") and as its icon.
QString editorToolKey(EditorTool tool);

// What a palette row places. The rows are keyed "atom:C", "center:Fe" and "smiles:CCO", parts by
// their ID; the kind of a part comes from its category.
enum class PaletteKind { Atom, Center, Smiles, Part, Substituent, Ligand, Amino };
struct PaletteEntry {
    // Without a row chosen, the place and replace tools use the element of the element selector.
    PaletteKind kind = PaletteKind::Atom;
    QString value; // The element of an atom or a center, or the SMILES.
    bool element() const {
        return kind == PaletteKind::Atom || kind == PaletteKind::Center;
    }
    // A part placed from its template: a skeleton, substituent, ligand, amino acid or cap.
    bool part() const {
        return !element() && kind != PaletteKind::Smiles;
    }
};
PaletteEntry paletteEntry(const QString &key, const QJsonObject &part = {});
QString paletteKey(PaletteKind kind, const QString &value);
} // namespace mf
