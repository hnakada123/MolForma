#include "editing/chemistry.h"
#include "mcp/headless.h"
#include "mcp/server.h"
#include "mcp/stdio_transport.h"
#include "ui/main_window.h"
#include "ui/mcp_gui.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QLabel>
#include <QStatusBar>
#include <QTimer>
#include <cstdio>
namespace {
void addMcpOptions(QCommandLineParser &parser) {
    parser.addOption({"mcp-allow-dir", "Limit MCP file access to this directory (repeatable)", "directory"});
    parser.addOption({"mcp-allow-execution-settings",
                      "Let MCP clients change the NWChem executable, launcher and environment overrides"});
}
// MCP restrictions come only from the command line that the user configured in the client.
mf::McpPolicy mcpPolicy(const QCommandLineParser &parser) {
    mf::McpPolicy policy;
    policy.allowExecutionSettings = parser.isSet("mcp-allow-execution-settings");
    for (const auto &directory : parser.values("mcp-allow-dir"))
        policy.allowDirectory(directory);
    return policy;
}
} // namespace
int main(int argc, char **argv) {
    // Check before QApplication so headless MCP needs no display or platform plugin.
    bool headlessMcp = false, guiMcp = false;
    for (int i = 1; i < argc; ++i) {
        const auto arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--")
            break;
        headlessMcp |= arg == "--mcp";
        guiMcp |= arg == "--mcp-gui";
    }
    if (headlessMcp) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName("MolForma");
        QCoreApplication::setApplicationVersion(MOLFORMA_VERSION);
        QCommandLineParser parser;
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addOption({"mcp", "Run the headless MCP stdio server"});
        parser.addOption({"mcp-gui", "Run MCP with the editor window"});
        addMcpOptions(parser);
        parser.process(app);
        if (guiMcp || !parser.positionalArguments().isEmpty()) {
            std::fprintf(stderr, "Use --mcp alone, or --mcp-gui [file] for the editor.\n");
            return 1;
        }
        try {
            const auto policy = mcpPolicy(parser);
            mf::McpHeadless runtime(policy);
            mf::McpServer server(runtime.editor(), policy);
            runtime.attach(server);
            mf::McpStdioTransport transport(&server);
            QObject::connect(&transport, &mf::McpStdioTransport::finished, &app, &QCoreApplication::exit);
            transport.start();
            return app.exec();
        } catch (const std::exception &e) {
            std::fprintf(stderr, "MolForma MCP: %s\n", e.what());
            return 1;
        }
    }
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--result-worker") {
        QCoreApplication app(argc, argv);
        return mf::resultWorker(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    }
    // The same native executable acts as a cancellable chemistry worker; no Python runtime.
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        QString input = QString::fromLocal8Bit(argv[2]), output = QString::fromLocal8Bit(argv[3]);
        try {
            auto request = mf::parseJson(mf::readFile(input));
            auto mode = request["mode"].toString();
            auto d = mode == "smiles" ? mf::fromSmiles(request["smiles"].toString())
                                      : mf::refine(mf::Document::fromJson(request["document"].toObject()),
                                                   mode == "regenerate", mf::idsFromJson(request["moving"]));
            mf::writeFile(output, QJsonDocument(QJsonObject{{"document", d.toJson()}}).toJson());
            return 0;
        } catch (const std::exception &e) {
            try {
                mf::writeFile(output,
                              QJsonDocument(QJsonObject{{"error", QString::fromUtf8(e.what())}}).toJson());
            } catch (...) {
            }
            return 1;
        }
    }
#ifdef Q_OS_LINUX
    // MCP clients may filter the child environment. Diagnose missing desktop variables
    // before Qt aborts while loading its platform plugin, closing the handshake pipe.
    if (guiMcp && qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
        qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        std::fprintf(stderr, "MolForma MCP: --mcp-gui requires a desktop display. Forward DISPLAY, "
                             "WAYLAND_DISPLAY, XDG_RUNTIME_DIR and XAUTHORITY through the MCP client's "
                             "environment (Codex: mcp_servers.molforma.env_vars), or use --mcp for "
                             "headless operation.\n");
        return 1;
    }
#endif
    mf::MoleculeView::configureDefaultSurfaceFormat();
    if (guiMcp)
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("MolForma");
    QCoreApplication::setApplicationVersion(MOLFORMA_VERSION);
    QApplication::setStyle("Fusion");
    QCommandLineParser parser;
    parser.setApplicationDescription("Qt6/C++ molecular editor and NWChem input, run and result front end");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "XYZ/MOL/SDF/PDB/KAGAMI project");
    parser.addOption({"screenshot", "Save a screenshot and exit", "path"});
    parser.addOption({"mcp", "Run the headless MCP stdio server"});
    parser.addOption({"mcp-gui", "Run the MCP stdio server with the editor window"});
    addMcpOptions(parser);
    parser.process(app);
    try {
        mf::MainWindow window;
        if (!guiMcp && !parser.positionalArguments().isEmpty())
            window.loadPath(parser.positionalArguments().first());
        window.show();
        std::unique_ptr<mf::McpServer> server;
        std::unique_ptr<mf::McpGui> gui;
        std::unique_ptr<mf::McpStdioTransport> transport;
        if (!parser.isSet("mcp-gui") &&
            (parser.isSet("mcp-allow-dir") || parser.isSet("mcp-allow-execution-settings")))
            mf::fail("--mcp-allow-dir and --mcp-allow-execution-settings require --mcp or --mcp-gui.");
        if (parser.isSet("mcp-gui")) {
            if (parser.isSet("screenshot"))
                mf::fail("--mcp-gui and --screenshot cannot be combined.");
            const auto policy = mcpPolicy(parser);
            gui = std::make_unique<mf::McpGui>(window, policy);
            server = std::make_unique<mf::McpServer>(gui->editor(), policy);
            transport = std::make_unique<mf::McpStdioTransport>(server.get());
            QObject::connect(transport.get(), &mf::McpStdioTransport::finished, &app,
                             &QCoreApplication::exit);
            transport->start();
            window.statusBar()->addPermanentWidget(new QLabel("MCP stdio 接続中", &window));
            // PDB MODEL/ALTLOC and load-error dialogs must already be reachable over MCP.
            if (!parser.positionalArguments().isEmpty()) {
                const auto path = parser.positionalArguments().first();
                QTimer::singleShot(0, &window,
                                   [&window, path] { window.safe([&] { window.loadPath(path); }); });
            }
        }
        if (parser.isSet("screenshot")) {
            QTimer::singleShot(500, &window, [&] {
                window.canvas()->fit();
                QTimer::singleShot(100, &window, [&] {
                    bool ok = window.grab().save(parser.value("screenshot"));
                    app.exit(ok ? 0 : 1);
                });
            });
        }
        return app.exec();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "MolForma: %s\n", e.what());
        return 1;
    }
}
