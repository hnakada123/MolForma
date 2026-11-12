// SPDX-License-Identifier: GPL-2.0-or-later
#include "mcp/server.h"
#include "mcp/server_detail.h"
#include <QDir>
#include <algorithm>

// The document behind the tools: the editor's through its hooks, or the server's own session. Reading
// it, the summary of a candidate, and edits with MCP's size limit.
namespace mf {
namespace mcp_detail {
void validateDocument(const Document &d) {
    for (const auto &a : d.atoms)
        element(a.element);
    auto errors = d.validate();
    if (!errors.isEmpty())
        fail(errors.join('\n'));
}
} // namespace mcp_detail
using namespace mcp_detail;
namespace {
constexpr int SummaryIds = 100;
} // namespace
void McpServer::resetDocument(const Document &initial) {
    session.reset(initial);
}
QString McpServer::documentId() const {
    return id;
}
Document McpServer::document() const {
    return editor.document ? editor.document() : session.document();
}
std::optional<Document> McpServer::candidate() const {
    return editor.candidate ? editor.candidate() : session.candidate();
}
QString McpServer::revision() const {
    return QString::number(editor.revision ? editor.revision() : session.revision());
}
QJsonObject McpServer::snapshot() const {
    const auto d = document();
    const auto c = candidate();
    return {{"documentId", id},       {"revision", revision()},
            {"formula", d.formula()}, {"atomCount", d.atoms.size()},
            {"project", d.project()}, {"candidate", c ? QJsonValue(c->project()) : QJsonValue()}};
}
QJsonObject McpServer::summary() const {
    const auto d = document();
    const auto c = candidate();
    QJsonObject out{{"documentId", id},
                    {"revision", revision()},
                    {"formula", d.formula()},
                    {"atomCount", d.atoms.size()},
                    {"candidate", QJsonValue()}};
    if (!c)
        return out;
    // Atom-level differences let a client review a candidate without reading the full project.
    QJsonArray added, removed, changed;
    bool truncated = false;
    const auto note = [&](QJsonArray &list, const QString &atomId) {
        if (list.size() < SummaryIds)
            list.append(atomId);
        else
            truncated = true;
    };
    QHash<QString, const Atom *> before;
    for (const auto &a : d.atoms)
        before.insert(a.id, &a);
    QSet<QString> after;
    for (const auto &a : c->atoms) {
        after.insert(a.id);
        if (const auto *old = before.value(a.id); !old)
            note(added, a.id);
        else if (*old != a)
            note(changed, a.id);
    }
    for (const auto &a : d.atoms)
        if (!after.contains(a.id))
            note(removed, a.id);
    out["candidate"] =
        QJsonObject{{"formula", c->formula()}, {"atomCount", c->atoms.size()}, {"addedIds", added},
                    {"removedIds", removed},   {"changedIds", changed},        {"idsTruncated", truncated}};
    return out;
}
void McpServer::edit(const QString &name, const std::function<void(Document &)> &operation, bool preview) {
    const auto checked = [&](Document &d) {
        // MCP's input size limit, in both frontends: no edit makes a document of more than MaxAtoms atoms. A
        // larger one opened in the GUI can be edited, so long as no atom is added to it.
        QSet<QString> before;
        if (d.atoms.size() > MaxAtoms)
            for (const auto &a : d.atoms)
                before.insert(a.id);
        operation(d);
        if (d.atoms.size() > MaxAtoms && std::any_of(d.atoms.begin(), d.atoms.end(),
                                                     [&](const Atom &a) { return !before.contains(a.id); }))
            fail(QString(
                     "MCP edits cannot make documents of more than %1 atoms. A larger document opened in the "
                     "GUI can be edited without adding atoms, and cut_out_site with path reads a site of a "
                     "larger structure file.")
                     .arg(MaxAtoms));
        validateDocument(d);
    };
    if (editor.edit)
        editor.edit(name, checked, preview);
    else
        session.edit(name, checked, preview, true);
}
void McpServer::history(const QString &action) {
    if (editor.history)
        editor.history(action);
    else
        session.navigate(action);
}
QString McpServer::baseDirectory() const {
    return editor.baseDirectory ? editor.baseDirectory() : QDir::currentPath();
}
} // namespace mf
