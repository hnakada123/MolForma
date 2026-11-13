// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/coordination.h"
#include "editing/library.h"
#include "editing/peptide.h"
#include "foundation/files.h"
#include "io/document_storage.h"
#include "io/structure_io.h"
#include "mcp/execution_trust.h"
#include "mcp/headless.h"
#include "mcp/server.h"
#include "mcp/tools.h"
#include "mcp_support.h"
#include "model/geometry.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include "results/results.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <cmath>
using namespace mf;

// The MCP server without a window or another process: the protocol, the tools on its own document, the
// access policy of execution settings, and calculations and results of the headless runtime.
class McpServerTest : public QObject {
    Q_OBJECT
    QTemporaryDir settingsHome;
  private slots:
    void initTestCase() {
        QVERIFY(settingsHome.isValid());
        qputenv("XDG_CONFIG_HOME", settingsHome.path().toUtf8());
        qputenv("XDG_DATA_HOME", settingsHome.path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsHome.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settingsHome.path()); // MolForma uses NativeFormat.
    }
    void atomPropertiesRejectUnknownIsotope() {
        McpServer server;
        const auto carbon = Document::fromXyz("1\ncarbon\nC 0 0 0\n");
        server.resetDocument(carbon);
        const auto before = server.snapshot();
        for (bool preview : {false, true}) {
            QVERIFY(
                failed(change(server, "set_atom_properties",
                              {{"ids", idsToJson(carbon.ids())}, {"isotope", 400}, {"preview", preview}})));
            QCOMPARE(server.snapshot(), before);
        }
        QVERIFY(!failed(
            change(server, "set_atom_properties", {{"ids", idsToJson(carbon.ids())}, {"isotope", 13}})));
        QCOMPARE(server.candidate()->atoms[0].isotope, 13);
        QCOMPARE(server.document().project(), carbon.project());
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
    void settingsBeforeMolecule() {
        // Errors already there (no atoms yet) do not block settings; errors a change adds do.
        McpServer s;
        QVERIFY(!failed(
            change(s, "set_execution", {{"settings", QJsonObject{{"mpi_ranks", 3}}}, {"preview", false}})));
        QVERIFY(!failed(
            change(s, "set_nwchem",
                   {{"settings", QJsonObject{{"method", "dft"}, {"basis", "6-31g*"}}}, {"preview", false}})));
        QVERIFY(!failed(change(s, "set_advanced",
                               {{"settings", QJsonObject{{"freq_animate", true}}}, {"preview", false}})));
        QCOMPARE(executionSettings(committed(s))["mpi_ranks"].toInt(), 3);
        QCOMPARE(nwchemSettings(committed(s))["basis"].toString(), QString("6-31g*"));
        const auto before = s.snapshot();
        QVERIFY(failed(change(
            s, "set_nwchem",
            {{"settings", QJsonObject{{"method", "ccsd"}, {"task", "optimize"}}}, {"preview", false}})));
        QVERIFY(failed(
            change(s, "set_execution", {{"settings", QJsonObject{{"mpi_ranks", 0}}}, {"preview", false}})));
        QCOMPARE(s.snapshot(), before);
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
    void solvationShell() {
        McpServer server;
        QVERIFY(!failed(change(
            server, "place_template",
            {{"templateId", "skeleton:benzene"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        const auto carbon = committed(server).atoms.first().id;
        // A candidate by default, around the chosen atom; only the listed solvents.
        const auto shell = change(
            server, "solvate", {{"solvent", "skeleton:methanol"}, {"count", 3}, {"ids", QJsonArray{carbon}}});
        QVERIFY2(!failed(shell), qPrintable(QString::fromUtf8(QJsonDocument(shell).toJson())));
        QCOMPARE(data(shell)["candidate"].toObject()["formula"].toString(), QString("C9H18O3"));
        QVERIFY(!failed(change(server, "history", {{"action", "confirm"}})));
        QCOMPARE(committed(server).parts.size(), 4);
        QVERIFY(!failed(change(server, "solvate", {{"solvent", "skeleton:water"}, {"preview", false}})));
        QCOMPARE(committed(server).atoms.size(), 12 + 3 * 6 + 6 * 3); // Benzene, 3 methanols, 6 waters.
        QVERIFY(failed(change(server, "solvate", {{"solvent", "skeleton:benzene"}})));
        QVERIFY(failed(change(server, "solvate", {{"solvent", "skeleton:water"}, {"count", 0}})));
        QVERIFY(failed(
            change(server, "solvate", {{"solvent", "skeleton:water"}, {"ids", QJsonArray{"missing"}}})));
    }
    void dimensionsAndAtomProperties() {
        McpServer server;
        QVERIFY(!failed(change(
            server, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        auto d = committed(server);
        const auto o = d.atoms[0].id, h1 = d.atoms[1].id, h2 = d.atoms[2].id;
        QVERIFY(!failed(change(server, "set_dimension",
                               {{"ids", QJsonArray{o, h1}}, {"value", 1.1}, {"preview", false}})));
        QVERIFY(std::abs(measure(committed(server), {o, h1}) - 1.1) < 1e-9);
        QVERIFY(!failed(change(server, "set_dimension",
                               {{"ids", QJsonArray{h1, o, h2}}, {"value", 120}, {"preview", false}})));
        QVERIFY(std::abs(measure(committed(server), {h1, o, h2}) - 120) < 1e-9);
        QVERIFY(failed(change(server, "set_dimension",
                              {{"ids", QJsonArray{o, h1}}, {"value", -1}, {"preview", false}})));
        // Properties: isotope and fixed flags, charge, ghost and name; fixed atoms block later moves.
        QVERIFY(!failed(change(server, "set_atom_properties",
                               {{"ids", QJsonArray{h1}},
                                {"isotope", 2},
                                {"fixed", true},
                                {"name", "HD1"},
                                {"preview", false}})));
        d = committed(server);
        QCOMPARE(d.atom(h1).isotope, 2);
        QVERIFY(d.atom(h1).fixed);
        QCOMPARE(d.atom(h1).name, QString("HD1"));
        QVERIFY(failed(change(server, "set_dimension",
                              {{"ids", QJsonArray{o, h1}}, {"value", 1.0}, {"preview", false}})));
        QVERIFY(failed(change(server, "set_atom_properties",
                              {{"ids", QJsonArray{h2}}, {"isotope", 2}, {"mass", 2.0}, {"preview", false}})));
        QVERIFY(
            !failed(change(server, "set_atom_properties",
                           {{"ids", QJsonArray{o}}, {"element", "S"}, {"charge", 0}, {"preview", false}})));
        QCOMPARE(committed(server).atom(o).element, QString("S"));
        QVERIFY(!failed(change(server, "set_atom_properties",
                               {{"ids", QJsonArray{h2}}, {"ghost", true}, {"preview", false}})));
        QVERIFY(committed(server).atom(h2).ghost);
    }
    void deletionFollowsCharge() {
        // Deleting a center takes its oxidation number out of the total charge, as in the GUI.
        McpServer server;
        Document d;
        const auto sodium = addCenter(d, "Na", "tetrahedral", Vec::Zero(), 2.4, 1);
        placeTemplate(d, findTemplate("skeleton:water"), Vec(5, 0, 0));
        QCOMPARE(d.charge, 1);
        QVERIFY(!failed(change(server, "import_document",
                               {{"format", "project"},
                                {"text", QString::fromUtf8(QJsonDocument(d.project()).toJson())},
                                {"preview", false}})));
        QVERIFY(!failed(change(server, "delete_atoms", {{"ids", QJsonArray{sodium}}, {"preview", false}})));
        QCOMPARE(committed(server).formula(), QString("H2O"));
        QCOMPARE(committed(server).charge, 0);
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
    void resultSections() {
        // get_results pages TDDFT states and optimization steps and reports log summaries.
        ResultData r;
        r.format = "NWChem";
        r.excitations = {{1, "singlet", "a", 8.5, 0.02}, {2, "", "a", 2.0, 0.001, 0.75}};
        r.steps = {{0, -76.0, 0, .067, .024}, {1, -76.01, -.01, .008, .003}};
        r.properties["dipole"] = QJsonObject{{"debye", 1.7}};
        const auto excited = resultsJson(r, {{"section", "excitations"}}, {});
        QCOMPARE(excited["items"].toArray().size(), 2);
        QCOMPARE(excited["items"].toArray()[1].toObject()["s2"].toDouble(), 0.75);
        QVERIFY(std::abs(excited["items"].toArray()[0].toObject()["nm"].toDouble() - 1239.84198 / 8.5) <
                1e-9);
        QCOMPARE(
            resultsJson(r, {{"section", "steps"}}, {})["items"].toArray()[1].toObject()["gmax"].toDouble(),
            .008);
        const auto summary = resultsJson(r, {}, {});
        QCOMPARE(summary["counts"].toObject()["excitations"].toInt(), 2);
        QCOMPARE(summary["properties"].toObject()["dipole"].toObject()["debye"].toDouble(), 1.7);
        QVERIFY(excited["items"].toArray()[0].toObject()["contributions"].toArray().isEmpty());
        QVERIFY(!summary["properties"].toObject().contains("bond_indices"));
        QCOMPARE(summary["counts"].toObject()["bondIndices"].toInt(), 0);
        // The orbital pairs of a state, the heaviest first, with X² − Y² as their weights and the
        // frontier orbitals of five occupied ones.
        r.excitations[0].contributions = {
            {3, 7, "", 0.6, false}, {4, 6, "", -0.8, false}, {4, 6, "", 0.1, true}};
        r.properties["tddft"] = QJsonObject{{"alpha_electrons", 5}, {"beta_electrons", 5}};
        const auto pairs = resultsJson(r, {{"section", "excitations"}}, {})["items"]
                               .toArray()[0]
                               .toObject()["contributions"]
                               .toArray();
        QCOMPARE(pairs.size(), 2);
        QCOMPARE(pairs[0].toObject()["from"].toInt(), 4);
        QCOMPARE(pairs[0].toObject()["to"].toInt(), 6);
        QCOMPARE(pairs[0].toObject()["x"].toDouble(), -0.8);
        QCOMPARE(pairs[0].toObject()["y"].toDouble(), 0.1);
        QVERIFY(std::abs(pairs[0].toObject()["weight"].toDouble() - 100 * .63 / .99) < 1e-9);
        QCOMPARE(pairs[0].toObject()["transition"].toString(), QString("HOMO−1 → LUMO"));
        QCOMPARE(pairs[1].toObject()["transition"].toString(), QString("HOMO−2 → LUMO+1"));
        QVERIFY(!pairs[1].toObject().contains("y") && pairs[1].toObject()["spin"].toString().isEmpty());
        // Bond indices: the bonds in the properties of every view, all pairs with their distances
        // in a section of their own.
        ResultFrame frame;
        frame.molecule = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        r.frames = {frame};
        r.properties["valency"] = QJsonArray{1.66, 0.83, 0.83};
        r.properties["bond_indices"] = QJsonArray{QJsonObject{{"a", 1}, {"b", 2}, {"value", 0.83}},
                                                  QJsonObject{{"a", 1}, {"b", 3}, {"value", 0.83}},
                                                  QJsonObject{{"a", 2}, {"b", 3}, {"value", -0.0003}}};
        const auto bonded = resultsJson(r, {{"section", "steps"}}, {});
        QCOMPARE(bonded["properties"].toObject()["bond_indices"].toArray().size(), 2);
        QCOMPARE(bonded["properties"].toObject()["valency"].toArray().size(), 3);
        QCOMPARE(bonded["counts"].toObject()["bondIndices"].toInt(), 3);
        const auto all = resultsJson(r, {{"section", "bond_indices"}, {"offset", 1}}, {});
        QCOMPARE(all["total"].toInt(), 3);
        QCOMPARE(all["items"].toArray().size(), 2);
        const auto last = all["items"].toArray()[1].toObject();
        QCOMPARE(last["a"].toInt(), 2);
        QCOMPARE(last["b"].toInt(), 3);
        QCOMPARE(last["value"].toDouble(), -0.0003);
        QCOMPARE(last["elements"].toArray(), (QJsonArray{"H", "H"}));
        QVERIFY(std::abs(last["distance"].toDouble() - 1.516) < 1e-12);
    }
    void spectrumSections() {
        ResultData r;
        r.properties["shielding"] =
            QJsonArray{QJsonObject{{"atom", 3}, {"element", "H"}, {"isotropic", 28.}}};
        const QJsonObject p{{"kind", "nmr"}, {"nucleus", "1H"}, {"reference_ppm", 31.}, {"width", .1}};
        const auto peaks = resultsJson(r, {{"section", "spectrum_peaks"}, {"parameters", p}}, {});
        QCOMPARE(peaks["items"].toArray().size(), 1);
        QCOMPARE(peaks["items"].toArray()[0].toObject()["atom"].toInt(), 3);
        QCOMPARE(peaks["items"].toArray()[0].toObject()["x"].toDouble(), 3.);
        QVERIFY(peaks["reversed"].toBool());
        const auto curve =
            resultsJson(r, {{"section", "spectrum"}, {"parameters", p}, {"offset", 7}, {"limit", 11}}, {});
        QCOMPARE(curve["items"].toArray().size(), 11);
        QCOMPARE(curve["nextOffset"].toInt(), 18);
        QCOMPARE(curve["xUnit"].toString(), "ppm");
    }
    void chartKindsAndCorrectedProfile() {
        // The IR and absorption charts of the result viewer are spectrum kinds too.
        ResultData r;
        r.modes.append({1600., 50., {}});
        r.excitations.append({1, "singlet", "a", 5., .1});
        auto peaks = resultsJson(
            r, {{"section", "spectrum_peaks"}, {"parameters", QJsonObject{{"kind", "ir"}, {"width", 20.}}}},
            {});
        QCOMPARE(peaks["xUnit"].toString(), "cm⁻¹");
        QCOMPARE(peaks["items"].toArray()[0].toObject()["x"].toDouble(), 1600.);
        QCOMPARE(peaks["items"].toArray()[0].toObject()["mode"].toInt(), 0);
        peaks = resultsJson(
            r, {{"section", "spectrum_peaks"}, {"parameters", QJsonObject{{"kind", "uv"}, {"width", .3}}}},
            {});
        QCOMPARE(peaks["xUnit"].toString(), "eV");
        QCOMPARE(peaks["items"].toArray()[0].toObject()["weight"].toDouble(), .1);
        // A comparison with corrections of the thermochemistry. One vibration of 417.0209 cm⁻¹ has
        // hcν/kT = 2 at 300 K and a zero-point energy of RT = 0.596161 kcal/mol; a scale factor of
        // 0.5 takes half of it from the printed 2 kcal/mol.
        QJsonObject profile{
            {"format", "molforma-energy-profile"},
            {"version", 1},
            {"metric", "ZPE"},
            {"unit", "kcal"},
            {"temperature", 300.},
            {"standard_state", "1 atm"},
            {"reference", 0},
            {"frequency_scale", .5},
            {"records",
             QJsonArray{QJsonObject{{"id", "a"},
                                    {"name", "a"},
                                    {"normal", true},
                                    {"energy_hartree", -1.},
                                    {"composition", QJsonObject{{"H", 2}}},
                                    {"charge", 0},
                                    {"vibrations", QJsonArray{2 * 208.51044014583823}},
                                    {"thermochemistry", QJsonArray{QJsonObject{{"temperature", 300.},
                                                                               {"zpe_kcal", 2.},
                                                                               {"enthalpy_kcal", 3.},
                                                                               {"entropy_cal", 5.}}}}}}},
            {"stages",
             QJsonArray{QJsonObject{
                 {"name", "a"}, {"terms", QJsonArray{QJsonObject{{"record", "a"}, {"coefficient", 1}}}}}}}};
        auto evaluated = resultsJson({}, {{"section", "energy_profile"}, {"profile", profile}}, {});
        QCOMPARE(evaluated["frequency_scale"].toDouble(), .5);
        QCOMPARE(evaluated["entropy_model"].toString(), "rrho");
        QVERIFY(std::abs(evaluated["stages"][0].toObject()["sources"][0].toObject()["zpe_kcal"].toDouble() -
                         (2 - .29808063879612473)) < 1e-12);
        // G at 1 mol/L: the shift at 300 K is 0.596161 × ln(0.0820573661 × 300) = 1.9098 kcal/mol,
        // and the stages carry exp(−ΔG/RT) against the reference.
        profile["metric"] = "G";
        profile["frequency_scale"] = 1.;
        profile["standard_state_correction"] = "1M";
        evaluated = resultsJson({}, {{"section", "energy_profile"}, {"profile", profile}}, {});
        QVERIFY(std::abs(evaluated["standard_state_shift_kcal"].toDouble() - 1.9098) < 1e-4);
        QCOMPARE(evaluated["stages"][0].toObject()["boltzmann_factor"].toDouble(), 1.);
        QVERIFY(std::abs(
                    evaluated["stages"][0].toObject()["sources"][0].toObject()["correction_kcal"].toDouble() -
                    (3 - 300 * 5 / 1000. + 1.9098)) < 1e-4);
    }
    void appendXyz() {
        McpServer server;
        const QString xyz = "3\nwater\nO 0 0 0\nH .758 0 .504\nH -.758 0 .504\n";
        QVERIFY(!failed(
            change(server, "import_document", {{"format", "xyz"}, {"text", xyz}, {"preview", false}})));
        QVERIFY(!failed(change(server, "import_document",
                               {{"format", "xyz"}, {"text", xyz}, {"append", true}, {"preview", false}})));
        QCOMPARE(committed(server).atoms.size(), 6);
        QVERIFY(
            failed(change(server, "import_document",
                          {{"format", "project"}, {"text", "{}"}, {"append", true}, {"preview", false}})));
    }
    void openStructureKeepsSettings() {
        // Like the coordinate table, opening a structure file replaces only the structure.
        QTemporaryDir dir;
        McpServer server;
        QVERIFY(!failed(change(
            server, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        QVERIFY(!failed(change(server, "set_nwchem",
                               {{"settings", QJsonObject{{"basis", "6-31g*"}}}, {"preview", false}})));
        QVERIFY(!failed(change(server, "set_execution",
                               {{"settings", QJsonObject{{"mpi_ranks", 5}}}, {"preview", false}})));
        const auto xyz = dir.filePath("h2.xyz");
        writeFile(xyz, "2\nH2\nH 0 0 0\nH 0 0 .74\n");
        QVERIFY(!failed(change(server, "open_file", {{"path", xyz}, {"preview", false}})));
        QCOMPARE(committed(server).formula(), QString("H2"));
        QCOMPARE(nwchemSettings(committed(server))["basis"].toString(), QString("6-31g*"));
        QCOMPARE(executionSettings(committed(server))["mpi_ranks"].toInt(), 5);
        // Multi-frame XYZ files need a frame; NWChem inputs bring charge, multiplicity and settings
        // but keep the additional input chosen in MolForma.
        const auto frames = dir.filePath("frames.xyz");
        writeFile(frames, "1\na\nHe 0 0 0\n1\nb\nNe 0 0 0\n");
        QVERIFY(failed(change(server, "open_file", {{"path", frames}, {"preview", false}})));
        QVERIFY(!failed(change(server, "open_file", {{"path", frames}, {"frame", 1}, {"preview", false}})));
        QCOMPARE(committed(server).formula(), QString("Ne"));
        const auto nw = dir.filePath("input.nw");
        writeFile(nw, "charge 1\ngeometry\n O 0 0 0\n H 0 0 0.97\nend\nbasis\n * library cc-pvdz\nend\n"
                      "dft\n xc b3lyp\n mult 1\nend\ntask dft optimize\n");
        QVERIFY(!failed(change(server, "open_file", {{"path", nw}, {"preview", false}})));
        const auto opened = committed(server);
        QCOMPARE(opened.formula(), QString("HO"));
        QCOMPARE(opened.charge, 1);
        QCOMPARE(nwchemSettings(opened)["basis"].toString(), QString("cc-pvdz"));
        QCOMPARE(nwchemSettings(opened)["task"].toString(), QString("optimize"));
        QCOMPARE(executionSettings(opened)["mpi_ranks"].toInt(), 5);
        // PDB and mmCIF files read the chains and kinds of residues asked for.
        const auto pdb = dir.filePath("entry.pdb");
        writeFile(pdb, "ATOM      1  CA  GLY A   1       0.000   0.000   0.000  1.00  0.00           C\n"
                       "ATOM      2  CA  GLY B   1       0.000   5.000   0.000  1.00  0.00           C\n"
                       "HETATM    3 ZN    ZN A 101       5.000   5.000   5.000  1.00  0.00          ZN\n"
                       "HETATM    4  O   HOH A 201       9.000   9.000   9.000  1.00  0.00           O\n");
        QVERIFY(!failed(change(server, "open_file", {{"path", pdb}, {"preview", false}})));
        QCOMPARE(committed(server).formula(), QString("C2OZn"));
        QVERIFY(!failed(
            change(server, "open_file",
                   {{"path", pdb}, {"chains", QJsonArray{"A"}}, {"water", false}, {"preview", false}})));
        QCOMPARE(committed(server).formula(), QString("CZn"));
        QVERIFY(failed(
            change(server, "open_file", {{"path", pdb}, {"chains", QJsonArray{"X"}}, {"preview", false}})));
        QCOMPARE(committed(server).formula(), QString("CZn"));
        // A small-molecule CIF: water in P2(1)/c, one molecule or the four of the unit cell.
        const auto cif = dir.filePath("water.cif");
        writeFile(cif,
                  "data_water\n_cell_length_a 7\n_cell_length_b 8\n_cell_length_c 9\n_cell_angle_alpha 90\n"
                  "_cell_angle_beta 100.5\n_cell_angle_gamma 90\nloop_\n_space_group_symop_operation_xyz\n"
                  "'x,y,z'\n'-x,y+1/2,-z+1/2'\n'-x,-y,-z'\n'x,-y+1/2,z+1/2'\nloop_\n_atom_site_label\n"
                  "_atom_site_type_symbol\n_atom_site_fract_x\n_atom_site_fract_y\n_atom_site_fract_z\n"
                  "O1 O 0.99 0.30 0.20\nH1 H 0.12 0.30 0.20\nH2 H 0.94 0.412 0.20\n");
        QVERIFY(!failed(change(server, "open_file", {{"path", cif}, {"preview", false}})));
        QCOMPARE(committed(server).formula(), QString("H2O"));
        QVERIFY(!failed(change(server, "open_file", {{"path", cif}, {"cell", true}, {"preview", false}})));
        QCOMPARE(committed(server).formula(), QString("H8O4"));
    }
    void cutOutSite() {
        QTemporaryDir dir;
        Document peptide;
        buildPeptide(peptide, {"GLY", "LYS", "GLY", "ASP", "GLY"}, "charged", "charged");
        const auto pdb = dir.filePath("peptide.pdb");
        exportStructure(peptide, pdb);
        McpServer server;
        QVERIFY(!failed(change(server, "set_nwchem",
                               {{"settings", QJsonObject{{"basis", "6-31g*"}}}, {"preview", false}})));
        auto response = change(server, "cut_out_site",
                               {{"path", pdb},
                                {"residues", QJsonArray{QJsonObject{{"number", 3}}}},
                                {"radius", 0.1},
                                {"pointCharges", true},
                                {"chargeRadius", 100},
                                {"preview", false}});
        QVERIFY2(!failed(response), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson())));
        auto site = committed(server);
        QCOMPARE(site.residues.size(), 1);
        QCOMPARE(site.residues[0].code, QString("GLY"));
        QCOMPARE(std::count_if(site.atoms.begin(), site.atoms.end(), [](const Atom &a) { return a.fixed; }),
                 2);
        QCOMPARE(site.pointCharges.size(), 6); // NH3+, Lys, Asp and COO− of the residues removed.
        QCOMPARE(site.charge, 0);
        QVERIFY(nwchemSettings(site)["include_point_charges"].toBool());
        QCOMPARE(nwchemSettings(site)["basis"].toString(), QString("6-31g*")); // Settings stay.
        // The fixed atoms at the cuts stay: a smaller site that would remove them is refused.
        const auto alpha = site.residues[0].atoms["CA"];
        QVERIFY(failed(change(
            server, "cut_out_site",
            {{"ids", QJsonArray{alpha}}, {"radius", 0}, {"wholeResidues", false}, {"preview", false}})));
        QCOMPARE(committed(server).atoms.size(), site.atoms.size());
        // The document itself, by atom IDs; the IDs of a file's atoms do not exist yet.
        McpServer opened;
        QVERIFY(!failed(change(opened, "open_file", {{"path", pdb}, {"preview", false}})));
        const auto whole = committed(opened);
        QVERIFY(!failed(change(
            opened, "cut_out_site",
            {{"ids", QJsonArray{whole.residues[2].atoms["CA"]}}, {"radius", 0.1}, {"preview", false}})));
        QCOMPARE(committed(opened).residues.size(), 1);
        QVERIFY(committed(opened).atoms.size() < whole.atoms.size());
        QVERIFY(failed(
            change(server, "cut_out_site", {{"path", pdb}, {"ids", QJsonArray{alpha}}, {"preview", false}})));
        QVERIFY(failed(change(
            server, "cut_out_site",
            {{"path", pdb}, {"residues", QJsonArray{QJsonObject{{"code", "TRP"}}}}, {"preview", false}})));
        QVERIFY(failed(change(server, "cut_out_site", {{"preview", false}})));
    }
    void invalidPropertyNumbersDoNotChangeTheDocument() {
        McpServer server;
        QVERIFY(!failed(change(
            server, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        QVERIFY(!failed(change(
            server, "set_nwchem",
            {{"settings", QJsonObject{{"method", "dft"}, {"task", "frequencies"}, {"prop_raman", true}}},
             {"preview", false}})));
        const auto before = server.snapshot();
        for (const bool preview : {false, true})
            for (const auto *key :
                 {"raman_wavelength_nm", "property_grid_padding", "ord_start_nm", "ord_end_nm"}) {
                const auto response =
                    change(server, "set_nwchem", {{"settings", QJsonObject{{key, 0}}}, {"preview", preview}});
                QVERIFY2(failed(response), qPrintable(QString::fromUtf8(QJsonDocument(response).toJson())));
                QCOMPARE(server.snapshot(), before); // Includes revision and candidate state.
            }
        const auto input = call(server, "export_document", scoped(server, {{"format", "nwchem"}}));
        QVERIFY(!failed(input));
        QVERIFY(data(input)["text"].toString().contains("response 1 "));
        QVERIFY(!data(input)["text"].toString().contains(" inf"));
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
        // The geometry_optimization prompt relies on get_nwchem listing the allowed values.
        const auto choices = data(call(server, "get_nwchem", scoped(server)))["choices"].toObject();
        QVERIFY(
            choices["basis"].toArray().contains(QJsonObject{{"value", "def2-svp"}, {"label", "def2-SVP"}}));
        QCOMPARE(choices["basis_functions"].toArray().size(), 3);
        QVERIFY(!choices.contains("memory_mb"));
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
        auto path = directory.filePath("molecule.molforma.json");
        QVERIFY(!failed(change(server, "save_file", {{"path", path}})));
        QCOMPARE(loadProject(path).toJson(), committed(server).toJson());
        QVERIFY(failed(change(server, "save_file", {{"path", path}})));
        QVERIFY(!failed(change(server, "save_file", {{"path", path}, {"overwrite", true}})));
        QVERIFY(!failed(change(server, "open_file", {{"path", path}})));
        auto templates = data(call(server, "list_templates"))["templates"].toArray();
        QVERIFY(!templates.isEmpty());
        QVERIFY(!templates[0].toObject()["name"].toString().isEmpty());
    }
    void saveFileElicitsOverwrite() {
        McpServer server;
        QVERIFY(!failed(change(
            server, "place_template",
            {{"templateId", "skeleton:water"}, {"position", QJsonArray{0, 0, 0}}, {"preview", false}})));
        QTemporaryDir directory;
        const auto path = directory.filePath("existing.molforma.json");
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
        QCOMPARE(loadProject(path).toJson(), committed(server).toJson());
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
        // So is the computer that runs it: a remote destination is the user's choice.
        const auto remote = change(server, "set_execution",
                                   {{"settings", QJsonObject{{"remote", "cluster"}}}, {"preview", false}});
        QVERIFY(failed(remote));
        QVERIFY(text(remote).contains("cannot be changed over MCP"));
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
        // Unchecked NWChem input could start programs through NWChem: protected the same way.
        const auto extra =
            change(server, "set_nwchem",
                   {{"settings", QJsonObject{{"extra_tasks", "task scf property"}}}, {"preview", false}});
        QVERIFY(failed(extra));
        QVERIFY(text(extra).contains("only the user can change it"));
        QVERIFY(!failed(change(
            server, "set_nwchem",
            {{"settings", QJsonObject{{"extra_tasks", ""}, {"task", "optimize"}}}, {"preview", false}})));
        QVERIFY(!failed(
            change(allowed, "set_nwchem",
                   {{"settings", QJsonObject{{"extra_tasks", "task scf property"}}}, {"preview", false}})));
        auto edited = committed(server);
        auto s = edited.data["nwchem"].toObject();
        s["extra_input"] = "set cphf:maxiter 50";
        edited.data["nwchem"] = s;
        QVERIFY(protectedExecution(edited) != protectedExecution(committed(server)));
    }
    void executionTrustCountsWhatTheUserEnteredOrConfirmed() {
        // The rule behind MainWindow::untrustedExecution and the headless server's check, without a window.
        using Origin = ExecutionTrust::Origin;
        const auto with = [](Document d, const QString &key, const QJsonValue &value) {
            setExecutionSetting(d, key, value);
            return d;
        };
        const Document plain;
        const auto preferences = protectedExecution(plain);
        ExecutionTrust trust;
        QVERIFY(trust.untrusted(plain, preferences).isEmpty());
        // What the user enters in MolForma is trusted: the value the edit changed, not the others kept
        // beside it (a launcher that came with the file).
        const auto opened = with(plain, "launcher", "/opt/mpi/bin/srun");
        const auto entered = with(opened, "executable", "/opt/nwchem/bin/nwchem");
        QCOMPARE(trust.untrusted(entered, preferences), (QStringList{"executable", "launcher"}));
        trust.documentChanged(opened, entered, Origin::User);
        QCOMPARE(trust.untrusted(entered, preferences), QStringList{"launcher"});
        // It stays trusted when it comes back later, whoever brings it.
        trust.documentChanged(entered, plain, Origin::History);
        trust.documentChanged(plain, entered, Origin::Load);
        QCOMPARE(trust.untrusted(entered, preferences), QStringList{"launcher"});
        // The values of a load, of undo/redo and of MCP are not trusted, whatever they replace.
        auto loaded = with(plain, "executable", "/tmp/engine");
        auto calculation = loaded.data["nwchem"].toObject();
        calculation["extra_input"] = "set cphf:maxiter 50";
        loaded.data["nwchem"] = calculation;
        loaded =
            with(loaded, "environment",
                 QJsonArray{QJsonObject{{"enabled", true}, {"name", "LD_PRELOAD"}, {"value", "/tmp/x.so"}}});
        const QStringList carried{"environment", "executable", "extra_input"};
        for (const auto origin : {Origin::Load, Origin::History, Origin::Mcp}) {
            trust.documentChanged(plain, loaded, origin);
            QCOMPARE(trust.untrusted(loaded, preferences), carried);
        }
        // … until the user confirms them before a run: then all of them are, also for MCP's next run.
        trust.confirm(loaded);
        QVERIFY(trust.untrusted(loaded, preferences).isEmpty());
        trust.documentChanged(plain, loaded, Origin::Mcp);
        QVERIFY(trust.untrusted(loaded, preferences).isEmpty());
        // Another value of a confirmed setting asks again; the confirmed one is kept beside it.
        const auto other = with(loaded, "executable", "/tmp/other engine");
        QCOMPARE(trust.untrusted(other, preferences), QStringList{"executable"});
        trust.confirm(other);
        QVERIFY(trust.untrusted(other, preferences).isEmpty());
        QVERIFY(trust.untrusted(loaded, preferences).isEmpty());
        // What Preferences hold always counts, with nothing entered or confirmed: the saved values of
        // the moment, not those of an earlier one.
        const ExecutionTrust empty{};
        const auto saved = protectedExecution(opened);
        QVERIFY(empty.untrusted(opened, saved).isEmpty());
        QCOMPARE(empty.untrusted(opened, preferences), QStringList{"launcher"});
        QCOMPARE(empty.untrusted(plain, saved), QStringList{"launcher"});
        // That empty trust is the headless server's rule: a run only with exactly the trusted values.
        for (const auto &document : {plain, opened, entered, loaded, other})
            for (const auto &baseline : {preferences, saved, protectedExecution(loaded)})
                QCOMPARE(empty.untrusted(document, baseline).isEmpty(),
                         protectedExecution(document) == baseline);
    }
    void headlessCalculationQueue() {
        QTemporaryDir files;
        McpPolicy policy;
        policy.allowExecutionSettings = true;
        McpHeadless runtime(policy);
        McpServer server(runtime.editor(), policy);
        runtime.attach(server);
        QVERIFY(!failed(
            change(server, "import_document",
                   {{"format", "xyz"}, {"text", "2\nH2\nH 0 0 0\nH 0 0 .74\n"}, {"preview", false}})));
        QVERIFY(!failed(
            change(server, "set_execution",
                   {{"settings",
                     QJsonObject{{"mode", "direct"},
                                 {"executable", QCoreApplication::applicationDirPath() + "/fake_nwchem" EXE},
                                 {"job_root", files.path()}}},
                    {"preview", false}})));
        const auto first = change(server, "calculation", {{"action", "enqueue"}});
        QVERIFY(!failed(first));
        const auto id = data(first)["queueId"].toString();
        QVERIFY(!id.isEmpty());
        QVERIFY(!failed(change(server, "calculation", {{"action", "enqueue"}})));
        const auto queue = [&] {
            return data(call(server, "get_app_state", scoped(server)))["calculation"]
                .toObject()["queue"]
                .toObject();
        };
        QCOMPARE(queue()["pending"].toInt(), 2);
        QVERIFY(failed(change(server, "calculation", {{"action", "queue_move"}, {"id", id}})));
        QVERIFY(!failed(change(server, "calculation", {{"action", "queue_cancel"}, {"id", id}})));
        QCOMPARE(queue()["pending"].toInt(), 1);
        QVERIFY(!failed(change(server, "calculation", {{"action", "queue_resume"}})));
        QTRY_COMPARE_WITH_TIMEOUT(queue()["entries"].toArray()[1].toObject()["state"].toString(), "finished",
                                  5000);
        QCOMPARE(queue()["entries"].toArray()[0].toObject()["state"].toString(), "cancelled");
        // A chain from an earlier entry; every step is checked before any is registered (H2 has no IRC).
        QVERIFY(!failed(change(server, "calculation", {{"action", "queue_pause"}})));
        const auto source =
            data(change(server, "calculation", {{"action", "enqueue"}}))["queueId"].toString();
        const auto before = queue()["entries"].toArray().size();
        QVERIFY(failed(change(
            server, "calculation",
            {{"action", "enqueue"}, {"chainFrom", source}, {"chainSteps", QJsonArray{"saddle", "irc"}}})));
        QCOMPARE(queue()["entries"].toArray().size(), before);
        const auto chain =
            change(server, "calculation",
                   {{"action", "enqueue"}, {"chainFrom", source}, {"chainSteps", QJsonArray{"saddle"}}});
        QVERIFY2(!failed(chain), qPrintable(text(chain)));
        QCOMPARE(data(chain)["queueIds"].toArray().size(), 1);
        const auto last = queue()["entries"].toArray().last().toObject();
        QCOMPARE(last["chain"].toObject()["from"].toString(), source);
        QCOMPARE(last["chain"].toObject()["structure"].toString(), "final");
        QCOMPARE(last["name"].toString(), "molforma_ts");
        QVERIFY(
            failed(change(server, "calculation",
                          {{"action", "enqueue"}, {"chainFrom", source}, {"chainStructure", "highest"}})));
        QVERIFY(failed(change(server, "calculation", {{"action", "enqueue"}, {"chainFrom", "missing"}})));
    }
    void nebEndOverMcp() {
        McpServer server;
        QVERIFY(!failed(change(server, "import_document",
                               {{"format", "xyz"},
                                {"text", "3\nHCN\nC 0 0 0\nN 0 0 1.156\nH -0.9 0 -0.7\n"},
                                {"preview", false}})));
        // Turned and shifted: fitted onto the molecule by default.
        const auto set = change(server, "set_neb_end",
                                {{"xyz", "3\nHNC\nC 1 1 1\nN 1 1 2.17\nH 1.8 1 2.85\n"}, {"preview", false}});
        QVERIFY2(!failed(set), qPrintable(text(set)));
        const auto d = committed(server);
        QCOMPARE(nwchemSettings(d)["task"].toString(), QString("neb"));
        const auto end = nwchemNebEnd(d).document;
        QVERIFY(end);
        QVERIFY(validateNwchem(d).errors.isEmpty());
        QVERIFY(nwchemInput(d).contains("geometry endgeom"));
        QVERIFY(failed(change(server, "set_neb_end", {{"xyz", "1\nH\nH 0 0 0\n"}, {"preview", false}})));
        QVERIFY(failed(change(server, "set_neb_end", {{"preview", false}})));
    }
};
int main(int argc, char **argv) {
    if (const int code = resultWorkerMain(argc, argv); code >= 0)
        return code;
    QCoreApplication app(argc, argv);
    McpServerTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_mcp_server.moc"
