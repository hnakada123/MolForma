#include "editing/library.h"
#include "mcp/server.h"
#include "mcp/stdio_transport.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include "ui/main_window.h"
#include "ui/mcp_gui.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
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
QJsonObject call(McpServer &server, const QString &name, QJsonObject args = {}) {
    return server.receive(message("tools/call", {{"name", name}, {"arguments", args}}));
}
QJsonObject data(const QJsonObject &response) {
    return response["result"].toObject()["structuredContent"].toObject();
}
bool failed(const QJsonObject &response) {
    return response.contains("error") || response["result"].toObject()["isError"].toBool();
}
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
        // Each request carries its own metadata; no initialization or cached version is used.
        auto unsupported = server.receive(message("ping", {{"_meta", metadata("1900-01-01")}}, 2, false));
        QCOMPARE(unsupported["error"].toObject()["code"].toInt(), -32022);
        QCOMPARE(unsupported["error"].toObject()["data"].toObject()["requested"].toString(),
                 QString("1900-01-01"));
        QVERIFY(!server.receive(message("ping")).contains("error"));
        auto missing = metadata();
        missing.remove("io.modelcontextprotocol/clientCapabilities");
        QCOMPARE(server.receive(message("ping", {{"_meta", missing}}, 3, false))["error"]
                     .toObject()["code"]
                     .toInt(),
                 -32602);
        QCOMPARE(server.receive(message("tools/list", {}, 4, false))["error"].toObject()["code"].toInt(),
                 -32602);
        QCOMPARE(server.receive(message("unknown"))["error"].toObject()["code"].toInt(), -32601);
        QCOMPARE(server.receive(message("prompts/list"))["error"].toObject()["code"].toInt(), -32601);
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
                table.setItem(row, col, new QTableWidgetItem(QString(4096, 'x')));
        const auto first =
            data(call(s, "get_ui_item", scoped(s, {{"target", "largeTable"}, {"limit", 200}})));
        const int next = first["nextOffset"].toInt();
        QVERIFY(next > 0 && next < 100);
        const auto second = data(
            call(s, "get_ui_item", scoped(s, {{"target", "largeTable"}, {"offset", next}, {"limit", 200}})));
        QCOMPARE(second["nextOffset"].toInt(), 100);
        QCOMPARE(first["items"].toArray().size() + second["items"].toArray().size(), 100);
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
        writeFile(
            modes,
            "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\nH 2 1 0 0 1.4\n"
            "[GTO]\n1 0\ns 1 1.0\n1.0 1.0\n"
            "[MO]\nSym= A1\nEne= -0.5\nSpin= Alpha\nOccup= 2.0\n1 1.0\n"
            "[FREQ]\n-100\n1500\n[FR-COORD]\nH 0 0 0\nH 0 0 1.4\n"
            "[FR-NORM-COORD]\nvibration 1\n0 0 .5\n0 0 -.5\nvibration 2\n0 0 .1\n0 0 -.1\n[INT]\n10\n20\n");
        QProcess process;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "offscreen");
        env.insert("MOLFORMA_RENDERER", "painter");
        env.insert("XDG_CONFIG_HOME", files.filePath("settings"));
        process.setProcessEnvironment(env);
        process.start(QCoreApplication::applicationDirPath() + "/molforma", {"--mcp-gui"});
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
        QVERIFY(server.receive(message("tools/list", {}, 2, false)).contains("error"));
        QVERIFY(
            server.receive(message("notifications/initialized", {}, QJsonValue::Undefined, false)).isEmpty());
        QVERIFY(!server.receive(message("tools/list", {}, 3, false)).contains("error"));
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
        QCOMPARE(Document::fromProject(data(placed)["candidate"].toObject()).formula(), QString("H2O"));
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
        QVERIFY(!failed(change(server, "delete_atoms", {{"ids", QJsonArray{id}}})));
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
