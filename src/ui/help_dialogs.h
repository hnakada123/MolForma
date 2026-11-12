// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <functional>
class QAction;
class QMenuBar;
class QWidget;
namespace mf {
class UiServices;
// Makes the embedded manual and the window icon (docs/manual.qrc) available: before either is read.
void initManualResources();
// "操作ガイド・マニュアル": the pages of the manual embedded in the application (in the language of the
// interface), with a page chooser, back and a search. Links between pages are followed there, other
// links open outside. A link to a practice file of the recipes is handed to useSample (the resource
// path; it returns whether the file was used, and the viewer closes then).
void manualDialog(QWidget *parent, UiServices &services,
                  const std::function<bool(const QString &resource)> &useSample);
// "キーボードショートカット一覧": the keys of every command in the menus, with where each works, then
// the mouse controls of the 3D view. Undo and redo are listed by their own names, not by the operation
// their menu entries name at the moment.
void shortcutsDialog(QWidget *parent, const QMenuBar *menus, const QAction *undoAction,
                     const QAction *redoAction);
} // namespace mf
