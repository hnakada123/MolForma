// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/i18n.h"
#include "results/result_parsing.h"
#include "results/results.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

namespace mf {
using namespace result_detail;
int resultWorker(const QString &request, const QString &response) {
    try {
        const auto q = parseJson(boundedRead(request));
        auto path = q["path"].toString();
        // A combination of CUBE files is written first, then read like any CUBE.
        if (const auto terms = q["combine"].toArray(); !terms.isEmpty()) {
            QVector<CubeTerm> combine;
            for (const auto &value : terms) {
                const auto term = value.toObject();
                combine.append({term["path"].toString(), term["factor"].toDouble(), term["dataset"].toInt()});
            }
            combineCubes(combine, path);
        }
        const auto animation = q["animation_files"].toArray();
        if (!animation.isEmpty()) {
            // The loader owns this scratch directory, so cancellation removes the joined file too.
            // Bound each read before allocating; all source I/O runs in this cancellable worker.
            constexpr qint64 limit = 64 * 1024 * 1024;
            QByteArray joined;
            for (const auto &source : animation) {
                QFile file(source.toString());
                if (!file.open(QIODevice::ReadOnly))
                    fail(file.errorString());
                const auto remaining = limit - joined.size();
                if (file.size() > remaining)
                    fail(tr("振動アニメーションのファイルが大きすぎます。"));
                joined += file.read(remaining + 1);
                if (file.error() != QFileDevice::NoError)
                    fail(file.errorString());
                if (!joined.endsWith('\n'))
                    joined += '\n';
                if (joined.size() > limit)
                    fail(tr("振動アニメーションのファイルが大きすぎます。"));
            }
            path = QDir(QFileInfo(response).absolutePath()).filePath("animation.xyz");
            writeFile(path, joined);
        }
        auto data = readResult(path, q["isovalue"].toDouble(.03), q["dataset"].toInt(),
                               q["orbital"].toInt(-1), q["normalization"].toString("auto"),
                               q["orbital_divisions"].toInt(40), q["cache"].toString());
        data.source = QFileInfo(q["path"].toString()).absoluteFilePath();
        saveResultData(response, data);
        return 0;
    } catch (const std::exception &e) {
        try {
            writeFile(response + ".error", QByteArray(e.what()).left(4096));
        } catch (...) {
        }
        return 1;
    }
}
int resultWorkerMain(int argc, char **argv, void (*prepare)()) {
    const auto mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const bool ready = argc == 2 && mode == "--result-worker-ready";
    if (!ready && !(argc == 4 && mode == "--result-worker"))
        return -1;
    QCoreApplication app(argc, argv);
    if (prepare)
        prepare();
    if (!ready)
        return resultWorker(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return 1;
    const auto paths = QJsonDocument::fromJson(input.readLine()).object(); // Waits for the request.
    if (!paths["request"].isString() || !paths["response"].isString())
        return 1;
    return resultWorker(paths["request"].toString(), paths["response"].toString());
}

} // namespace mf
