#pragma once
#include "editing/editor_session.h"
#include "mcp/policy.h"
#include "mcp/tools.h"
#include <QDateTime>
#include <QMap>
#include <QObject>
#include <functional>
#include <optional>

namespace mf {
inline constexpr auto McpProtocolVersion = "2026-07-28";
inline constexpr auto McpLegacyVersion = "2025-11-25";
inline constexpr auto McpTasksExtension = "io.modelcontextprotocol/tasks";

// Hooks run on the application's thread. Without hooks the server owns a headless document.
struct McpEditor {
    std::function<Document()> document;
    std::function<std::optional<Document>()> candidate;
    std::function<void(const QString &, const std::function<void(Document &)> &, bool)> edit;
    std::function<void(const QString &)> history;
    std::function<QJsonArray()> additionalTools;
    std::function<QJsonObject(const QString &, const QJsonObject &)> callAdditionalTool;
    std::function<QString()> baseDirectory;
    // Optional asynchronous work (Tasks extension). taskWork names the work a tool result started, or
    // returns empty. taskState returns {state: queued|running|done|failed|cancelled, message,
    // result|error}, or nullopt once expired. cancelTask returns whether cancellation was started.
    std::function<QString(const QString &tool, const QJsonObject &result)> taskWork;
    std::function<std::optional<QJsonObject>(const QString &)> taskState;
    std::function<bool(const QString &)> cancelTask;
    // Optional application-modal confirmation: {key, message, choices}, or empty when none is shown.
    std::function<QJsonObject()> pendingPrompt;
    std::function<void(const QString &key, const QString &choice)> answerPrompt;
    // Optional resources beyond the document: listed entries and {uri, mimeType, text} contents.
    std::function<QJsonArray()> additionalResources;
    std::function<std::optional<QJsonObject>(const QString &uri)> readResource;
};

class McpServer : public QObject {
    Q_OBJECT
  public:
    explicit McpServer(McpEditor editor = {}, McpPolicy policy = {}, QObject *parent = nullptr);
    // Empty means a notification: never send a response to it.
    QJsonObject receive(const QByteArray &line);
    QString documentId() const;
    QJsonObject snapshot() const;
    // Compact edit result: revision, formula and candidate atom differences without the project.
    QJsonObject summary() const;
    Document document() const;
    std::optional<Document> candidate() const;
    // Without editor hooks: start the server's own document from `initial` (e.g. with the user's
    // saved execution defaults). Clears the candidate and the undo history.
    void resetDocument(const Document &initial);

  private:
    McpEditor editor;
    McpPolicy policy;
    QString id = newId();
    EditorSession session;
    enum class LegacyState { None, Initializing, Ready };
    LegacyState legacy = LegacyState::None;
    // Per-request input of the request being dispatched (multi round-trip requests).
    struct Request {
        bool modern = false;
        QJsonObject capabilities, inputResponses;
        QString requestState;
    } incoming;
    struct Task {
        QString work, tool, state;
        QDateTime created, updated;
        bool cancelRequested = false;
    };
    QMap<QString, Task> tasks;
    QStringList taskOrder;
    QByteArray stateKey = newId().toLatin1();
    bool elicitation() const;
    QString sealState(const QJsonObject &state) const;
    std::optional<QJsonObject> openState() const;
    QJsonObject createTask(const QString &tool, const QString &work);
    QJsonObject taskResult(const QString &taskId);
    QString revision() const;
    void edit(const QString &name, const std::function<void(Document &)> &operation, bool preview);
    void history(const QString &action);
    // A built-in tool: what tools/list says of it and what a call does. tools/list and tools/call both
    // come from this list (and the editor's additional tools).
    struct Tool {
        McpToolSpec spec;
        std::function<QJsonObject(const QJsonObject &args)> call;
    };
    QVector<Tool> builtIn;
    void registerTools();
    QJsonObject saveFile(const QJsonObject &args);
    QString baseDirectory() const;
    QJsonArray toolList() const;
    QSet<QString> toolNames() const;
    QJsonArray resourceList() const;
    QJsonObject callTool(const QString &name, const QJsonObject &args);
};
} // namespace mf
