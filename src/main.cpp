// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry_job.h"
#include "mcp/headless.h"
#include "mcp/server.h"
#include "mcp/stdio_transport.h"
#include "model/i18n.h"
#include "model/settings.h"
#include "results/results.h"
#include "ui/main_window.h"
#include "ui/mcp_gui.h"
#include "ui/translations.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QStatusBar>
#include <QTimer>
#include <cstdio>
namespace {
void addMcpOptions(QCommandLineParser &parser) {
    parser.addOption({"mcp-allow-dir", mf::tr("MCPのファイル操作をこのディレクトリに限定する（複数指定可）"),
                      "directory"});
    parser.addOption({"mcp-allow-execution-settings",
                      mf::tr("MCPクライアントにNWChem実行ファイル・ランチャー・環境変数の変更を許可する")});
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
#ifdef Q_OS_WIN
    // Windows has no XDG folders; honoring XDG_CONFIG_HOME moves MolForma.ini there as on Linux (tests).
    if (const auto config = qEnvironmentVariable("XDG_CONFIG_HOME"); !config.isEmpty())
        QSettings::setPath(mf::SettingsFormat, QSettings::UserScope, config);
#endif
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
        mf::installTranslations(app);
        QCommandLineParser parser;
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addOption({"mcp", mf::tr("画面なしのMCPサーバーを標準入出力で起動する")});
        parser.addOption({"mcp-gui", mf::tr("編集画面つきでMCPサーバーを起動する")});
        addMcpOptions(parser);
        parser.process(app);
        if (guiMcp || !parser.positionalArguments().isEmpty()) {
            std::fprintf(stderr, "Use --mcp alone, or --mcp-gui [file] for the editor.\n");
            return 1;
        }
        try {
            const auto policy = mcpPolicy(parser);
            // The executable, launcher and environment saved in Preferences are the user's choice.
            mf::Document initial;
            mf::applyExecutionPreferences(initial);
            mf::McpHeadless runtime(policy, mf::protectedExecution(initial));
            mf::McpServer server(runtime.editor(), policy);
            server.resetDocument(initial);
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
    if (const int code = mf::resultWorkerMain(argc, argv, mf::inheritTranslations); code >= 0)
        return code;
    // The same native executable acts as a cancellable chemistry worker; no Python runtime.
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        mf::inheritTranslations();
        return mf::chemistryWorker(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
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
    mf::installTranslations(app);
    QApplication::setStyle("Fusion");
    Q_INIT_RESOURCE(manual);
    QApplication::setWindowIcon(QIcon(":/icons/molforma.png"));
    QGuiApplication::setDesktopFileName("molforma"); // Matches packaging/molforma.desktop.
    QCommandLineParser parser;
    parser.setApplicationDescription(
        mf::tr("分子の作成・編集と、NWChemの入力作成・実行・結果表示（Qt6 / C++）"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(
        "file",
        mf::tr("開くファイル（XYZ・MOL・SDF・PDB・mmCIF・MolFormaプロジェクト .molforma.json など）"));
    parser.addOption({"screenshot", mf::tr("画面をPNGで保存して終了する"), "path"});
    parser.addOption({"mcp", mf::tr("画面なしのMCPサーバーを標準入出力で起動する")});
    parser.addOption({"mcp-gui", mf::tr("編集画面つきでMCPサーバーを起動する")});
    addMcpOptions(parser);
    parser.process(app);
    try {
        // Refuse contradictory options before a window opens, a file loads or autosave starts.
        if (!parser.isSet("mcp-gui") &&
            (parser.isSet("mcp-allow-dir") || parser.isSet("mcp-allow-execution-settings")))
            mf::fail("--mcp-allow-dir and --mcp-allow-execution-settings require --mcp or --mcp-gui.");
        if (parser.isSet("mcp-gui") && parser.isSet("screenshot"))
            mf::fail("--mcp-gui and --screenshot cannot be combined.");
        const auto policy = mcpPolicy(parser);
        mf::MainWindow window;
        if (!guiMcp && !parser.positionalArguments().isEmpty())
            window.loadPath(parser.positionalArguments().first());
        window.show();
        if (!parser.isSet("screenshot")) {
            window.startAutosave();
            // A person decides about recovered work; MCP sessions start without the question.
            if (!guiMcp)
                QTimer::singleShot(0, &window, [&window] { window.safe([&] { window.offerRecovery(); }); });
        }
        std::unique_ptr<mf::McpServer> server;
        std::unique_ptr<mf::McpGui> gui;
        std::unique_ptr<mf::McpStdioTransport> transport;
        if (parser.isSet("mcp-gui")) {
            gui = std::make_unique<mf::McpGui>(window, policy);
            server = std::make_unique<mf::McpServer>(gui->editor(), policy);
            transport = std::make_unique<mf::McpStdioTransport>(server.get());
            QObject::connect(transport.get(), &mf::McpStdioTransport::finished, &app,
                             &QCoreApplication::exit);
            transport->start();
            window.statusBar()->addPermanentWidget(new QLabel(mf::tr("MCP stdio 接続中"), &window));
            // The dialogs of what to read (PDB, mmCIF, XYZ frames) and of load errors must already be
            // reachable over MCP.
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
