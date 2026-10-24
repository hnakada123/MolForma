#include "editing/library.h"
#include "mcp/headless.h"
#include "mcp/server.h"
#include "mcp/stdio_transport.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include "ui/main_window.h"
#include "ui/mcp_gui.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace mf;
namespace {
QJsonObject metadata(const QString &version = McpProtocolVersion) {
    return {{"io.modelcontextprotocol/protocolVersion", version},
            {"io.modelcontextprotocol/clientCapabilities", QJsonObject{}}};
}
QByteArray message(const QString &method, QJsonObject params = {}, QJsonValue id = 1, bool modern = true) {
    if (modern)
        params["_meta"] = metadata();
    QJsonObject request{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (!id.isUndefined())
        request["id"] = id;
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}
// A modern request from a client declaring elicitation and/or the Tasks extension.
QJsonObject clientCapabilities(bool elicitation, bool tasks) {
    QJsonObject c;
    if (elicitation)
        c["elicitation"] = QJsonObject{{"form", QJsonObject{}}};
    if (tasks)
        c["extensions"] = QJsonObject{{McpTasksExtension, QJsonObject{}}};
    return c;
}
QByteArray capable(const QString &method, QJsonObject params, const QJsonObject &capabilities,
                   QJsonValue id = 1) {
    auto meta = metadata();
    meta["io.modelcontextprotocol/clientCapabilities"] = capabilities;
    params["_meta"] = meta;
    return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}})
        .toJson(QJsonDocument::Compact);
}
QJsonObject call(McpServer &server, const QString &name, QJsonObject args = {}) {
    return server.receive(message("tools/call", {{"name", name}, {"arguments", args}}));
}
QJsonObject data(const QJsonObject &response) {
    return response["result"].toObject()["structuredContent"].toObject();
}
bool failed(const QJsonObject &response) {
    return response.contains("error") || response["result"].toObject()["isError"].toBool();
}
QString text(const QJsonObject &response) {
    return response["result"].toObject()["content"].toArray()[0].toObject()["text"].toString();
}
QJsonObject toolNamed(McpServer &server, const QString &name) {
    for (const auto &tool : server.receive(message("tools/list"))["result"].toObject()["tools"].toArray())
        if (tool.toObject()["name"] == name)
            return tool.toObject();
    return {};
}
QString resourceText(McpServer &server, const QString &uri) {
    return server.receive(message("resources/read", {{"uri", uri}}))["result"]
        .toObject()["contents"]
        .toArray()[0]
        .toObject()["text"]
        .toString();
}
const char *SyntheticModes =
    "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\nH 2 1 0 0 1.4\n"
    "[GTO]\n1 0\ns 1 1.0\n1.0 1.0\n"
    "[MO]\nSym= A1\nEne= -0.5\nSpin= Alpha\nOccup= 2.0\n1 1.0\n"
    "[FREQ]\n-100\n1500\n[FR-COORD]\nH 0 0 0\nH 0 0 1.4\n"
    "[FR-NORM-COORD]\nvibration 1\n0 0 .5\n0 0 -.5\nvibration 2\n0 0 .1\n0 0 -.1\n[INT]\n10\n20\n";
QJsonObject scoped(McpServer &server, QJsonObject args = {}, bool mutation = false) {
    args["documentId"] = server.documentId();
    if (mutation)
        args["expectedRevision"] = server.snapshot()["revision"];
    return args;
}
QJsonObject change(McpServer &server, const QString &tool, QJsonObject args = {}) {
    return call(server, tool, scoped(server, args, true));
}
Document committed(McpServer &server) {
    return Document::fromProject(server.snapshot()["project"].toObject());
}
QJsonObject readReply(QProcess &process) {
    QElapsedTimer timer;
    timer.start();
    while (!process.canReadLine() && timer.elapsed() < 15000)
        if (!process.waitForReadyRead(1000) && process.state() == QProcess::NotRunning)
            break;
    return QJsonDocument::fromJson(process.readLine()).object();
}
void startProcess(QProcess &process, const QString &mode) {
    auto env = QProcessEnvironment::systemEnvironment();
    env.remove("DISPLAY");
    env.remove("WAYLAND_DISPLAY");
    env.insert("QT_QPA_PLATFORM", mode == "--mcp" ? "nonexistent-platform-plugin" : "offscreen");
    env.insert("MOLFORMA_RENDERER", "painter");
    process.setProcessEnvironment(env);
    process.start(QCoreApplication::applicationDirPath() + "/molforma", {mode});
}
QJsonObject processCall(QProcess &process, const QString &tool, QJsonObject args = {}, bool mutate = false) {
    process.write(message("tools/call", {{"name", "list_documents"}, {"arguments", QJsonObject{}}}) + '\n');
    const auto docs = data(readReply(process))["documents"].toArray();
    if (docs.isEmpty())
        return {{"error", "missing document"}};
    const auto doc = docs[0].toObject();
    args["documentId"] = doc["documentId"];
    if (mutate)
        args["expectedRevision"] = doc["revision"];
    process.write(message("tools/call", {{"name", tool}, {"arguments", args}}) + '\n');
    return readReply(process);
}
QJsonObject processCommand(QProcess &process, const QString &tool, QJsonObject args) {
    const auto reply = processCall(process, tool, args, true);
    if (failed(reply))
        return {{"state", "failed"}, {"response", reply}};
    const QJsonValue id = data(reply)["commandId"];
    QElapsedTimer timer;
    timer.start();
    QJsonObject state;
    do {
        state = data(processCall(process, "get_ui_command", {{"commandId", id}}));
        if (state["state"] == "done" || state["state"] == "failed")
            return state;
        QTest::qWait(10);
    } while (timer.elapsed() < 10000);
    return state;
}
} // namespace

class McpTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, "/tmp/molforma-mcp-test-settings");
    }
    void modernProtocol() {
        McpServer server;
        const auto reply = server.receive(message("server/discover"));
        QCOMPARE(reply["result"].toObject()["supportedVersions"].toArray(),
                 QJsonArray({McpProtocolVersion, McpLegacyVersion}));
        QCOMPARE(reply["result"].toObject()["resultType"].toString(), QString("complete"));
        QVERIFY(
            reply["result"].toObject()["_meta"].toObject().contains("io.modelcontextprotocol/serverInfo"));
        const auto list = server.receive(message("tools/list", {}, "list-1"));
        QCOMPARE(list["id"].toString(), QString("list-1"));
        QVERIFY(list["result"].toObject()["tools"].toArray().size() >= 15);
        // CacheableResult hints are required on modern discovery, list and read results.
        for (const auto &method : {"server/discover", "tools/list", "resources/list",
                                   "resources/templates/list", "prompts/list"}) {
            const auto cached = server.receive(message(method))["result"].toObject();
            QVERIFY2(cached["ttlMs"].isDouble() && cached["ttlMs"].toInt() >= 0, method);
            QCOMPARE(cached["cacheScope"].toString(), QString("private"));
        }
        // Each request carries its own metadata; no initialization or cached version is used.
        auto unsupported = server.receive(message("ping", {{"_meta", metadata("1900-01-01")}}, 2, false));
        QCOMPARE(unsupported["error"].toObject()["code"].toInt(), -32022);
        QCOMPARE(unsupported["error"].toObject()["data"].toObject()["requested"].toString(),
                 QString("1900-01-01"));
        // ping was removed from the stateless protocol; legacy clients keep it.
        QCOMPARE(server.receive(message("ping"))["error"].toObject()["code"].toInt(), -32601);
        const auto discovered = server.receive(message("server/discover"))["result"].toObject();
        QVERIFY(discovered["capabilities"].toObject().contains("prompts"));
        QVERIFY(!discovered["capabilities"].toObject().contains("extensions"));
        auto missing = metadata();
        missing.remove("io.modelcontextprotocol/clientCapabilities");
        QCOMPARE(server.receive(message("ping", {{"_meta", missing}}, 3, false))["error"]
                     .toObject()["code"]
                     .toInt(),
                 -32602);
        QCOMPARE(server.receive(message("tools/list", {}, 4, false))["error"].toObject()["code"].toInt(),
                 -32602);
        QCOMPARE(server.receive(message("unknown"))["error"].toObject()["code"].toInt(), -32601);
        QCOMPARE(server.receive(message("completion/complete"))["error"].toObject()["code"].toInt(), -32601);
        // A second client may invoke a modern RPC without discovery.
        McpServer direct;
        QVERIFY(!direct.receive(message("tools/list")).contains("error"));
    }
    void advancedAndExecutionSettings() {
        McpServer s;
        QVERIFY(!failed(change(
            s, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        QVERIFY(!failed(change(s, "set_advanced",
                               {{"settings", QJsonObject{{"freq_animate", true},
                                                         {"freq_temperatures", QJsonArray{298.15, 350.0}}}},
                                {"preview", false}})));
        QCOMPARE(data(call(s, "get_advanced", scoped(s)))["settings"].toObject()["freq_animate"],
                 QJsonValue(true));
        const auto before = s.snapshot();
        QVERIFY(failed(change(s, "set_advanced",
                              {{"settings", QJsonObject{{"freq_animate", "bad"}}}, {"preview", false}})));
        QVERIFY(failed(change(s, "set_advanced",
                              {{"settings", QJsonObject{{"not_an_option", 1}}}, {"preview", false}})));
        QVERIFY(failed(change(s, "set_execution",
                              {{"settings", QJsonObject{{"not_an_option", 1}}}, {"preview", false}})));
        QCOMPARE(s.snapshot(), before);
        QVERIFY(
            !failed(change(s, "set_advanced",
                           {{"settings", QJsonObject{{"freq_animate", QJsonValue()}}}, {"preview", false}})));
        QVERIFY(!nwchemAdvanced(committed(s)).contains("freq_animate"));
        QVERIFY(!failed(change(
            s, "set_execution",
            {{"settings", QJsonObject{{"mode", "direct"}, {"timeout_seconds", 5}}}, {"preview", false}})));
        QCOMPARE(executionSettings(committed(s))["timeout_seconds"].toInt(), 5);
        QVERIFY(data(call(s, "get_execution", scoped(s))).contains("errors"));
        const auto listing = s.receive(message("tools/list"))["result"].toObject()["tools"].toArray();
        for (const auto &tool : listing)
            QVERIFY(tool.toObject()["name"] != "ui_activate");
    }
    void nativeGuiControlsAndStaleCommands() {
        MainWindow w;
        Document water;
        placeTemplate(water, findTemplate("skeleton:water"), Vec::Zero());
        w.loadDocument(water);
        w.show();
        McpGui gui(w);
        McpServer s(gui.editor());
        auto response =
            data(call(s, "get_ui", scoped(s, {{"query", "dimensionValue"}, {"includeHidden", true}})));
        QCOMPARE(response["items"].toArray().size(), 1);
        const QJsonValue target = response["items"].toArray()[0].toObject()["target"];
        auto command = data(change(s, "ui_set_value", {{"target", target}, {"value", 2.15}}));
        const auto get = [&] {
            return data(call(s, "get_ui_command", scoped(s, {{"commandId", command["commandId"]}})));
        };
        QTRY_COMPARE(get()["state"].toString(), "done");
        QCOMPARE(w.findChild<QDoubleSpinBox *>("dimensionValue")->value(), 2.15);
        command = data(change(s, "ui_set_value", {{"target", target}, {"value", 1e8}}));
        QTRY_COMPARE(get()["state"].toString(), "failed");
        QCOMPARE(w.findChild<QDoubleSpinBox *>("dimensionValue")->value(), 2.15);
        command = data(change(s, "ui_set_value", {{"target", target}, {"value", 3.0}}));
        w.edit("Concurrent GUI edit", [](Document &d) { d.setJobValue("charge", -1); });
        QTRY_COMPARE(get()["state"].toString(), "failed");
        QVERIFY(get()["error"].toString().contains("Document changed"));
        auto *temporary = new QPushButton("temporary", &w);
        temporary->setObjectName("temporaryControl");
        command = data(change(s, "ui_activate", {{"target", "temporaryControl"}}));
        delete temporary;
        QTRY_COMPARE(get()["state"].toString(), "failed");
        command = data(change(s, "ui_select", {{"target", "workspaceTabs"}, {"index", 6}}));
        QTRY_COMPARE(get()["state"].toString(), "done");
        QCOMPARE(w.findChild<QTabWidget *>("workspaceTabs")->currentIndex(), 6);
        const auto original = w.document().project();
        command = data(change(s, "ui_set_value", {{"target", "resultsYaw"}, {"value", 55}}));
        QTRY_COMPARE(get()["state"].toString(), "done");
        QCOMPARE(w.document().project(), original);
        QVERIFY(failed(change(s, "ui_select", {{"target", "workspaceTabs"}, {"index", -1}})));
        command = data(change(s, "ui_select", {{"target", "coordinateUnits"}, {"index", 0}}));
        QTRY_COMPARE(get()["state"].toString(), "done");
        command = data(change(s, "ui_edit_cell",
                              {{"target", "coordinateTable"}, {"row", 1}, {"column", 1}, {"value", "2.15"}}));
        QTRY_VERIFY2(get()["state"] == "done", qPrintable(QString::fromUtf8(QJsonDocument(get()).toJson())));
        QCOMPARE(w.document().atoms[1].position.x(), 2.15);
        QVERIFY(!failed(change(s, "history", {{"action", "undo"}})));
        QCOMPARE(w.document().project(), original);
        command = data(change(s, "set_selection", {{"ids", QJsonArray{w.document().atoms[1].id}}}));
        QTRY_COMPARE(get()["state"].toString(), "done");
        QCOMPARE(data(call(s, "get_app_state", scoped(s)))["selection"].toArray(),
                 QJsonArray{w.document().atoms[1].id});
        // Some native controls apply their settings on editingFinished, not valueChanged.
        QSpinBox spin(&w);
        spin.setObjectName("testCommitSpin");
        QSignalSpy edits(&spin, &QSpinBox::editingFinished);
        command = data(change(s, "ui_set_value", {{"target", "testCommitSpin"}, {"value", 7}}));
        QTRY_COMPARE(get()["state"].toString(), "done");
        QCOMPARE(edits.count(), 1);
        command =
            data(change(s, "ui_set_value", {{"target", "testCommitSpin"}, {"value", 8}, {"commit", false}}));
        QTRY_COMPARE(get()["state"].toString(), "done");
        QCOMPARE(edits.count(), 1);
    }
    void paginatedUiDoesNotOverflowTransport() {
        MainWindow w;
        McpGui gui(w);
        McpServer s(gui.editor());
        QTableWidget table(100, 8, &w);
        table.setObjectName("largeTable");
        for (int row = 0; row < table.rowCount(); ++row)
            for (int col = 0; col < table.columnCount(); ++col)
                table.setItem(row, col, new QTableWidgetItem(QString(1024, 'x')));
        // Pages stay below Claude Code's 50,000-character inline limit; an oversized row comes alone.
        for (int col = 0; col < table.columnCount(); ++col)
            table.setItem(0, col, new QTableWidgetItem(QString(8192, 'y')));
        int offset = 0, pages = 0, rows = 0;
        while (offset < 100) {
            const auto reply = call(
                s, "get_ui_item", scoped(s, {{"target", "largeTable"}, {"offset", offset}, {"limit", 200}}));
            const auto page = data(reply);
            const auto items = page["items"].toArray();
            QVERIFY(!items.isEmpty());
            if (offset == 0)
                QCOMPARE(items.size(), 1);
            else
                QVERIFY(
                    reply["result"].toObject()["content"].toArray()[0].toObject()["text"].toString().size() <
                    50000);
            rows += items.size();
            offset = page["nextOffset"].toInt();
            ++pages;
        }
        QCOMPARE(rows, 100);
        QVERIFY(pages > 2);
    }
    void modalCommandRemainsResponsive() {
        MainWindow w;
        w.show();
        McpGui gui(w);
        McpServer s(gui.editor());
        QPushButton open("Open a modal", &w);
        open.setObjectName("testModal");
        bool handled = false, observedRunning = false;
        QJsonValue commandId;
        connect(&open, &QPushButton::clicked, &w, [&] {
            QMessageBox dialog(QMessageBox::Question, "MCP modal test", "Confirm operation",
                               QMessageBox::Ok | QMessageBox::Cancel, &w);
            QTimer::singleShot(20, &dialog, [&] {
                const auto status = data(call(s, "get_ui_command", scoped(s, {{"commandId", commandId}})));
                observedRunning = status["state"] == "running";
                const auto ui = data(call(s, "get_ui", scoped(s)));
                QVERIFY(!ui["activeModal"].toString().isEmpty());
                QVERIFY(failed(
                    change(s, "add_atom",
                           {{"element", "H"}, {"position", QJsonArray{5, 0, 0}}, {"preview", false}})));
                dialog.button(QMessageBox::Cancel)->setObjectName("testModalCancel");
                QVERIFY(!failed(change(s, "ui_activate", {{"target", "testModalCancel"}})));
            });
            QTimer::singleShot(2000, &dialog, &QDialog::reject);
            handled = dialog.exec() == QMessageBox::Cancel;
        });
        commandId = data(change(s, "ui_activate", {{"target", "testModal"}}))["commandId"];
        QTRY_COMPARE(
            data(call(s, "get_ui_command", scoped(s, {{"commandId", commandId}})))["state"].toString(),
            "done");
        QVERIFY(observedRunning);
        QVERIFY(handled);
    }
    void contextMenuCanBeActivatedOverMcp() {
        MainWindow w;
        Document water;
        placeTemplate(water, findTemplate("skeleton:water"), Vec::Zero());
        w.loadDocument(water);
        w.show();
        w.setSelection({water.atoms[0].id});
        w.canvas()->setObjectName("testCanvas");
        McpGui gui(w);
        McpServer s(gui.editor());
        bool found = false;
        QTimer::singleShot(30, &w, [&] {
            const auto ui = data(call(s, "get_ui", scoped(s, {{"query", "固定 / 解除"}})));
            QVERIFY(!ui["activePopup"].toString().isEmpty());
            for (const auto &entry : ui["items"].toArray())
                if (entry.toObject()["class"] == "QAction") {
                    found = true;
                    QVERIFY(!failed(change(s, "ui_activate", {{"target", entry.toObject()["target"]}})));
                }
        });
        QTimer::singleShot(2000, &w, [] {
            if (auto *popup = QApplication::activePopupWidget())
                popup->close();
        });
        const QJsonValue id = data(change(s, "ui_input",
                                          {{"target", "testCanvas"},
                                           {"action", "click"},
                                           {"button", "right"},
                                           {"x", 10},
                                           {"y", 10}}))["commandId"];
        QTRY_COMPARE(data(call(s, "get_ui_command", scoped(s, {{"commandId", id}})))["state"].toString(),
                     "done");
        QVERIFY(found);
        QVERIFY(w.document().atom(water.atoms[0].id).fixed);
        QVERIFY(!QApplication::activePopupWidget());
    }
    void startupDialogRemainsReachable() {
        QTemporaryDir files;
        QProcess process;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "offscreen");
        env.insert("MOLFORMA_RENDERER", "painter");
        env.insert("XDG_CONFIG_HOME", files.filePath("settings"));
        process.setProcessEnvironment(env);
        process.start(QCoreApplication::applicationDirPath() + "/molforma",
                      {"--mcp-gui", files.filePath("missing.xyz")});
        QVERIFY(process.waitForStarted());
        QJsonObject ui;
        QTRY_VERIFY(([&] {
            ui = data(processCall(process, "get_ui", {{"query", "QMessageBox"}}));
            return !ui["activeModal"].toString().isEmpty();
        })());
        QCOMPARE(
            processCommand(process, "ui_input",
                           {{"target", ui["activeModal"]}, {"action", "key"}, {"key", "Return"}})["state"]
                .toString(),
            "done");
        QVERIFY(data(processCall(process, "get_ui"))["activeModal"].toString().isEmpty());
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
    }
    void guiProcessCalculationAndVibration() {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const auto modes = files.filePath("synthetic-modes.molden");
        writeFile(modes, SyntheticModes);
        QProcess process;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "offscreen");
        env.insert("MOLFORMA_RENDERER", "painter");
        env.insert("XDG_CONFIG_HOME", files.filePath("settings"));
        process.setProcessEnvironment(env);
        process.start(QCoreApplication::applicationDirPath() + "/molforma",
                      {"--mcp-gui", "--mcp-allow-execution-settings"});
        QVERIFY(process.waitForStarted());
        QVERIFY(!failed(processCall(
            process, "import_document",
            {{"format", "xyz"}, {"text", "2\nH2\nH 0 0 0\nH 0 0 .74\n"}, {"preview", false}}, true)));
        const QJsonValue project = data(processCall(process, "get_document"))["project"];
        // Real dialogs over stdio, without mouse/keyboard or OS automation.
        const QJsonValue opening =
            data(processCall(process, "ui_activate", {{"target", "resultsOpen"}}, true))["commandId"];
        QJsonObject dialog;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                     const auto ui =
                                         data(processCall(process, "get_ui", {{"query", "QFileDialog"}}));
                                     for (const auto &v : ui["items"].toArray())
                                         if (v.toObject()["class"] == "QFileDialog") {
                                             dialog = v.toObject();
                                             return true;
                                         }
                                     return false;
                                 })(),
                                 5000);
        QCOMPARE(data(processCall(process, "get_ui_command", {{"commandId", opening}}))["state"].toString(),
                 "running");
        QCOMPARE(processCommand(process, "ui_file_dialog",
                                {{"target", dialog["target"]}, {"path", modes}, {"accept", true}})["state"]
                     .toString(),
                 "done");
        QTRY_VERIFY2(
            data(processCall(process, "get_results"))["counts"].toObject()["modes"].toInt() == 2,
            qPrintable(QString::fromUtf8(QJsonDocument(processCall(process, "get_results")).toJson())));
        const auto parsed =
            data(processCall(process, "get_results", {{"section", "modes"}}))["items"].toArray();
        QCOMPARE(parsed[0].toObject()["frequency"].toDouble(), -100.0);
        QCOMPARE(parsed[1].toObject()["frequency"].toDouble(), 1500.0);
        QCOMPARE(processCommand(process, "results_action",
                                {{"action", "play_mode"}, {"index", 0}, {"amplitude", .4}})["state"]
                     .toString(),
                 "done");
        QVERIFY(data(processCall(process, "get_app_state"))["results"].toObject()["playing"].toBool());
        const auto screenshot = files.filePath("mode.png");
        auto shot = processCall(process, "capture_screenshot", {{"path", screenshot}, {"maxWidth", 800}});
        QVERIFY2(!failed(shot), qPrintable(QString::fromUtf8(QJsonDocument(shot).toJson())));
        QCOMPARE(shot["result"].toObject()["content"].toArray()[1].toObject()["type"].toString(), "image");
        QVERIFY(QFileInfo(screenshot).size() > 100);
        QVERIFY(failed(processCall(process, "capture_screenshot", {{"path", screenshot}})));
        QCOMPARE(processCommand(process, "results_action", {{"action", "play_mode"}, {"index", 1}})["state"]
                     .toString(),
                 "done");
        QCOMPARE(data(processCall(process, "get_app_state"))["results"].toObject()["mode"].toInt(), 1);
        QCOMPARE(processCommand(process, "results_action", {{"action", "stop"}})["state"].toString(), "done");
        QVERIFY(!data(processCall(process, "get_app_state"))["results"].toObject()["playing"].toBool());
        QCOMPARE(processCommand(process, "results_action", {{"action", "play_mode"}, {"index", 99}})["state"]
                     .toString(),
                 "failed");
        QCOMPARE(data(processCall(process, "get_document"))["project"], project);
        const auto xyz = files.filePath("result.xyz");
        QCOMPARE(processCommand(process, "results_action", {{"action", "save_xyz"}, {"path", xyz}})["state"]
                     .toString(),
                 "done");
        QCOMPARE(Document::fromXyz(QString::fromUtf8(readFile(xyz))).atoms.size(), 2);
        QCOMPARE(
            processCommand(process, "results_action", {{"action", "select_orbital"}, {"index", 0}})["state"]
                .toString(),
            "done");
        QTRY_COMPARE(data(processCall(process, "get_results"))["selectedOrbital"].toInt(-1), 0);
        QVERIFY(data(processCall(process, "get_results"))["orbitalError"].toString().isEmpty());
        QCOMPARE(
            data(processCall(process, "get_ui_item", {{"target", "resultsOrbitals"}}))["currentRow"].toInt(),
            0);
        // Execution and history use the same immutable bundle and cancellable process as the GUI.
        const auto root = files.filePath("jobs");
        QVERIFY(!failed(processCall(
            process, "set_execution",
            {{"settings", QJsonObject{{"mode", "direct"},
                                      {"executable", QCoreApplication::applicationDirPath() + "/fake_nwchem"},
                                      {"job_root", root}}},
             {"preview", false}},
            true)));
        QCOMPARE(processCommand(process, "calculation", {{"action", "start"}})["state"].toString(), "done");
        QTRY_COMPARE(
            data(processCall(process, "get_app_state"))["calculation"].toObject()["state"].toString(),
            "finished");
        const auto directory =
            data(processCall(process, "get_app_state"))["calculation"].toObject()["directory"].toString();
        QVERIFY(QFileInfo::exists(directory + "/input.nw"));
        QCOMPARE(processCommand(process, "history_action", {{"action", "scan"}, {"path", root}})["state"]
                     .toString(),
                 "done");
        QTRY_VERIFY(!data(processCall(process, "get_app_state"))["history"].toObject()["scanning"].toBool());
        QCOMPARE(
            processCommand(process, "history_action", {{"action", "select"}, {"path", directory}})["state"]
                .toString(),
            "done");
        QCOMPARE(processCommand(process, "history_action",
                                {{"action", "save_csv"}, {"path", files.filePath("history.csv")}})["state"]
                     .toString(),
                 "done");
        QVERIFY(!failed(processCall(
            process, "set_execution",
            {{"settings", QJsonObject{{"environment", QJsonArray{QJsonObject{{"enabled", true},
                                                                             {"name", "MOLFORMA_FAKE_MODE"},
                                                                             {"value", "slow"}}}}}},
             {"preview", false}},
            true)));
        QCOMPARE(processCommand(process, "calculation", {{"action", "start"}})["state"].toString(), "done");
        QTRY_VERIFY(data(processCall(process, "get_app_state"))["calculation"].toObject()["active"].toBool());
        QCOMPARE(processCommand(process, "calculation", {{"action", "stop"}})["state"].toString(), "done");
        QTRY_COMPARE(
            data(processCall(process, "get_app_state"))["calculation"].toObject()["state"].toString(),
            "cancelled");
        QCOMPARE(
            processCommand(process, "chemistry", {{"action", "smiles"}, {"smiles", "O"}})["state"].toString(),
            "done");
        QTRY_VERIFY(data(processCall(process, "get_app_state"))["candidate"].toBool());
        QVERIFY(!failed(processCall(process, "history", {{"action", "cancel"}}, true)));
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 0);
    }
    void legacyProtocol() {
        McpServer server;
        QJsonObject params{{"protocolVersion", "2099-01-01"},
                           {"capabilities", QJsonObject{}},
                           {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}};
        const auto init = server.receive(message("initialize", params, 1, false));
        QCOMPARE(init["result"].toObject()["protocolVersion"].toString(), QString(McpLegacyVersion));
        QVERIFY(!init["result"].toObject().contains("resultType"));
        QVERIFY(!init["result"].toObject().contains("ttlMs"));
        QVERIFY(server.receive(message("tools/list", {}, 2, false)).contains("error"));
        QVERIFY(
            server.receive(message("notifications/initialized", {}, QJsonValue::Undefined, false)).isEmpty());
        const auto legacyList = server.receive(message("tools/list", {}, 3, false));
        QVERIFY(!legacyList.contains("error"));
        QVERIFY(!legacyList["result"].toObject().contains("cacheScope"));
        // Interleaving modern requests never changes the legacy connection's negotiated version.
        QVERIFY(!server.receive(message("server/discover")).contains("error"));
        QVERIFY(!server.receive(message("ping", {}, 4, false))["result"].toObject().contains("resultType"));
        QVERIFY(server.receive(message("initialize", params, 5, false)).contains("error"));
        QVERIFY(server.receive("{").contains("id"));
        QVERIFY(server.receive("{")["id"].isNull());
    }
    void invalidMessagesAndNotifications() {
        McpServer server;
        QCOMPARE(server.receive("{")["error"].toObject()["code"].toInt(), -32700);
        QVERIFY(!server.receive("{").contains("id"));
        for (const QByteArray bytes : {QByteArray("[]"), QByteArray("{}"), message("ping", {}, QJsonValue()),
                                       message("ping", {}, 1.5), message("ping", {}, true)})
            QCOMPARE(server.receive(bytes)["error"].toObject()["code"].toInt(), -32600);
        QVERIFY(server.receive(message("unknown/notification", {}, QJsonValue::Undefined)).isEmpty());
        // Requests sent without IDs must not execute mutating tools.
        QVERIFY(server
                    .receive(message(
                        "tools/call",
                        {{"name", "add_atom"},
                         {"arguments",
                          scoped(server, {{"element", "C"}, {"position", QJsonArray{0, 0, 0}}}, true)}},
                        QJsonValue::Undefined))
                    .isEmpty());
        QCOMPARE(committed(server).atoms.size(), 0);
        QVERIFY(server.receive(message("notifications/cancelled", {{"requestId", 99}}, QJsonValue::Undefined))
                    .isEmpty());
        QCOMPARE(call(server, "no_such_tool")["error"].toObject()["code"].toInt(), -32602);
        auto response = change(server, "add_atom", {{"element", "C"}, {"position", "0,0,0"}});
        QVERIFY(response["result"].toObject()["isError"].toBool());
        QVERIFY(!response.contains("error"));
        QVERIFY(failed(change(server, "add_atom",
                              {{"element", "C"}, {"position", QJsonArray{0, 0, 0}}, {"preview", "false"}})));
    }
    void candidateRevisionAndUndo() {
        McpServer server;
        const QJsonValue initial = server.snapshot().value("revision");
        auto placed = change(server, "place_template",
                             {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}});
        QVERIFY2(!failed(placed), qPrintable(QString::fromUtf8(QJsonDocument(placed).toJson())));
        QCOMPARE(committed(server).atoms.size(), 0);
        // Edit results summarize the candidate instead of repeating the whole project.
        QVERIFY(!data(placed).contains("project"));
        QCOMPARE(data(placed)["revision"], server.snapshot()["revision"]);
        const auto summary = data(placed)["candidate"].toObject();
        QCOMPARE(summary["formula"].toString(), QString("H2O"));
        QCOMPARE(summary["addedIds"].toArray().size(), 3);
        QVERIFY(summary["removedIds"].toArray().isEmpty());
        QCOMPARE(Document::fromProject(server.snapshot()["candidate"].toObject()).formula(), QString("H2O"));
        const auto full = data(call(server, "get_document", scoped(server)));
        QVERIFY(full["project"].isObject() && full["candidate"].isObject());
        const auto light = data(call(server, "get_document", scoped(server, {{"include", QJsonArray{}}})));
        QVERIFY(!light.contains("project") && !light.contains("candidate"));
        QVERIFY(light["hasCandidate"].toBool());
        QCOMPARE(light["revision"], full["revision"]);
        const auto onlyCandidate =
            data(call(server, "get_document", scoped(server, {{"include", QJsonArray{"candidate"}}})));
        QVERIFY(!onlyCandidate.contains("project") && onlyCandidate["candidate"].isObject());
        QVERIFY(failed(call(server, "get_document", scoped(server, {{"include", QJsonArray{"atoms"}}}))));
        QVERIFY(failed(call(
            server, "history",
            {{"documentId", server.documentId()}, {"expectedRevision", initial}, {"action", "confirm"}})));
        QVERIFY(failed(change(server, "place_template",
                              {{"templateId", "skeleton:water"}, {"position", QJsonArray{4, 0, 0}}})));
        QVERIFY(!failed(change(server, "history", {{"action", "confirm"}})));
        auto water = committed(server);
        QCOMPARE(water.formula(), QString("H2O"));
        auto id = water.atoms.first().id;
        QVERIFY(
            !failed(change(server, "translate",
                           {{"ids", QJsonArray{id}}, {"offset", QJsonArray{1, 0, 0}}, {"preview", false}})));
        QCOMPARE(committed(server).atom(id).position.x(), water.atom(id).position.x() + 1);
        QVERIFY(!failed(change(server, "history", {{"action", "undo"}})));
        QCOMPARE(committed(server).atom(id).position, water.atom(id).position);
        QVERIFY(!failed(change(server, "history", {{"action", "redo"}})));
        const auto deleted = change(server, "delete_atoms", {{"ids", QJsonArray{id}}});
        QVERIFY(!failed(deleted));
        QCOMPARE(data(deleted)["candidate"].toObject()["removedIds"].toArray(), QJsonArray{id});
        QVERIFY(!failed(change(server, "history", {{"action", "cancel"}})));
        QCOMPARE(committed(server).atoms.size(), 3);
    }
    void atomicEditsAndFixedAtoms() {
        McpServer server;
        Document d;
        placeTemplate(d, findTemplate("skeleton:water"), Vec::Zero());
        d.atoms[0].fixed = true;
        d.envelope["future_field"] = "preserve me";
        QVERIFY(!failed(change(server, "import_document",
                               {{"format", "project"},
                                {"text", QString::fromUtf8(QJsonDocument(d.project()).toJson())},
                                {"preview", false}})));
        auto before = server.snapshot();
        auto positions = QJsonArray{QJsonObject{{"id", d.atoms[1].id}, {"position", QJsonArray{4, 5, 6}}},
                                    QJsonObject{{"id", d.atoms[0].id}, {"position", QJsonArray{1, 2, 3}}}};
        QVERIFY(failed(change(server, "set_positions", {{"positions", positions}, {"preview", false}})));
        QCOMPARE(server.snapshot(), before);
        QVERIFY(failed(change(
            server, "translate",
            {{"ids", QJsonArray{d.atoms[0].id}}, {"offset", QJsonArray{1, 2, 3}}, {"preview", false}})));
        QVERIFY(failed(change(server, "delete_atoms", {{"ids", QJsonArray{"missing"}}})));
        QVERIFY(failed(change(
            server, "set_positions",
            {{"positions", QJsonArray{QJsonObject{{"id", d.atoms[1].id},
                                                  {"position", vectorToJson(d.atoms[2].position)}}}}})));
        QCOMPARE(server.snapshot(), before);
        QVERIFY(!failed(change(
            server, "set_positions",
            {{"positions", QJsonArray{QJsonObject{{"id", d.atoms[1].id}, {"position", QJsonArray{4, 5, 6}}}}},
             {"preview", false}})));
        QCOMPARE(committed(server).envelope["future_field"].toString(), QString("preserve me"));
    }
    void filesResourcesAndNwchem() {
        McpServer server;
        QVERIFY(!failed(change(
            server, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        auto configure =
            change(server, "set_nwchem",
                   {{"settings", QJsonObject{{"method", "hf"}, {"basis", "sto-3g"}}}, {"preview", false}});
        QVERIFY2(!failed(configure), qPrintable(QString::fromUtf8(QJsonDocument(configure).toJson())));
        auto exported = call(server, "export_document", scoped(server, {{"format", "nwchem"}}));
        QVERIFY(!failed(exported));
        QVERIFY(data(exported)["text"].toString().contains("task scf energy"));
        auto before = server.snapshot();
        QVERIFY(failed(change(server, "set_nwchem", {{"settings", QJsonObject{{"basis", "bad\ninput"}}}})));
        QCOMPARE(server.snapshot(), before);
        auto measured =
            call(server, "measure",
                 scoped(server,
                        {{"ids", QJsonArray{committed(server).atoms[0].id, committed(server).atoms[1].id}}}));
        QVERIFY(data(measured)["value"].toDouble() > 0);
        auto listing = server.receive(message("resources/list"));
        const QJsonValue uri = listing["result"].toObject()["resources"].toArray()[0].toObject().value("uri");
        auto resource = server.receive(message("resources/read", {{"uri", uri}}));
        QCOMPARE(resource["result"].toObject()["ttlMs"].toInt(-1), 0);
        QCOMPARE(resource["result"].toObject()["cacheScope"].toString(), QString("private"));
        auto resourceText =
            resource["result"].toObject()["contents"].toArray()[0].toObject()["text"].toString();
        QCOMPARE(QJsonDocument::fromJson(resourceText.toUtf8()).object(), server.snapshot());
        QCOMPARE(server.receive(message("resources/read", {{"uri", "file:///etc/passwd"}}))["error"]
                     .toObject()["code"]
                     .toInt(),
                 -32602);
        QTemporaryDir directory;
        auto path = directory.filePath("molecule.kgui.json");
        QVERIFY(!failed(change(server, "save_file", {{"path", path}})));
        QCOMPARE(Document::load(path).toJson(), committed(server).toJson());
        QVERIFY(failed(change(server, "save_file", {{"path", path}})));
        QVERIFY(!failed(change(server, "save_file", {{"path", path}, {"overwrite", true}})));
        QVERIFY(!failed(change(server, "open_file", {{"path", path}})));
        auto templates = data(call(server, "list_templates"))["templates"].toArray();
        QVERIFY(!templates.isEmpty());
        QVERIFY(!templates[0].toObject()["name"].toString().isEmpty());
    }
    void guiIntegration() {
        MainWindow window;
        window.loadDocument(Document());
        McpEditor hooks{[&] { return window.document(); },
                        [&]() -> std::optional<Document> {
                            return window.hasCandidate() ? std::optional(window.displayDocument())
                                                         : std::nullopt;
                        },
                        [&](const QString &name, const std::function<void(Document &)> &edit, bool preview) {
                            window.edit(name, edit, preview);
                        },
                        [&](const QString &action) { window.remoteHistory(action); }};
        McpServer server(hooks);
        QVERIFY(!failed(change(server, "place_template",
                               {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}})));
        QVERIFY(window.hasCandidate());
        QCOMPARE(window.document().atoms.size(), 0);
        QVERIFY(!failed(change(server, "history", {{"action", "confirm"}})));
        QCOMPARE(window.document().atoms.size(), 3);
        QVERIFY(window.history()->canUndo());
        const QJsonValue stale = server.snapshot().value("revision");
        window.edit("User edit", [](Document &d) { d.atoms[0].position.x() += 2; });
        QVERIFY(failed(
            call(server, "history",
                 {{"documentId", server.documentId()}, {"expectedRevision", stale}, {"action", "undo"}})));
        QVERIFY(!failed(change(server, "history", {{"action", "undo"}})));
        QCOMPARE(window.document().atoms.size(), 3);
        // Envelope-only replacements must also participate in candidates and GUI undo.
        auto project = window.document().project();
        project["future_field"] = "new metadata";
        QVERIFY(!failed(change(server, "import_document",
                               {{"format", "project"},
                                {"text", QString::fromUtf8(QJsonDocument(project).toJson())},
                                {"preview", false}})));
        QCOMPARE(window.document().envelope["future_field"].toString(), QString("new metadata"));
        QVERIFY(!failed(change(server, "history", {{"action", "undo"}})));
        QVERIFY(!window.document().envelope.contains("future_field"));
    }
    void toolMetadataAndPrompts() {
        MainWindow w;
        McpGui gui(w);
        McpServer headless, guiServer(gui.editor());
        for (auto *server : {&headless, &guiServer})
            for (const auto &tool :
                 server->receive(message("tools/list"))["result"].toObject()["tools"].toArray()) {
                const auto t = tool.toObject();
                QVERIFY2(!t["title"].toString().isEmpty(), qPrintable(t["name"].toString()));
                QVERIFY(t["outputSchema"].isObject());
            }
        const auto names = [](McpServer &server) {
            QStringList list;
            for (const auto &p :
                 server.receive(message("prompts/list"))["result"].toObject()["prompts"].toArray())
                list << p.toObject()["name"].toString();
            return list;
        };
        QCOMPARE(names(headless), QStringList({"review_candidate", "geometry_optimization"}));
        // The headless runtime adds calculation and result tools, so frequency analysis is offered.
        McpHeadless runtime({});
        McpServer headlessRuntime(runtime.editor());
        runtime.attach(headlessRuntime);
        QCOMPARE(names(headlessRuntime),
                 QStringList({"review_candidate", "geometry_optimization", "frequency_analysis"}));
        // Claude Code hints: large documents stay inline; running a program needs a person.
        for (auto *server : {&headless, &guiServer, &headlessRuntime})
            for (const auto *name : {"get_document", "export_document"})
                QCOMPARE(toolNamed(*server, name)["_meta"].toObject()["anthropic/maxResultSizeChars"].toInt(),
                         100000);
        // Only when MCP may choose the program does each run need a person's approval.
        McpPolicy permissive;
        permissive.allowExecutionSettings = true;
        McpGui permissiveGui(w, permissive);
        McpServer permissiveServer(permissiveGui.editor(), permissive);
        for (auto *server : {&guiServer, &headlessRuntime}) {
            QCOMPARE(
                toolNamed(*server, "get_results")["_meta"].toObject()["anthropic/maxResultSizeChars"].toInt(),
                100000);
            QVERIFY(!toolNamed(*server, "calculation")["_meta"].toObject().contains(
                "anthropic/requiresUserInteraction"));
        }
        QVERIFY(toolNamed(permissiveServer, "calculation")["_meta"]
                    .toObject()["anthropic/requiresUserInteraction"]
                    .toBool());
        QCOMPARE(headless.receive(message("server/discover"))["result"]
                     .toObject()["_meta"]
                     .toObject()["io.modelcontextprotocol/serverInfo"]
                     .toObject()["version"]
                     .toString(),
                 QString(MOLFORMA_VERSION));
        QVERIFY(names(guiServer).contains("build_from_smiles") &&
                names(guiServer).contains("frequency_analysis"));
        const auto prompt = headless.receive(message(
            "prompts/get",
            {{"name", "geometry_optimization"},
             {"arguments", QJsonObject{{"method", "dft"}, {"basis", "6-31g*"}, {"frequencies", "yes"}}}}));
        const auto text = prompt["result"]
                              .toObject()["messages"]
                              .toArray()[0]
                              .toObject()["content"]
                              .toObject()["text"]
                              .toString();
        QVERIFY(text.contains("\"task\": \"optimize_frequencies\"") &&
                text.contains("\"basis\": \"6-31g*\""));
        QVERIFY(text.contains("export_document"));
        const auto guiText =
            guiServer
                .receive(message("prompts/get", {{"name", "build_from_smiles"},
                                                 {"arguments", QJsonObject{{"smiles", "C\"O"}}}}))["result"]
                .toObject()["messages"]
                .toArray()[0]
                .toObject()["content"]
                .toObject()["text"]
                .toString();
        QVERIFY(guiText.contains("\"C\\\"O\""));
        for (const auto &bad :
             {QJsonObject{{"name", "missing"}}, QJsonObject{{"name", "build_from_smiles"}},
              QJsonObject{{"name", "geometry_optimization"}, {"arguments", QJsonObject{{"method", 1}}}}})
            QCOMPARE(guiServer.receive(message("prompts/get", bad))["error"].toObject()["code"].toInt(),
                     -32602);
        // Legacy clients see the same prompts without cache hints.
        McpServer legacy;
        legacy.receive(message("initialize",
                               {{"protocolVersion", McpLegacyVersion},
                                {"capabilities", QJsonObject{}},
                                {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}},
                               1, false));
        legacy.receive(message("notifications/initialized", {}, QJsonValue::Undefined, false));
        const auto legacyPrompts = legacy.receive(message("prompts/list", {}, 2, false))["result"].toObject();
        QCOMPARE(legacyPrompts["prompts"].toArray().size(), 2);
        QVERIFY(!legacyPrompts.contains("ttlMs"));
    }
    void saveFileElicitsOverwrite() {
        McpServer server;
        QVERIFY(!failed(change(
            server, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        QTemporaryDir directory;
        const auto path = directory.filePath("existing.kgui.json");
        writeFile(path, "old");
        const auto save = [&](const QJsonObject &caps, QJsonObject params = {}) {
            params["name"] = "save_file";
            params["arguments"] = scoped(server, {{"path", path}}, true);
            return server.receive(capable("tools/call", params, caps));
        };
        // Without elicitation the explicit overwrite flag is still required.
        QVERIFY(failed(save({})));
        const auto asked = save(clientCapabilities(true, false))["result"].toObject();
        QCOMPARE(asked["resultType"].toString(), QString("input_required"));
        const auto request = asked["inputRequests"].toObject()["overwrite"].toObject();
        QCOMPARE(request["method"].toString(), QString("elicitation/create"));
        QCOMPARE(request["params"].toObject()["mode"].toString(), QString("form"));
        const auto state = asked["requestState"].toString();
        QVERIFY(!state.isEmpty());
        QCOMPARE(readFile(path), QByteArray("old"));
        const auto accept = QJsonObject{
            {"overwrite", QJsonObject{{"action", "accept"}, {"content", QJsonObject{{"overwrite", true}}}}}};
        // Tampered state and missing responses ask again instead of writing.
        auto tampered = state;
        tampered[0] = tampered[0] == 'A' ? 'B' : 'A';
        QCOMPARE(save(clientCapabilities(true, false),
                      {{"inputResponses", accept}, {"requestState", tampered}})["result"]
                     .toObject()["resultType"]
                     .toString(),
                 QString("input_required"));
        QCOMPARE(save(clientCapabilities(true, false), {{"requestState", state}})["result"]
                     .toObject()["resultType"]
                     .toString(),
                 QString("input_required"));
        const auto declined =
            save(clientCapabilities(true, false),
                 {{"inputResponses", QJsonObject{{"overwrite", QJsonObject{{"action", "decline"}}}}},
                  {"requestState", state}});
        QVERIFY(failed(declined));
        QCOMPARE(readFile(path), QByteArray("old"));
        const auto saved =
            save(clientCapabilities(true, false), {{"inputResponses", accept}, {"requestState", state}});
        QVERIFY2(!failed(saved), QJsonDocument(saved).toJson().constData());
        QCOMPARE(saved["result"].toObject()["resultType"].toString(), QString("complete"));
        QCOMPARE(Document::load(path).toJson(), committed(server).toJson());
        // A state is bound to the document revision it was issued for.
        QVERIFY(!failed(change(server, "translate",
                               {{"ids", QJsonArray{committed(server).atoms[0].id}},
                                {"offset", QJsonArray{1, 0, 0}},
                                {"preview", false}})));
        QCOMPARE(save(clientCapabilities(true, false),
                      {{"inputResponses", accept}, {"requestState", state}})["result"]
                     .toObject()["resultType"]
                     .toString(),
                 QString("input_required"));
        QVERIFY(failed(server.receive(
            capable("tools/call",
                    {{"name", "save_file"},
                     {"arguments", scoped(server, {{"path", path}, {"overwrite", false}}, true)}},
                    clientCapabilities(true, false)))));
        QCOMPARE(server
                     .receive(capable("tools/call", {{"name", "list_documents"}, {"inputResponses", "x"}},
                                      clientCapabilities(true, false)))["error"]
                     .toObject()["code"]
                     .toInt(),
                 -32602);
    }
    void tasksAnswerGuiDialogs() {
        MainWindow w;
        w.show();
        McpGui gui(w);
        McpServer s(gui.editor());
        QVERIFY(s.receive(message("server/discover"))["result"]
                    .toObject()["capabilities"]
                    .toObject()["extensions"]
                    .toObject()
                    .contains(McpTasksExtension));
        QPushButton open("Open a modal", &w);
        open.setObjectName("testTaskModal");
        int answered = -1;
        QString seenKey, plainMessage, taskId;
        connect(&open, &QPushButton::clicked, &w, [&] {
            QMessageBox dialog(QMessageBox::Question, "MCP task test", "Replace the selected structure?",
                               QMessageBox::Yes | QMessageBox::No, &w);
            QTimer::singleShot(20, &dialog, [&] {
                // A client without elicitation is told how to answer through GUI tools instead.
                const auto plain = s.receive(capable("tasks/get", {{"taskId", taskId}},
                                                     clientCapabilities(false, true)))["result"]
                                       .toObject();
                QCOMPARE(plain["status"].toString(), QString("working"));
                plainMessage = plain["statusMessage"].toString();
                const auto polled = s.receive(capable("tasks/get", {{"taskId", taskId}},
                                                      clientCapabilities(true, true)))["result"]
                                        .toObject();
                QCOMPARE(polled["status"].toString(), QString("input_required"));
                const auto requests = polled["inputRequests"].toObject();
                QCOMPARE(requests.size(), 1);
                seenKey = requests.keys().first();
                const auto params = requests[seenKey].toObject()["params"].toObject();
                QVERIFY(params["message"].toString().contains("Replace the selected structure?"));
                const auto choices = params["requestedSchema"]
                                         .toObject()["properties"]
                                         .toObject()["choice"]
                                         .toObject()["enum"]
                                         .toArray();
                QVERIFY(choices.contains("No"));
                const auto ack = s.receive(capable(
                    "tasks/update",
                    {{"taskId", taskId},
                     {"inputResponses",
                      QJsonObject{{"unknown", QJsonObject{{"action", "accept"}}},
                                  {seenKey, QJsonObject{{"action", "accept"},
                                                        {"content", QJsonObject{{"choice", "No"}}}}}}}},
                    clientCapabilities(true, true)));
                QCOMPARE(ack["result"].toObject()["resultType"].toString(), QString("complete"));
            });
            QTimer::singleShot(5000, &dialog, &QDialog::reject);
            answered = dialog.exec();
        });
        const auto created = s.receive(capable(
            "tools/call",
            {{"name", "ui_activate"}, {"arguments", scoped(s, {{"target", "testTaskModal"}}, true)}},
            clientCapabilities(true, true)))["result"]
                                 .toObject();
        QCOMPARE(created["resultType"].toString(), QString("task"));
        QCOMPARE(created["status"].toString(), QString("working"));
        QVERIFY(created["ttlMs"].isNull() && created["pollIntervalMs"].toInt() > 0);
        taskId = created["taskId"].toString();
        QTRY_COMPARE(answered, int(QMessageBox::No));
        QVERIFY(plainMessage.contains("confirmation dialog"));
        QJsonObject finished;
        QTRY_COMPARE((finished = s.receive(capable("tasks/get", {{"taskId", taskId}},
                                                   clientCapabilities(true, true)))["result"]
                                     .toObject())["status"]
                         .toString(),
                     QString("completed"));
        const auto result = finished["result"].toObject();
        QVERIFY(!result["isError"].toBool());
        QCOMPARE(result["structuredContent"].toObject()["state"].toString(), QString("done"));
        QCOMPARE(finished["resultType"].toString(), QString("complete"));
        QVERIFY(!finished["lastUpdatedAt"].toString().isEmpty());
        // tasks/cancel acknowledges; clients without the extension or unknown IDs are rejected.
        QCOMPARE(s.receive(capable("tasks/cancel", {{"taskId", taskId}},
                                   clientCapabilities(false, true)))["result"]
                     .toObject()["resultType"]
                     .toString(),
                 QString("complete"));
        QCOMPARE(
            s.receive(capable("tasks/get", {{"taskId", taskId}}, {}))["error"].toObject()["code"].toInt(),
            -32021);
        QCOMPARE(
            s.receive(capable("tasks/get", {{"taskId", "nope"}}, clientCapabilities(false, true)))["error"]
                .toObject()["code"]
                .toInt(),
            -32602);
        // Without the extension the same tool keeps returning a pollable commandId.
        const auto plain = data(change(s, "set_selection", {{"ids", QJsonArray{}}}));
        QCOMPARE(plain["state"].toString(), QString("queued"));
    }
    void subprocessTaskTracksCalculation() {
        QTemporaryDir files;
        QProcess process;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "offscreen");
        env.insert("MOLFORMA_RENDERER", "painter");
        env.insert("XDG_CONFIG_HOME", files.filePath("settings"));
        process.setProcessEnvironment(env);
        process.start(QCoreApplication::applicationDirPath() + "/molforma",
                      {"--mcp-gui", "--mcp-allow-execution-settings"});
        QVERIFY(process.waitForStarted());
        QVERIFY(!failed(processCall(
            process, "import_document",
            {{"format", "xyz"}, {"text", "2\nH2\nH 0 0 0\nH 0 0 .74\n"}, {"preview", false}}, true)));
        const auto configure = [&](const QString &mode) {
            QVERIFY(!failed(processCall(
                process, "set_execution",
                {{"settings",
                  QJsonObject{{"mode", "direct"},
                              {"executable", QCoreApplication::applicationDirPath() + "/fake_nwchem"},
                              {"job_root", files.filePath("jobs")},
                              {"environment", QJsonArray{QJsonObject{{"enabled", !mode.isEmpty()},
                                                                     {"name", "MOLFORMA_FAKE_MODE"},
                                                                     {"value", mode}}}}}},
                 {"preview", false}},
                true)));
        };
        const auto caps = clientCapabilities(true, true);
        const auto start = [&] {
            process.write(message("tools/call", {{"name", "list_documents"}, {"arguments", QJsonObject{}}}) +
                          '\n');
            const auto doc = data(readReply(process))["documents"].toArray()[0].toObject();
            process.write(capable("tools/call",
                                  {{"name", "calculation"},
                                   {"arguments", QJsonObject{{"documentId", doc["documentId"]},
                                                             {"expectedRevision", doc["revision"]},
                                                             {"action", "start"}}}},
                                  caps) +
                          '\n');
            return readReply(process)["result"].toObject();
        };
        const auto poll = [&](const QString &taskId) {
            process.write(capable("tasks/get", {{"taskId", taskId}}, caps) + '\n');
            return readReply(process)["result"].toObject();
        };
        const auto finish = [&](const QString &taskId) {
            QJsonObject task;
            QElapsedTimer timer;
            timer.start();
            while ((task = poll(taskId))["status"] != "completed" && timer.elapsed() < 15000)
                QTest::qWait(50);
            return task;
        };
        // The task completes when NWChem stops, not when the start button returns.
        configure({});
        const auto created = start();
        QCOMPARE(created["resultType"].toString(), QString("task"));
        auto task = finish(created["taskId"].toString());
        QCOMPARE(task["status"].toString(), QString("completed"));
        auto calculation = task["result"]
                               .toObject()["structuredContent"]
                               .toObject()["app"]
                               .toObject()["calculation"]
                               .toObject();
        QCOMPARE(calculation["state"].toString(), QString("finished"));
        QVERIFY(!calculation["active"].toBool());
        // A long run stays working until the GUI calculation is stopped.
        configure("slow");
        const auto slow = start()["taskId"].toString();
        QElapsedTimer timer;
        timer.start();
        while (!(task = poll(slow))["statusMessage"].toString().contains("background") &&
               timer.elapsed() < 10000)
            QTest::qWait(50);
        QCOMPARE(task["status"].toString(), QString("working"));
        // tasks/cancel stops the calculation the task started; the task then reports cancelled.
        process.write(capable("tasks/cancel", {{"taskId", slow}}, caps) + '\n');
        QCOMPARE(readReply(process)["result"].toObject()["resultType"].toString(), QString("complete"));
        timer.restart();
        while ((task = poll(slow))["status"] != "cancelled" && timer.elapsed() < 15000)
            QTest::qWait(50);
        QCOMPARE(task["status"].toString(), QString("cancelled"));
        QCOMPARE(data(processCall(process, "get_app_state"))["calculation"].toObject()["state"].toString(),
                 QString("cancelled"));
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(10000));
    }
    void executionSettingsAreProtected() {
        McpServer server;
        QVERIFY(!failed(change(
            server, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        const auto rejected =
            change(server, "set_execution",
                   {{"settings", QJsonObject{{"executable", "/bin/sh"}}}, {"preview", false}});
        QVERIFY(failed(rejected));
        QVERIFY(text(rejected).contains("cannot be changed over MCP"));
        QVERIFY(failed(change(
            server, "set_execution",
            {{"settings", QJsonObject{{"environment", QJsonArray{QJsonObject{{"enabled", true},
                                                                             {"name", "LD_PRELOAD"},
                                                                             {"value", "/tmp/x.so"}}}}}},
             {"preview", false}})));
        // Other launch settings, and protected values left unchanged, remain editable.
        QVERIFY(!failed(change(
            server, "set_execution",
            {{"settings", QJsonObject{{"mpi_ranks", 4}, {"executable", "nwchem"}}}, {"preview", false}})));
        QCOMPARE(executionSettings(committed(server))["mpi_ranks"].toInt(), 4);
        const auto policy = data(call(server, "get_execution", scoped(server)))["policy"].toObject();
        QVERIFY(!policy["allowExecutionSettings"].toBool() && policy["allowedDirectories"].isNull());
        QVERIFY(!toolNamed(server, "set_execution")["_meta"].toObject().contains(
            "anthropic/requiresUserInteraction"));
        McpPolicy permissive;
        permissive.allowExecutionSettings = true;
        McpServer allowed({}, permissive);
        QVERIFY(!failed(change(
            allowed, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        QVERIFY(!failed(change(allowed, "set_execution",
                               {{"settings", QJsonObject{{"executable", "/bin/sh"}}}, {"preview", false}})));
        QVERIFY(toolNamed(allowed, "set_execution")["_meta"]
                    .toObject()["anthropic/requiresUserInteraction"]
                    .toBool());
    }
    void guiExecutionGuard() {
        QTemporaryDir jobs;
        MainWindow w;
        w.show();
        McpGui gui(w);
        McpServer s(gui.editor());
        // The program, launcher and environment controls cannot be operated over MCP.
        QVERIFY(
            failed(change(s, "ui_set_value", {{"target", "execution_executable"}, {"value", "/bin/sh"}})));
        QVERIFY(failed(change(s, "ui_activate", {{"target", "executionAddEnvironment"}})));
        const auto listed = data(call(
            s, "get_ui", scoped(s, {{"query", "execution_executable"}, {"includeHidden", true}})))["items"]
                                .toArray();
        QVERIFY(!listed.isEmpty());
        QVERIFY(listed[0].toObject()["blocked"].toString().contains("cannot be changed over MCP"));
        QVERIFY(!failed(change(
            s, "set_execution",
            {{"settings", QJsonObject{{"mode", "direct"}, {"job_root", jobs.path()}}}, {"preview", false}})));
        // A project loaded over MCP may name another program; MCP cannot run it.
        auto project = w.document();
        const auto fake = QCoreApplication::applicationDirPath() + "/fake_nwchem";
        setExecutionSetting(project, "executable", fake);
        QVERIFY(!failed(change(s, "import_document",
                               {{"format", "project"},
                                {"text", QString::fromUtf8(QJsonDocument(project.project()).toJson())},
                                {"preview", false}})));
        const QJsonValue refused = data(change(s, "calculation", {{"action", "start"}}))["commandId"];
        QJsonObject command;
        QTRY_COMPARE((command = data(call(s, "get_ui_command", scoped(s, {{"commandId", refused}}))))["state"]
                         .toString(),
                     QString("failed"));
        QVERIFY(command["error"].toString().contains("MCPからはこの設定で実行できません"));
        QVERIFY(!data(call(s, "get_app_state", scoped(s)))["calculation"].toObject()["active"].toBool());
        // A person starting it confirms in a dialog that MCP can neither answer nor elicit.
        auto *start = w.findChild<QPushButton *>("executionStart");
        QVERIFY(start && start->isEnabled());
        bool protectedDialog = false;
        const auto answer = [&](QMessageBox::StandardButton button) {
            QTimer::singleShot(50, &w, [&, button] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box);
                protectedDialog = box->property("mcpProtected").toBool();
                box->button(QMessageBox::Yes)->setObjectName("guardYes");
                QVERIFY(failed(change(s, "ui_activate", {{"target", "guardYes"}})));
                box->button(button)->click();
            });
        };
        answer(QMessageBox::No);
        start->click();
        QVERIFY(protectedDialog);
        QVERIFY(!data(call(s, "get_app_state", scoped(s)))["calculation"].toObject()["active"].toBool());
        answer(QMessageBox::Yes);
        start->click();
        QTRY_COMPARE(data(call(s, "get_app_state", scoped(s)))["calculation"].toObject()["state"].toString(),
                     QString("finished"));
        // Once a person accepted these settings, MCP may start runs with them.
        const QJsonValue accepted = data(change(s, "calculation", {{"action", "start"}}))["commandId"];
        QTRY_COMPARE(
            data(call(s, "get_ui_command", scoped(s, {{"commandId", accepted}})))["state"].toString(),
            QString("done"));
        QTRY_COMPARE(data(call(s, "get_app_state", scoped(s)))["calculation"].toObject()["state"].toString(),
                     QString("finished"));
    }
    void directoryRestriction() {
        QTemporaryDir allowed, outside;
        McpPolicy policy;
        policy.allowDirectory(allowed.path());
        const auto rejects = [&](const QString &path, const QString &base = {}) {
            try {
                policy.checkPath(path, base);
                return false;
            } catch (const std::exception &) {
                return true;
            }
        };
        QVERIFY(!rejects(allowed.filePath("new/sub/file.json")));
        QVERIFY(!rejects("relative.json", allowed.path()));
        QVERIFY(rejects(allowed.filePath("../escape.json")));
        QVERIFY(rejects(allowed.path() + "-sibling/file"));
        QVERIFY(rejects(outside.filePath("file.json")));
        QVERIFY(QFile::link(outside.path(), allowed.filePath("link")));
        QVERIFY(rejects(allowed.filePath("link/file.json")));
        QVERIFY(QFile::link(allowed.filePath("missing-target"), allowed.filePath("dangling")));
        QVERIFY(rejects(allowed.filePath("dangling")));
        McpServer server({}, policy);
        QVERIFY(!failed(change(
            server, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        const auto inside = allowed.filePath("water.kgui.json"), copy = outside.filePath("water.kgui.json");
        QVERIFY(!failed(change(server, "save_file", {{"path", inside}})));
        QVERIFY(failed(change(server, "save_file", {{"path", copy}})));
        QVERIFY(failed(change(server, "save_file", {{"path", allowed.filePath("link/water.kgui.json")}})));
        QVERIFY(!QFile::exists(copy));
        QVERIFY(QFile::copy(inside, copy));
        QVERIFY(failed(change(server, "open_file", {{"path", copy}, {"preview", false}})));
        QVERIFY(!failed(change(server, "open_file", {{"path", inside}, {"preview", false}})));
        QVERIFY(failed(
            change(server, "set_execution", {{"settings", QJsonObject{{"job_root", outside.path()}}}})));
        QVERIFY(!failed(
            change(server, "set_execution",
                   {{"settings", QJsonObject{{"job_root", allowed.filePath("jobs")}}}, {"preview", false}})));
        QVERIFY(failed(
            change(server, "set_advanced", {{"settings", QJsonObject{{"scratch_dir", outside.path()}}}})));
        QCOMPARE(data(call(server, "get_execution", scoped(server)))["policy"]
                     .toObject()["allowedDirectories"]
                     .toArray(),
                 QJsonArray{QFileInfo(allowed.path()).canonicalFilePath()});
        // GUI paths are checked before a command is queued; file dialogs only via ui_file_dialog.
        MainWindow w;
        w.show();
        McpGui gui(w, policy);
        McpServer s(gui.editor(), policy);
        QVERIFY(failed(change(s, "results_action", {{"action", "open_file"}, {"path", copy}})));
        QVERIFY(failed(change(s, "history_action", {{"action", "scan"}, {"path", outside.path()}})));
        QVERIFY(failed(call(s, "capture_screenshot", scoped(s, {{"path", outside.filePath("view.png")}}))));
        QVERIFY(!QFile::exists(outside.filePath("view.png")));
        QVERIFY(failed(change(s, "ui_set_value", {{"target", "historyRoot"}, {"value", outside.path()}})));
        QFileDialog dialog(&w, "MCP test", allowed.path());
        dialog.setOption(QFileDialog::DontUseNativeDialog);
        dialog.setObjectName("testDialog");
        dialog.show();
        QVERIFY(failed(change(s, "ui_set_value", {{"target", "fileNameEdit"}, {"value", copy}})));
        QVERIFY(failed(
            change(s, "ui_file_dialog", {{"target", "testDialog"}, {"path", copy}, {"accept", true}})));
        QVERIFY(dialog.isVisible());
        const QJsonValue accepted = data(change(s, "ui_file_dialog",
                                                {{"target", "testDialog"},
                                                 {"path", allowed.filePath("chosen.txt")},
                                                 {"accept", true}}))["commandId"];
        QTRY_COMPARE(
            data(call(s, "get_ui_command", scoped(s, {{"commandId", accepted}})))["state"].toString(),
            QString("done"));
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void tasksCancelQueuedGuiCommand() {
        MainWindow w;
        McpGui gui(w);
        McpServer s(gui.editor());
        QPushButton button("Count", &w);
        button.setObjectName("cancelTarget");
        int clicks = 0;
        connect(&button, &QPushButton::clicked, &w, [&] { ++clicks; });
        const auto caps = clientCapabilities(false, true);
        const auto created = s.receive(capable(
            "tools/call",
            {{"name", "ui_activate"}, {"arguments", scoped(s, {{"target", "cancelTarget"}}, true)}},
            caps))["result"]
                                 .toObject();
        QCOMPARE(created["resultType"].toString(), QString("task"));
        // The command is withdrawn before the event loop runs it.
        QCOMPARE(s.receive(capable("tasks/cancel", {{"taskId", created["taskId"]}}, caps))["result"]
                     .toObject()["resultType"]
                     .toString(),
                 QString("complete"));
        QTest::qWait(50);
        QCOMPARE(clicks, 0);
        QCOMPARE(s.receive(capable("tasks/get", {{"taskId", created["taskId"]}}, caps))["result"]
                     .toObject()["status"]
                     .toString(),
                 QString("cancelled"));
        // Cancelling finished work changes nothing.
        const auto done = s.receive(capable(
            "tools/call",
            {{"name", "ui_activate"}, {"arguments", scoped(s, {{"target", "cancelTarget"}}, true)}},
            caps))["result"]
                              .toObject();
        QTRY_COMPARE(clicks, 1);
        s.receive(capable("tasks/cancel", {{"taskId", done["taskId"]}}, caps));
        QCOMPARE(s.receive(capable("tasks/get", {{"taskId", done["taskId"]}}, caps))["result"]
                     .toObject()["status"]
                     .toString(),
                 QString("completed"));
    }
    void resourcesForMentions() {
        McpServer server;
        const auto document = "molforma://documents/" + server.documentId();
        QStringList uris;
        for (const auto &r :
             server.receive(message("resources/list"))["result"].toObject()["resources"].toArray())
            uris << r.toObject()["uri"].toString();
        QCOMPARE(uris, QStringList({document, document + "/xyz", document + "/nwchem"}));
        // An empty document has no valid NWChem input.
        QCOMPARE(server.receive(message("resources/read", {{"uri", document + "/nwchem"}}))["error"]
                     .toObject()["code"]
                     .toInt(),
                 -32603);
        QVERIFY(!failed(change(
            server, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        QCOMPARE(
            resourceText(server, document + "/xyz"),
            data(call(server, "export_document", scoped(server, {{"format", "xyz"}})))["text"].toString());
        QVERIFY(resourceText(server, document + "/nwchem").contains("geometry"));
        QCOMPARE(server.receive(message("resources/read", {{"uri", "molforma://app/state"}}))["error"]
                     .toObject()["code"]
                     .toInt(),
                 -32602);
        // Both runtimes expose the calculation output and result views.
        MainWindow w;
        McpGui gui(w);
        McpServer s(gui.editor());
        McpHeadless runtime({});
        McpServer headless(runtime.editor());
        runtime.attach(headless);
        for (auto *runtimeServer : {&s, &headless}) {
            QStringList listed;
            for (const auto &r : runtimeServer->receive(message("resources/list"))["result"]
                                     .toObject()["resources"]
                                     .toArray())
                listed << r.toObject()["uri"].toString();
            for (const auto *uri : {"molforma://app/state", "molforma://calculation/output",
                                    "molforma://results/summary", "molforma://results/log"})
                QVERIFY2(listed.contains(uri), uri);
            QVERIFY(QJsonDocument::fromJson(resourceText(*runtimeServer, "molforma://app/state").toUtf8())
                        .object()
                        .contains("calculation"));
            QVERIFY(resourceText(*runtimeServer, "molforma://calculation/output").contains("No calculation"));
            QVERIFY(
                QJsonDocument::fromJson(resourceText(*runtimeServer, "molforma://results/summary").toUtf8())
                    .object()
                    .contains("counts"));
            QCOMPARE(resourceText(*runtimeServer, "molforma://results/log"), QString("No result is loaded."));
        }
    }
    void headlessCalculationAndResults() {
        QTemporaryDir files;
        const auto modes = files.filePath("modes.molden");
        writeFile(modes, SyntheticModes);
        const auto launch = [&](QProcess &process, const QStringList &extra) {
            auto env = QProcessEnvironment::systemEnvironment();
            env.remove("DISPLAY");
            env.remove("WAYLAND_DISPLAY");
            env.insert("QT_QPA_PLATFORM", "nonexistent-platform-plugin");
            env.insert("XDG_CONFIG_HOME", files.filePath("settings"));
            process.setProcessEnvironment(env);
            process.start(QCoreApplication::applicationDirPath() + "/molforma", QStringList{"--mcp"} + extra);
            return process.waitForStarted();
        };
        const auto fake = QCoreApplication::applicationDirPath() + "/fake_nwchem";
        const auto caps = clientCapabilities(false, true);
        QProcess process;
        QVERIFY(launch(process, {"--mcp-allow-execution-settings"}));
        QVERIFY(!failed(processCall(
            process, "import_document",
            {{"format", "xyz"}, {"text", "2\nH2\nH 0 0 0\nH 0 0 .74\n"}, {"preview", false}}, true)));
        QVERIFY(!failed(processCall(
            process, "set_execution",
            {{"settings",
              QJsonObject{{"mode", "direct"}, {"executable", fake}, {"job_root", files.filePath("jobs")}}},
             {"preview", false}},
            true)));
        const auto task = [&](const QString &tool, const QJsonObject &args) {
            process.write(message("tools/call", {{"name", "list_documents"}, {"arguments", QJsonObject{}}}) +
                          '\n');
            const auto doc = data(readReply(process))["documents"].toArray()[0].toObject();
            auto full = args;
            full["documentId"] = doc["documentId"];
            full["expectedRevision"] = doc["revision"];
            process.write(capable("tools/call", {{"name", tool}, {"arguments", full}}, caps) + '\n');
            const auto created = readReply(process)["result"].toObject();
            QJsonObject state;
            QElapsedTimer timer;
            timer.start();
            do {
                QTest::qWait(50);
                process.write(capable("tasks/get", {{"taskId", created["taskId"]}}, caps) + '\n');
                state = readReply(process)["result"].toObject();
            } while (state["status"] != "completed" && timer.elapsed() < 15000);
            return QJsonObject{{"created", created}, {"final", state}};
        };
        auto run = task("calculation", {{"action", "start"}});
        QCOMPARE(run["created"].toObject()["resultType"].toString(), QString("task"));
        const auto calculation = run["final"]
                                     .toObject()["result"]
                                     .toObject()["structuredContent"]
                                     .toObject()["app"]
                                     .toObject()["calculation"]
                                     .toObject();
        QCOMPARE(calculation["state"].toString(), QString("finished"));
        QVERIFY(QFileInfo::exists(calculation["directory"].toString() + "/input.nw"));
        process.write(message("resources/read", {{"uri", "molforma://calculation/output"}}) + '\n');
        QVERIFY(readReply(process)["result"]
                    .toObject()["contents"]
                    .toArray()[0]
                    .toObject()["text"]
                    .toString()
                    .contains("fake NWChem finished"));
        auto loaded = task("results_action", {{"action", "open_file"}, {"path", modes}});
        QCOMPARE(loaded["final"].toObject()["status"].toString(), QString("completed"));
        const auto parsed =
            data(processCall(process, "get_results", {{"section", "modes"}}))["items"].toArray();
        QCOMPARE(parsed.size(), 2);
        QCOMPARE(parsed[0].toObject()["frequency"].toDouble(), -100.0);
        QCOMPARE(data(processCall(process, "get_app_state"))["results"].toObject()["source"].toString(),
                 QFileInfo(modes).absoluteFilePath());
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(10000));
        // By default a headless server runs only nwchem/mpirun from PATH without overrides.
        QProcess strict;
        QVERIFY(launch(strict, {}));
        QVERIFY(
            failed(processCall(strict, "set_execution",
                               {{"settings", QJsonObject{{"executable", fake}}}, {"preview", false}}, true)));
        Document project;
        placeTemplate(project, findTemplate("skeleton:water"), Vec::Zero());
        setExecutionSetting(project, "executable", fake);
        setExecutionSetting(project, "mode", "direct");
        setExecutionSetting(project, "job_root", files.filePath("jobs"));
        QVERIFY(!failed(processCall(strict, "import_document",
                                    {{"format", "project"},
                                     {"text", QString::fromUtf8(QJsonDocument(project.project()).toJson())},
                                     {"preview", false}},
                                    true)));
        const auto refused = processCall(strict, "calculation", {{"action", "start"}}, true);
        QVERIFY(failed(refused));
        QVERIFY(text(refused).contains("--mcp-allow-execution-settings"));
        strict.closeWriteChannel();
        QVERIFY(strict.waitForFinished(10000));
    }
    void subprocess_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("headless without Qt platform") << QString("--mcp");
        QTest::newRow("GUI") << QString("--mcp-gui");
    }
    void subprocess() {
        QFETCH(QString, mode);
        QProcess process;
        startProcess(process, mode);
        QVERIFY(process.waitForStarted());
        const auto request = message("server/discover", {}, "日本語-id");
        process.write(request.left(request.size() / 2));
        QVERIFY(process.waitForBytesWritten());
        QVERIFY(!process.waitForReadyRead(100));
        process.write(request.mid(request.size() / 2) + "\r\n");
        const auto reply = readReply(process);
        QVERIFY2(reply.contains("result"), process.readAllStandardError().constData());
        QCOMPARE(reply["id"].toString(), QString("日本語-id"));
        // Multiple lines, notifications and invalid JSON remain correctly framed.
        process.write(message("notifications/cancelled", {{"requestId", 1}}, QJsonValue::Undefined) + "\n" +
                      "{invalid}\n" + message("ping", {}, 42) + "\n");
        QCOMPARE(readReply(process)["error"].toObject()["code"].toInt(), -32700);
        QCOMPARE(readReply(process)["id"].toInt(), 42);
        process.write(message("tools/call", {{"name", "list_documents"}}, 43) + '\n');
        const auto documents = data(readReply(process))["documents"].toArray();
        QCOMPARE(documents.size(), 1);
        QJsonObject args{{"documentId", documents[0].toObject()["documentId"]},
                         {"expectedRevision", documents[0].toObject()["revision"]},
                         {"element", "He"},
                         {"position", QJsonArray{10, 10, 10}}};
        process.write(message("tools/call", {{"name", "add_atom"}, {"arguments", args}}, 44) + '\n');
        const auto added = readReply(process);
        QVERIFY2(!failed(added), QJsonDocument(added).toJson().constData());
        QVERIFY(data(added)["candidate"].isObject());
        process.write(message("tools/call",
                              {{"name", "history"},
                               {"arguments", QJsonObject{{"documentId", args["documentId"]},
                                                         {"expectedRevision", data(added)["revision"]},
                                                         {"action", "confirm"}}}},
                              45) +
                      '\n');
        const auto confirmed = readReply(process);
        QVERIFY(!failed(confirmed));
        QVERIFY(data(confirmed)["candidate"].isNull());
        QVERIFY(data(confirmed)["atomCount"].toInt() > 0);
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().isEmpty());
    }
    void subprocessLimitsAndEof() {
        QProcess oversized;
        startProcess(oversized, "--mcp");
        QVERIFY(oversized.waitForStarted());
        oversized.write(QByteArray(McpStdioTransport::MaxMessageBytes + 1, 'x'));
        while (oversized.bytesToWrite() && oversized.state() == QProcess::Running)
            oversized.waitForBytesWritten(1000);
        QVERIFY(oversized.waitForFinished(10000));
        QCOMPARE(oversized.exitCode(), 1);
        QVERIFY(oversized.readAllStandardError().contains("8 MiB"));
        QProcess truncated;
        startProcess(truncated, "--mcp");
        QVERIFY(truncated.waitForStarted());
        QByteArray lines;
        for (int i = 0; i < 32; ++i)
            lines += message("ping", {}, i) + '\n';
        truncated.write(lines + "{truncated");
        truncated.closeWriteChannel();
        QVERIFY(truncated.waitForFinished(10000));
        QCOMPARE(truncated.exitCode(), 1);
        const auto output = truncated.readAllStandardOutput();
        QCOMPARE(output.count('\n'), 32);
        QVERIFY(truncated.readAllStandardError().contains("Truncated"));
    }
    void missingDesktopEnvironment() {
#ifdef Q_OS_LINUX
        auto env = QProcessEnvironment::systemEnvironment();
        for (const auto *name :
             {"DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XAUTHORITY", "QT_QPA_PLATFORM"})
            env.remove(name);
        QProcess gui;
        gui.setProcessEnvironment(env);
        gui.start(QCoreApplication::applicationDirPath() + "/molforma", {"--mcp-gui"});
        QVERIFY(gui.waitForStarted());
        QVERIFY(gui.waitForFinished(10000));
        QCOMPARE(gui.exitStatus(), QProcess::NormalExit);
        QCOMPARE(gui.exitCode(), 1);
        QVERIFY(gui.readAllStandardOutput().isEmpty());
        const auto diagnostic = gui.readAllStandardError();
        QVERIFY(diagnostic.contains("requires a desktop display"));
        QVERIFY(diagnostic.contains("env_vars"));
        QVERIFY(diagnostic.contains("--mcp for headless"));

        QProcess headless;
        headless.setProcessEnvironment(env);
        headless.start(QCoreApplication::applicationDirPath() + "/molforma", {"--mcp"});
        QVERIFY(headless.waitForStarted());
        headless.write(message("server/discover") + '\n');
        QVERIFY(readReply(headless).contains("result"));
        headless.closeWriteChannel();
        QVERIFY(headless.waitForFinished(10000));
        QCOMPARE(headless.exitCode(), 0);
#endif
    }
};
QTEST_MAIN(McpTest)
#include "test_mcp.moc"
