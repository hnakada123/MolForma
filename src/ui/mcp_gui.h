#pragma once
#include "mcp/server.h"
#include <QMap>
#include <QPointer>

namespace mf {
class MainWindow;
// Native, in-process UI access. Handles are scoped to this adapter, never addresses.
class McpGui : public QObject {
  public:
    explicit McpGui(MainWindow &window, McpPolicy policy = {});
    ~McpGui() override;
    McpEditor editor();
    QJsonArray tools() const;
    QJsonObject call(const QString &name, const QJsonObject &args);

  private:
    MainWindow &window;
    McpPolicy policy;
    QMap<QString, QPointer<QObject>> objects;
    QMap<QString, QJsonObject> commands;
    QStringList commandOrder;
    quint64 nextHandle = 0, nextCommand = 0;
    QString handle(QObject *object);
    QObject *resolve(const QString &target);
    QList<QObject *> discover(bool hidden);
    QJsonObject describe(QObject *object);
    QJsonObject state() const;
    QJsonObject results(const QJsonObject &args) const;
    QByteArray documentState() const;
    QJsonObject enqueue(const QString &name, const QJsonObject &args);
    std::optional<QJsonObject> task(const QString &commandId) const;
    bool cancel(const QString &commandId);
    // Why MCP may not operate this control, or empty.
    QString blocked(const QObject *object, const QString &tool) const;
    void checkPaths(const QString &name, const QJsonObject &args) const;
    QJsonObject prompt();
    void answer(const QString &key, const QString &choice);
    QJsonObject execute(const QString &name, const QJsonObject &args);
};
} // namespace mf
