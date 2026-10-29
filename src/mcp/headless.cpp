#include "mcp/headless.h"
#include "mcp/tools.h"
#include "results/result_summary.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace mf {
McpHeadless::McpHeadless(McpPolicy p, QJsonObject trustedExecution, QObject *parent)
    : QObject(parent), policy(std::move(p)), trusted(std::move(trustedExecution)) {
    connect(&run, &NwchemRun::changed, this, [this] {
        if (!run.active() && !runWork.isEmpty() && !finished.contains(runWork))
            finish(runWork, "calculation", "start", "calculation");
    });
    connect(&loader, &ResultLoader::loaded, this, [this](ResultData next, const QString &) {
        data = std::move(next);
        loadMessage = resultMessage(data); // As the result viewer reports it.
        finish(loadWork, "results_action", loadAction, "results");
    });
    connect(&loader, &ResultLoader::failed, this, [this](const QString &error) {
        loadMessage = error;
        finish(loadWork, "results_action", loadAction, "results");
    });
}
McpHeadless::~McpHeadless() {
    cancelLoad({});
    run.shutdown();
}
void McpHeadless::attach(const McpServer &owner) {
    server = &owner;
}
McpEditor McpHeadless::editor() {
    McpEditor e;
    e.additionalTools = [this] { return tools(); };
    e.callAdditionalTool = [this](const QString &name, const QJsonObject &args) { return call(name, args); };
    e.taskWork = [](const QString &, const QJsonObject &result) { return result["workId"].toString(); };
    e.taskState = [this](const QString &work) { return task(work); };
    e.cancelTask = [this](const QString &work) { return cancel(work); };
    e.additionalResources = [] { return runtimeResources(); };
    e.readResource = [this](const QString &uri) { return runtimeResource(uri, state(), data, run.plan()); };
    return e;
}
QJsonArray McpHeadless::tools() const {
    const QJsonObject text{{"type", "string"}};
    const QJsonObject work{{"type", "object"},
                           {"properties", QJsonObject{{"tool", text}, {"action", text}, {"workId", text}}},
                           {"required", QJsonArray{"tool", "action"}}};
    return {
        mcpTool("get_app_state", "Read application state",
                "Read the candidate flag, NWChem calculation state and result-loading state of this headless "
                "server.",
                {}, {}, false, true,
                {{"type", "object"}, {"required", QJsonArray{"candidate", "calculation", "results"}}}),
        calculationTool("Start/stop NWChem or save an immutable input bundle with the document's execution "
                        "settings. start returns at once; follow the returned task or poll get_app_state. "
                        "Exit code zero alone does not establish convergence.",
                        work, policy.allowExecutionSettings),
        resultsTool(),
        mcpTool(
            "results_action", "Load results",
            "Open a result file or calculation directory, reload it, or cancel loading. Loading runs in a "
            "cancellable process and never changes the document; follow the task or poll get_app_state.",
            {{"action", QJsonObject{{"type", "string"},
                                    {"enum", QJsonArray{"open_file", "open_directory", "reload", "cancel"}}}},
             {"path", text}},
            {"action"}, true, true, work)};
}
QJsonObject McpHeadless::state() const {
    return {{"candidate", server && server->candidate().has_value()},
            {"calculation", calculationJson(run, saved.directory)},
            {"results", QJsonObject{{"loading", loader.active()},
                                    {"source", data.source},
                                    {"pendingPath", loader.pendingPath()},
                                    {"message", loadMessage}}}};
}
QJsonObject McpHeadless::call(const QString &name, const QJsonObject &args) {
    if (name == "get_app_state")
        return state();
    if (name == "get_results")
        return resultsJson(data, args, state()["results"].toObject());
    const auto action = args["action"].toString();
    if (name == "calculation") {
        if (action == "stop") {
            run.stop();
            return {
                {"tool", name}, {"action", action}, {"calculation", calculationJson(run, saved.directory)}};
        }
        const auto d = server->document();
        const auto base = QDir::currentPath();
        if (server->candidate())
            fail("Confirm or cancel the candidate before running or saving a calculation.");
        policy.checkRun(d, base);
        if (action == "save_bundle") {
            saved = saveExecutionBundle(d, base);
            return {
                {"tool", name}, {"action", action}, {"calculation", calculationJson(run, saved.directory)}};
        }
        if (run.active())
            fail("A calculation is already running.");
        if (!policy.allowExecutionSettings && protectedExecution(d) != trusted)
            fail("The NWChem executable, MPI launcher, environment overrides or additional NWChem input "
                 "differ from the defaults this headless server trusts (MolForma's saved Preferences, "
                 "otherwise nwchem and mpirun from PATH with no overrides and no additional input). MCP "
                 "clients cannot choose them; restart the server with --mcp-allow-execution-settings to "
                 "permit it.");
        runWork = "calculation-" + QString::number(++nextWork);
        run.start(d, base);
        return {{"tool", name},
                {"action", action},
                {"workId", runWork},
                {"calculation", calculationJson(run, saved.directory)}};
    }
    if (action == "cancel") {
        cancelLoad("Loading was cancelled; the previous result is kept.");
        return {{"tool", name}, {"action", action}, {"results", state()["results"]}};
    }
    auto path = args["path"].toString();
    if (action == "reload") {
        if (data.source.isEmpty())
            fail("No result file is open.");
        path = data.source;
    } else if (path.isEmpty())
        fail("Specify path.");
    policy.checkPath(path);
    if (action == "open_directory")
        path = preferredResultFile(path);
    load(path, action);
    return {{"tool", name}, {"action", action}, {"workId", loadWork}, {"results", state()["results"]}};
}
void McpHeadless::load(const QString &path, const QString &action) {
    const QFileInfo before(path);
    if (!before.isFile())
        fail("Result file not found: " + path);
    cancelLoad("Superseded by a newer load.");
    loadWork = "results-" + QString::number(++nextWork);
    loadAction = action;
    loader.start({path});
    loadMessage = "Loading " + loader.pendingPath();
}
void McpHeadless::cancelLoad(const QString &message) {
    if (!loader.cancel())
        return;
    loadMessage = message;
    finish(loadWork, "results_action", loadAction, "results");
}

void McpHeadless::finish(const QString &work, const QString &tool, const QString &action,
                         const QString &section) {
    finished[work] = QJsonObject{{"tool", tool},
                                 {"action", action},
                                 {"workId", work},
                                 {"app", QJsonObject{{section, state()[section]}}}};
    finishedOrder.append(work);
    while (finishedOrder.size() > 128)
        finished.remove(finishedOrder.takeFirst());
}
std::optional<QJsonObject> McpHeadless::task(const QString &work) const {
    if (work == runWork && run.active())
        return QJsonObject{{"state", "running"}, {"message", "NWChem is " + runStateKey(run.state()) + "."}};
    if (work == loadWork && loader.active())
        return QJsonObject{{"state", "running"}, {"message", "Loading " + loader.pendingPath()}};
    if (finished.contains(work))
        return QJsonObject{{"state", "done"}, {"result", finished[work]}};
    return std::nullopt;
}
bool McpHeadless::cancel(const QString &work) {
    if (work == runWork && run.active()) {
        run.stop();
        return true;
    }
    if (work == loadWork && loader.active()) {
        cancelLoad("Loading was cancelled; the previous result is kept.");
        return true;
    }
    return false;
}
} // namespace mf
