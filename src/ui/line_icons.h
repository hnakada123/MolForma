// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QColor>
#include <QIcon>
#include <QString>

namespace mf {
// Stroked icons on a 24×24 grid ("select", "move", "undo", "lock", "copy", …), in the palette's text color,
// the accent when checked and the disabled color when off, or always in `color` if given. "play", "pause",
// "first" and "last" are filled.
QIcon lineIcon(const QString &name, const QColor &color = {});
// The number of a workflow step in a circle, filled with the accent while its tab is open.
QIcon stepIcon(int number);
// Markers of candidate changes as on the 3D view: "added", "removed" or "moved".
QIcon changeIcon(const QString &kind);
} // namespace mf
