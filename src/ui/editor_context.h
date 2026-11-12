// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QVariant>
#include <functional>
class QWidget;
namespace mf {
struct McpPolicy;
// Capabilities supplied by the composition root. Panels receive only the interfaces they use;
// none of these exposes MainWindow, sibling panels, or mutable document storage.
class EditorContext {
  public:
    virtual ~EditorContext() = default;
    virtual const Document &document() const = 0;
    virtual const Document &displayDocument() const = 0;
    virtual quint64 revision() const = 0;
    virtual bool hasCandidate() const = 0;
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
    virtual void checkMcpPath(const QString &path) const = 0;
    virtual void showMessage(const QString &message, int timeout = 0) = 0;
    virtual void addStatusWidget(QWidget *widget) = 0;
};
class WorkspaceActions {
  public:
    virtual ~WorkspaceActions() = default;
    virtual void showPanel(QWidget *panel) = 0;
    virtual void showPanel(int index) = 0;
    virtual void showSetting(int tab, const QString &setting = {}, const QString &group = {}) = 0;
    virtual void showInputLine(int line = -1) = 0;
    virtual void refreshPanel(QWidget *panel, bool always = false) = 0;
    virtual void commitPendingSettings() = 0;
    virtual void setPanelDiagnostics(QWidget *panel, bool error, const QString &tip) = 0;
    virtual void setRunPanelShown(bool shown) = 0;
    virtual void showRun(bool active, const QString &text, bool stoppable) = 0;
    virtual QString settingSectionTitle(const QString &setting) const = 0;
    virtual QString moleculeLayoutMode() const = 0;
    virtual void showCoordinateTable() = 0;
    virtual void sortAtoms(const QString &mode) = 0;
    virtual void fitMoleculeView() = 0;
    virtual Document resultStructure() const = 0;
};
class ExecutionAccess {
  public:
    virtual ~ExecutionAccess() = default;
    virtual const McpPolicy *accessPolicy() const = 0;
    virtual bool remoteRequest() const = 0;
    virtual void trustCurrentExecution() = 0;
    virtual QStringList untrustedExecution() const = 0;
};
} // namespace mf
