#pragma once
#include "model/document.h"
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
    // Optional asynchronous work behind a returned commandId (Tasks extension). taskState returns
    // {state: queued|running|done|failed, message, result|error}, or nullopt for an expired command.
    std::function<std::optional<QJsonObject>(const QString &)> taskState;
    // Optional application-modal confirmation: {key, message, choices}, or empty when none is shown.
    std::function<QJsonObject()> pendingPrompt;
    std::function<void(const QString &key, const QString &choice)> answerPrompt;
};

class McpServer : public QObject {
    Q_OBJECT
  public:
    explicit McpServer(McpEditor editor = {}, QObject *parent = nullptr);
    // Empty means a notification: never send a response to it.
    QJsonObject receive(const QByteArray &line);
    QString documentId() const;
    QJsonObject snapshot() const;
    // Compact edit result: revision, formula and candidate atom differences without the project.
    QJsonObject summary() const;

  private:
    McpEditor editor;
    QString id = newId();
    Document memory;
    std::optional<Document> pending;
    QVector<Document> undo{memory};
    int undoIndex = 0;
    enum class LegacyState { None, Initializing, Ready };
    LegacyState legacy = LegacyState::None;
    // Per-request input of the request being dispatched (multi round-trip requests).
    struct Request {
        bool modern = false;
        QJsonObject capabilities, inputResponses;
        QString requestState;
    } incoming;
    struct Task {
        QString commandId, tool, state;
        QDateTime created, updated;
    };
    QMap<QString, Task> tasks;
    QStringList taskOrder;
    QByteArray stateKey = newId().toLatin1();
    bool elicitation() const;
    QString sealState(const QJsonObject &state) const;
    std::optional<QJsonObject> openState() const;
    QJsonObject createTask(const QString &tool, const QJsonObject &queued);
    QJsonObject taskResult(const QString &taskId);
    Document document() const;
    std::optional<Document> candidate() const;
    QString revision() const;
    void edit(const QString &name, const std::function<void(Document &)> &operation, bool preview);
    void history(const QString &action);
    void commit(const Document &next);
    QJsonArray toolList() const;
    QJsonObject callTool(const QString &name, const QJsonObject &args);
};
} // namespace mf
