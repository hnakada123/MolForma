// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include "model/setting_target.h"
#include "nwchem/run_session.h"
#include <QVariant>
#include <functional>
class QWidget;
namespace mf {
// Capabilities supplied by the composition root. Panels receive only the interfaces they use;
// none of these exposes MainWindow, sibling panels, the editing session, its undo stack, the MCP
// policy or mutable document storage. What one panel asks of another is a signal of the panel,
// connected where the two are composed (MainWindow, WorkspaceController, a parent panel).
class EditorContext {
  public:
    virtual ~EditorContext() = default;
    virtual const Document &document() const = 0;
    // The candidate while there is one, else the document.
    virtual const Document &displayDocument() const = 0;
    virtual quint64 revision() const = 0;
    virtual bool hasCandidate() const = 0;
    // The candidate: the name of its edit, and a number that tells candidates apart (it grows with
    // each one proposed).
    virtual QString candidateName() const = 0;
    virtual int candidateSerial() const = 0;
    // Whether the one acting now may confirm the candidate in passing, on the way to a next one (a
    // ligand placed, as the next site is clicked): a person never confirms a candidate that MCP
    // proposed that way, while MCP may continue its own. False without a candidate.
    virtual bool ownsCandidate() const = 0;
    virtual bool chemistryRunning() const = 0;
    virtual const Ids &selection() const = 0;
    virtual void setSelection(const Ids &ids) = 0;
    virtual void edit(const QString &name, const std::function<void(Document &)> &operation,
                      bool preview = false) = 0;
    virtual void confirmCandidate() = 0;
    virtual void cancelCandidate() = 0;
    virtual bool resolveCandidate() = 0;
    virtual void cancelWorker() = 0;
    virtual void runWorker(const QString &mode, const QString &smiles = {}, const Ids &moving = {},
                           int count = 10) = 0;
    virtual bool replaceUnsavedDocument(const Document &document) = 0;
    virtual QString projectPath() const = 0;
};
class UiServices {
  public:
    virtual ~UiServices() = default;
    virtual void safe(const std::function<void()> &operation) = 0;
    // The user's preferences, by the keys of model/settings.h (Preference::…): the one way of widgets to
    // them. What a widget remembers goes through here, never through a store of its own.
    virtual QVariant preference(const QString &key, const QVariant &fallback = {}) const = 0;
    virtual void setPreference(const QString &key, const QVariant &value) = 0;
    // Refuses a path outside MCP's folders (--mcp-allow-dir) while a request of MCP is applied; the
    // user's own actions reach any path.
    virtual void checkMcpPath(const QString &path) const = 0;
    // Whether a path is outside MCP's folders, whoever acts now: for a widget that writes there and is
    // shown between requests, which says so to the MCP adapter (property `mcpPath`).
    virtual bool outsideMcpFolders(const QString &path) const = 0;
    virtual void showMessage(const QString &message, int timeout = 0) = 0;
    virtual void addStatusWidget(QWidget *widget) = 0;
};
// Navigation only: which panel is shown, at which setting or input line, and bringing a panel up to
// date. Places are named by what they hold (a panel, the owner of a setting), never by a tab number:
// the order of the tabs is the window's (ui/setting_navigation.h).
class WorkspaceActions {
  public:
    virtual ~WorkspaceActions() = default;
    virtual void showPanel(QWidget *panel) = 0;
    // The tab of an owner (the molecule, the calculation, the execution), with the focus on one of
    // its settings or, for the calculation, with the details of a group open ("dft").
    virtual void showSetting(SettingTarget target, const QString &setting = {},
                             const QString &group = {}) = 0;
    // "4 入力プレビュー", with the cursor on a line (from 0) of the input.
    virtual void showInputLine(int line = -1) = 0;
    // Refreshes a panel that changes left stale while it was hidden (always: also one that is not).
    virtual void refreshPanel(QWidget *panel, bool always = false) = 0;
};
// The gate of a calculation, as checks: who may run what is decided behind them (the user's
// confirmations, MCP's command-line policy), and a panel only asks.
class ExecutionAccess {
  public:
    virtual ~ExecutionAccess() = default;
    // The check of a calculation's paths for the one who asks for it now: MCP's folders
    // (--mcp-allow-dir) for a request over MCP, none (empty) for the user's own.
    virtual RunSession::Check runCheck() const = 0;
    // Whether the programs, environment and additional input of a run need no confirmation
    // (--mcp-allow-execution-settings).
    virtual bool mayChooseExecutionSettings() const = 0;
    // Whether the one who asks can be asked to confirm them: the user can, a request over MCP is
    // refused instead.
    virtual bool mayConfirmExecution() const = 0;
    virtual void trustCurrentExecution() = 0;
    // The keys of the execution settings whose values the user has neither entered nor confirmed.
    virtual QStringList untrustedExecution() const = 0;
};
} // namespace mf
