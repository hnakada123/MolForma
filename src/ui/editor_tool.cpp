// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/editor_tool.h"

namespace mf {
QString editorToolKey(EditorTool tool) {
    switch (tool) {
    case EditorTool::Select:
        return "select";
    case EditorTool::Move:
        return "move";
    case EditorTool::Bond:
        return "bond";
    case EditorTool::Delete:
        return "delete";
    case EditorTool::Place:
        return "place";
    case EditorTool::Replace:
        return "replace";
    case EditorTool::Charge:
        return "charge";
    }
    return {};
}
PaletteEntry paletteEntry(const QString &key, const QJsonObject &part) {
    for (const auto &[prefix, kind] : {std::pair{"atom:", PaletteKind::Atom},
                                       {"center:", PaletteKind::Center},
                                       {"smiles:", PaletteKind::Smiles}})
        if (key.startsWith(prefix))
            return {kind, key.mid(QString(prefix).size())};
    if (key.isEmpty())
        return {};
    const auto category = part["category"].toString();
    return {category == "substituent" ? PaletteKind::Substituent
            : category == "ligand"    ? PaletteKind::Ligand
            : category == "amino"     ? PaletteKind::Amino
                                      : PaletteKind::Part,
            {}};
}
QString paletteKey(PaletteKind kind, const QString &value) {
    switch (kind) {
    case PaletteKind::Atom:
        return "atom:" + value;
    case PaletteKind::Center:
        return "center:" + value;
    case PaletteKind::Smiles:
        return "smiles:" + value;
    default:
        return value; // A part's ID.
    }
}
} // namespace mf
