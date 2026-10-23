#include "editing/chemistry.h"
#include "mcp/server.h"
#include "mcp/stdio_transport.h"
#include "ui/main_window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QLabel>
#include <QStatusBar>
#include <QTimer>
#include <cstdio>
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
        QCoreApplication::setApplicationVersion("0.1.0");
        QCommandLineParser parser;
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addOption({"mcp", "Run the headless MCP stdio server"});
        parser.addOption({"mcp-gui", "Run MCP with the editor window"});
        parser.process(app);
        if (guiMcp || !parser.positionalArguments().isEmpty()) {
            std::fprintf(stderr, "Use --mcp alone, or --mcp-gui [file] for the editor.\n");
            return 1;
        }
        try {
            mf::McpServer server;
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
    mf::MoleculeView::configureDefaultSurfaceFormat();
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("MolForma");
    QCoreApplication::setApplicationVersion("0.1.0");
    QApplication::setStyle("Fusion");
    QCommandLineParser parser;
    parser.setApplicationDescription("Qt6/C++ molecular builder extracted from KAGAMI GUI");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "XYZ/MOL/SDF/PDB/KAGAMI project");
    parser.addOption({"screenshot", "Save a screenshot and exit", "path"});
    parser.addOption({"mcp", "Run the headless MCP stdio server"});
    parser.addOption({"mcp-gui", "Run the MCP stdio server with the editor window"});
    parser.process(app);
    try {
        mf::MainWindow window;
        if (!parser.positionalArguments().isEmpty())
            window.loadPath(parser.positionalArguments().first());
        window.show();
        std::unique_ptr<mf::McpServer> server;
        std::unique_ptr<mf::McpStdioTransport> transport;
        if (parser.isSet("mcp-gui")) {
            if (parser.isSet("screenshot"))
                mf::fail("--mcp-gui and --screenshot cannot be combined.");
            mf::McpEditor editor{
                [&] { return window.document(); },
                [&]() -> std::optional<mf::Document> {
                    return window.hasCandidate() ? std::optional(window.displayDocument()) : std::nullopt;
                },
                [&](const QString &name, const std::function<void(mf::Document &)> &operation, bool preview) {
                    window.edit(name, operation, preview);
                },
                [&](const QString &action) { window.remoteHistory(action); }};
            server = std::make_unique<mf::McpServer>(editor);
            transport = std::make_unique<mf::McpStdioTransport>(server.get());
            QObject::connect(transport.get(), &mf::McpStdioTransport::finished, &app,
                             &QCoreApplication::exit);
            transport->start();
            window.statusBar()->addPermanentWidget(new QLabel("MCP stdio 接続中", &window));
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
