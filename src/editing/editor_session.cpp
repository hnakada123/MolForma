// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/editor_session.h"
#include "model/i18n.h"
#include <QJsonDocument>

namespace mf {
class SessionCommand : public QUndoCommand {
    EditorSession &session;
    Document before, after;
    bool remote, pushed = false;

  public:
    SessionCommand(EditorSession &s, const QString &name, const Document &next, bool remote)
        : QUndoCommand(name), session(s), before(s.document()), after(next), remote(remote) {}
    void undo() override {
        session.apply(before, remote);
        emit session.historyMoved(true, text(), after, before);
    }
    void redo() override {
        session.apply(after, remote);
        if (pushed)
            emit session.historyMoved(false, text(), before, after);
        pushed = true;
    }
};
EditorSession::EditorSession(QObject *parent) : QObject(parent) {
    stack.setUndoLimit(100);
    connect(&stack, &QUndoStack::indexChanged, this, &EditorSession::changed);
}
void EditorSession::apply(const Document &d, bool remote) {
    const auto before = current;
    current = d;
    pending.reset();
    emit applied(before, current, remote);
}
void EditorSession::reset(const Document &d, bool remote) {
    apply(d, remote);
    stack.clear();
    stack.setClean();
    emit changed();
}
void EditorSession::validateEdit(const Document &next) const {
    for (const auto &atom : current.atoms)
        if (atom.fixed && next.contains(atom.id) && next.atom(atom.id).position != atom.position)
            fail(tr("固定原子は移動できません。"));
    for (const auto &atom : next.atoms)
        element(atom.element);
    const auto errors = next.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
}
bool EditorSession::commit(const QString &name, const Document &next, bool remote) {
    validateEdit(next);
    if (current.project() == next.project())
        return false;
    stack.push(new SessionCommand(*this, name, next, remote));
    return true;
}
bool EditorSession::edit(const QString &name, const std::function<void(Document &)> &operation, bool preview,
                         bool remote) {
    if (pending)
        fail(tr("先に変更候補を確定するか、取消してください。"));
    Document next = current;
    operation(next);
    return preview ? propose(name, next, remote) : commit(name, next, remote);
}
bool EditorSession::propose(const QString &name, const Document &next, bool remote) {
    if (pending)
        fail(tr("先に変更候補を確定するか、取消してください。"));
    validateEdit(next);
    if (current.project() == next.project())
        return false;
    pending = next;
    pendingName = name;
    pendingRemote = remote;
    ++pendingSerial;
    emit changed();
    return true;
}
bool EditorSession::confirm(bool remote) {
    if (!pending)
        return false;
    const auto next = *pending;
    const bool committed = commit(pendingName, next, pendingRemote || remote);
    if (!committed)
        cancel();
    return committed;
}
void EditorSession::cancel() {
    pending.reset();
    emit changed();
}
void EditorSession::navigate(const QString &action) {
    if (action == "confirm" || action == "cancel") {
        if (!pending)
            fail(tr("変更候補がありません。"));
        if (action == "confirm")
            confirm();
        else
            cancel();
        return;
    }
    if (pending)
        fail(tr("先に変更候補を確定するか、取消してください。"));
    if (action == "undo" && stack.canUndo())
        stack.undo();
    else if (action == "redo" && stack.canRedo())
        stack.redo();
    else
        fail(tr("対応する編集履歴がありません。"));
}
} // namespace mf
