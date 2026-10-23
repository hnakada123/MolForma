#include "editing/library.h"
#include "mcp/server.h"
#include "mcp/stdio_transport.h"
#include "ui/main_window.h"
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
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
};
QTEST_MAIN(McpTest)
#include "test_mcp.moc"
