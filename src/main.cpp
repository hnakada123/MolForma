#include "chemistry.h"
#include "main_window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
int main(int argc, char **argv) {
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
    parser.process(app);
    try {
        mf::MainWindow window;
        if (!parser.positionalArguments().isEmpty())
            window.loadPath(parser.positionalArguments().first());
        window.show();
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
