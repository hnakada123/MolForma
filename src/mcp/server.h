#pragma once
#include "model/document.h"
#include <QObject>
#include <functional>
#include <optional>

namespace mf {
inline constexpr auto McpProtocolVersion = "2026-07-28";
inline constexpr auto McpLegacyVersion = "2025-11-25";

// Hooks run on the application's thread. Without hooks the server owns a headless document.
struct McpEditor {
    std::function<Document()> document;
    std::function<std::optional<Document>()> candidate;
    std::function<void(const QString &, const std::function<void(Document &)> &, bool)> edit;
    std::function<void(const QString &)> history;
    std::function<QJsonArray()> additionalTools;
    std::function<QJsonObject(const QString &, const QJsonObject &)> callAdditionalTool;
    std::function<QString()> baseDirectory;
};

class McpServer : public QObject {
    Q_OBJECT
  public:
    explicit McpServer(McpEditor editor = {}, QObject *parent = nullptr);
    // Empty means a notification: never send a response to it.
    QJsonObject receive(const QByteArray &line);
    QString documentId() const;
    QJsonObject snapshot() const;

  private:
    McpEditor editor;
    QString id = newId();
    Document memory;
    std::optional<Document> pending;
    QVector<Document> undo{memory};
    int undoIndex = 0;
    enum class LegacyState { None, Initializing, Ready };
    LegacyState legacy = LegacyState::None;
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
