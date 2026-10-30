// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <QObject>
#include <QUndoStack>
#include <functional>
#include <optional>

namespace mf {
// One transaction boundary for GUI and headless editing. Presentation and access policy stay
// with the caller; candidates, validation, provenance and history belong to the session.
class EditorSession : public QObject {
    Q_OBJECT
  public:
    // How the document changed: an edit committed now, undo/redo, or a document loaded with reset.
    enum class Change { Edit, History, Reset };
    Q_ENUM(Change)
    explicit EditorSession(QObject *parent = nullptr);
    const Document &document() const {
        return current;
    }
    const std::optional<Document> &candidate() const {
        return pending;
    }
    const Document &displayDocument() const {
        return pending ? *pending : current;
    }
    QString candidateName() const {
        return pendingName;
    }
    bool candidateRemote() const {
        return pendingRemote;
    }
    // Tells candidates apart: it grows with each candidate proposed.
    int candidateSerial() const {
        return pendingSerial;
    }
    // Grows whenever the document or its candidate changes (edits, undo and redo, reset, candidates
    // proposed, confirmed or cancelled): what was read at another revision is stale.
    quint64 revision() const {
        return changes;
    }
    QUndoStack &history() {
        return stack;
    }
    void reset(const Document &document, bool remote = false);
    bool edit(const QString &name, const std::function<void(Document &)> &operation, bool preview = false,
              bool remote = false);
    bool propose(const QString &name, const Document &next, bool remote = false);
    bool confirm(bool remote = false);
    void cancel();
    void navigate(const QString &action);
  signals:
    void changed();
    void applied(const mf::Document &before, const mf::Document &after, bool remote,
                 mf::EditorSession::Change change);
    void historyMoved(bool undo, QString name, const mf::Document &before, const mf::Document &after);

  private:
    friend class SessionCommand;
    Document current;
    std::optional<Document> pending;
    QString pendingName;
    bool pendingRemote = false;
    int pendingSerial = 0;
    quint64 changes = 0;
    QUndoStack stack;
    void apply(const Document &document, bool remote, Change change);
    bool commit(const QString &name, const Document &next, bool remote);
    void validateEdit(const Document &next) const;
};
} // namespace mf
