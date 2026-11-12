// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/result_files.h"
#include "results/result_files.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
namespace mf {
QStringList nwchemResultFiles(const QString &directory) {
    auto files = resultFiles(directory);
    // NEB writes its path after every step (print_shift); the log holds the same paths.
    static const QRegularExpression nebStep(R"(\.nebpath_\d+\.xyz$)");
    files.removeIf([](const QString &file) { return nebStep.match(file).hasMatch(); });
    // Generated files can live in permanent_dir; do not recursively scan it or trust the saved cwd.
    QFile record(QDir(directory).filePath("run.json"));
    if (record.open(QIODevice::ReadOnly) && record.size() <= 256 * 1024) {
        const auto m = QJsonDocument::fromJson(record.read(256 * 1024)).object();
        if (m["format"] == "molforma-nwchem-run" && m["version"].toDouble() == 1) {
            QSet<QString> known;
            for (const auto &file : files)
                known.insert(QFileInfo(QDir(directory).filePath(file)).canonicalFilePath());
            const auto outputs = m["visualization_files"].toArray();
            for (int i = 0; i < std::min(2, int(outputs.size())); ++i) {
                const auto name = outputs[i].toString();
                const auto clean = QDir::cleanPath(name);
                if (clean == ".." || clean.startsWith("../"))
                    continue;
                const QFileInfo info(QDir(directory).filePath(name));
                if (!QStringList{"cube", "cub", "molden"}.contains(info.suffix().toLower()) ||
                    !info.isFile() || !info.isReadable() || known.contains(info.canonicalFilePath()))
                    continue;
                known.insert(info.canonicalFilePath());
                files << name;
            }
        }
    }
    return files;
}
} // namespace mf
