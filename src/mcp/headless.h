#pragma once
#include "mcp/policy.h"
#include "mcp/server.h"
#include "nwchem/nwchem_execution.h"
#include "results/result_loader.h"

namespace mf {
// NWChem runs and result reading for the headless MCP server, using the same core execution
// and cancellable --result-worker process as the GUI. Work stops when the process exits.
class McpHeadless : public QObject {
  public:
    // trustedExecution: the executable, launcher and environment MCP runs may use (by default the
    // built-in defaults; main() passes the user's saved Preferences).
    explicit McpHeadless(McpPolicy policy, QJsonObject trustedExecution = protectedExecution(Document()),
                         QObject *parent = nullptr);
    ~McpHeadless() override;
    McpEditor editor();
    // The server owns the document; attach it before the first request.
    void attach(const McpServer &owner);

  private:
    McpPolicy policy;
    const McpServer *server = nullptr;
    NwchemRun run;
    ExecutionPlan saved;
    // Without a GUI only the user's saved defaults are trusted: MCP cannot pick another program.
    QJsonObject trusted;
    ResultData data;
    ResultLoader loader;
    QString loadMessage, loadAction, runWork, loadWork;
    QMap<QString, QJsonObject> finished;
    QStringList finishedOrder;
    quint64 nextWork = 0;
    QJsonArray tools() const;
    QJsonObject call(const QString &name, const QJsonObject &args);
    QJsonObject state() const;
    void load(const QString &path, const QString &action);
    void cancelLoad(const QString &message);
    void finish(const QString &work, const QString &tool, const QString &action, const QString &section);
    std::optional<QJsonObject> task(const QString &work) const;
    bool cancel(const QString &work);
};
} // namespace mf
