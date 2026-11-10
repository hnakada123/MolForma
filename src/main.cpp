// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry_job.h"
#include "mcp/headless.h"
#include "mcp/server.h"
#include "mcp/stdio_transport.h"
#include "model/i18n.h"
#include "model/settings.h"
#include "nwchem/nwchem_execution.h"
#include "results/results.h"
#include "ui/execution_panel.h"
#include "ui/main_window.h"
#include "ui/mcp_gui.h"
#include "ui/translations.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QStatusBar>
#include <QTimer>
#include <cstdio>
#ifdef Q_OS_MACOS
#include <mach-o/dyld.h>
#include <string>
#endif
namespace {
#ifdef Q_OS_MACOS
// MolForma.app (docs/installation/macos.md): Open Babel's format plugins and data are the copies in the
// bundle, and PATH also holds Homebrew's folders, which apps started from Finder do not get (NWChem and
// mpirun in "3 実行設定"). Workers started by this process inherit both.
void prepareBundle() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string executable(size, '\0');
    if (_NSGetExecutablePath(executable.data(), &size) != 0)
        return;
    const QDir contents(QFileInfo(QString::fromUtf8(executable.c_str())).canonicalPath() + "/..");
    if (!QFileInfo(contents.filePath("Info.plist")).isFile() || !contents.exists("PlugIns/openbabel"))
        return; // The executable of the build folder.
    if (qEnvironmentVariableIsEmpty("BABEL_LIBDIR"))
        qputenv("BABEL_LIBDIR", QFile::encodeName(contents.absoluteFilePath("PlugIns/openbabel")));
    if (qEnvironmentVariableIsEmpty("BABEL_DATADIR"))
        qputenv("BABEL_DATADIR", QFile::encodeName(contents.absoluteFilePath("Resources/openbabel")));
    auto path = qEnvironmentVariable("PATH").split(':', Qt::SkipEmptyParts);
    for (const QString folder : {"/opt/homebrew/bin", "/usr/local/bin"})
        if (!path.contains(folder) && QFileInfo(folder).isDir())
            path << folder;
    qputenv("PATH", QFile::encodeName(path.join(':')));
}
// Files opened from Finder (double-click, "Open With", a drop on the Dock icon) arrive as events.
class FinderFiles : public QObject {
  public:
    explicit FinderFiles(mf::MainWindow &window) : QObject(&window), window(window) {}
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::FileOpen)
            return QObject::eventFilter(watched, event);
        if (const auto path = static_cast<QFileOpenEvent *>(event)->file(); !path.isEmpty())
            QTimer::singleShot(0, &window, [w = &window, path] {
                w->safe([&] {
                    if (w->confirmDiscard())
                        w->loadPath(path);
                });
            });
        return true;
    }

  private:
    mf::MainWindow &window;
};
#endif
void addMcpOptions(QCommandLineParser &parser) {
    parser.addOption({"mcp-allow-dir", mf::tr("MCPのファイル操作をこのディレクトリに限定する（複数指定可）"),
                      "directory"});
    parser.addOption({"mcp-allow-execution-settings",
                      mf::tr("MCPクライアントにNWChem実行ファイル・ランチャー・環境変数の変更を許可する")});
    parser.addOption({"mcp-keep-runs", mf::tr("MCPの接続が終わっても、実行中の計算を止めずに続ける")});
}
// MCP restrictions come only from the command line that the user configured in the client.
mf::McpPolicy mcpPolicy(const QCommandLineParser &parser) {
    mf::McpPolicy policy;
    policy.allowExecutionSettings = parser.isSet("mcp-allow-execution-settings");
    policy.keepRuns = parser.isSet("mcp-keep-runs");
    for (const auto &directory : parser.values("mcp-allow-dir"))
        policy.allowDirectory(directory);
    return policy;
}
} // namespace
int main(int argc, char **argv) {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // Windows and macOS have no XDG folders; honoring XDG_CONFIG_HOME moves MolForma.ini there as on
    // Linux (tests).
    if (const auto config = qEnvironmentVariable("XDG_CONFIG_HOME"); !config.isEmpty())
        QSettings::setPath(mf::SettingsFormat, QSettings::UserScope, config);
#ifdef Q_OS_MACOS
    else // Qt would put the INI file under ~/.config: keep it where macOS keeps preferences.
        QSettings::setPath(mf::SettingsFormat, QSettings::UserScope,
                           QDir::homePath() + "/Library/Preferences");
    prepareBundle();
#endif
#endif
    // The supervisor of a calculation: outlives the window that started it, and needs no display.
    if (const int code = mf::runJobMain(argc, argv); code >= 0)
        return code;
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
#ifdef Q_OS_MACOS
    // Qt's macOS theme gives tool buttons the 10 pt of native toolbars, against 13 pt for everything else.
    // MolForma's header, tool rail and chips are tool buttons, and should read like the rest of the window.
    QApplication::setFont(QApplication::font(), "QToolButton");
#endif
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
            (parser.isSet("mcp-allow-dir") || parser.isSet("mcp-allow-execution-settings") ||
             parser.isSet("mcp-keep-runs")))
            mf::fail("--mcp-allow-dir, --mcp-allow-execution-settings and --mcp-keep-runs require --mcp or "
                     "--mcp-gui.");
        if (parser.isSet("mcp-gui") && parser.isSet("screenshot"))
            mf::fail("--mcp-gui and --screenshot cannot be combined.");
        const auto policy = mcpPolicy(parser);
        mf::MainWindow window;
        // A screenshot is taken without a person to answer the dialogs of what to read.
        if (!guiMcp && !parser.positionalArguments().isEmpty())
            window.loadPath(parser.positionalArguments().first(), !parser.isSet("screenshot"));
        window.show();
#ifdef Q_OS_MACOS
        if (!guiMcp && !parser.isSet("screenshot"))
            app.installEventFilter(new FinderFiles(window));
#endif
        if (!parser.isSet("screenshot")) {
            window.startAutosave();
            // Calculations get a supervisor of their own, and go on when the window closes. From an
            // AppImage that is the image itself, which stays mounted for as long as it runs.
            window.executionPanel()->superviseRuns(mf::runSupervisorProgram());
            // A person decides about recovered work and about what the last session left running or
            // waiting. An MCP session starts without the question and the queue; with --mcp-keep-runs
            // it finds the calculation the last one left running.
            const bool keepRuns = policy.keepRuns;
            if (!guiMcp || keepRuns)
                QTimer::singleShot(0, &window, [&window, guiMcp] {
                    window.safe([&] {
                        if (const auto note = window.executionPanel()->restoreRuns(!guiMcp); !note.isEmpty())
                            window.statusBar()->showMessage(note, 20000);
                        if (!guiMcp)
                            window.offerRecovery();
                    });
                });
        }
        std::unique_ptr<mf::McpServer> server;
        std::unique_ptr<mf::McpGui> gui;
        std::unique_ptr<mf::McpStdioTransport> transport;
        if (parser.isSet("mcp-gui")) {
            gui = std::make_unique<mf::McpGui>(window, policy);
            server = std::make_unique<mf::McpServer>(gui->editor(), policy);
            transport = std::make_unique<mf::McpStdioTransport>(server.get());
            QObject::connect(transport.get(), &mf::McpStdioTransport::finished, &app,
                             [&window, keep = policy.keepRuns](int code) {
                                 // The session ends: its calculation ends with it unless it is to go on.
                                 if (keep)
                                     window.executionPanel()->shutdown(true);
                                 QCoreApplication::exit(code);
                             });
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
