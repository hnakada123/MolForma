// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QStringList>

namespace mf {
// Which values of the executable, launcher, environment, remote destination and additional NWChem input
// (the keys of protectedExecution, mcp/policy.h) a run may use without asking: those the user entered in
// MolForma or confirmed before a run. Values read from a project, an autosave copy or the history, those
// of undo/redo and those set over MCP count only once confirmed; those saved in Preferences always count.
// No widgets and no preference read: the window keeps one for its session and tells it what happened, and
// the headless MCP server, where nothing is entered or confirmed, asks one that stays empty.
class ExecutionTrust {
  public:
    // What changed a document.
    enum class Origin {
        User,    // An edit made in MolForma itself.
        Mcp,     // An edit made, or a candidate confirmed, over MCP.
        History, // Undo or redo.
        Load,    // Another document in its place: a project, an autosave copy, a job of the history.
    };
    // The document changed from `before` to `after`. Only the values an edit made in MolForma itself
    // changes become trusted: never those of loads, undo/redo or MCP requests, nor the other values kept
    // beside an edited one.
    void documentChanged(const Document &before, const Document &after, Origin origin);
    // The user confirmed the values of a document before a run: they are trusted from now on.
    void confirm(const Document &document);
    // Keys of protectedExecution() whose values in `document` the user has neither entered in MolForma nor
    // confirmed before a run, and that are not those of `preferences` (protectedExecution() of the
    // settings saved in Preferences): runs with them need a confirmation.
    QStringList untrusted(const Document &document, const QJsonObject &preferences) const;

  private:
    // The trusted values, per key of protectedExecution().
    QHash<QString, QList<QJsonValue>> trusted;
    void trust(const QJsonObject &values);
};
} // namespace mf
