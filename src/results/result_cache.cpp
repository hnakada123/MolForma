// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_cache.h"
#include "foundation/files.h"
#include "results/result_codec.h"
#include "results/result_files.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>

namespace mf::result_detail {
template <typename T, typename Read>
std::optional<T> ResultCache::read(const QString &what, Read &&entry) const {
    if (stamp.isEmpty())
        return std::nullopt;
    QFile f(ResultCache::file(what));
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    try {
        const auto bytes = f.readAll();
        QDataStream s(bytes);
        s.setVersion(QDataStream::Qt_6_4);
        QString magic;
        s >> magic;
        if (magic != "MolFormaResultCache1")
            return std::nullopt;
        auto value = entry(s);
        if (s.status() != QDataStream::Ok || !s.atEnd())
            return std::nullopt;
        return value;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}
template <typename Write> void ResultCache::write(const QString &what, Write &&entry) const {
    if (stamp.isEmpty())
        return;
    try {
        QByteArray bytes;
        QDataStream s(&bytes, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_4);
        s << QString("MolFormaResultCache1");
        entry(s);
        if (s.status() != QDataStream::Ok)
            return;
        writeFile(ResultCache::file(what), bytes);
        auto entries = QDir(folder).entryInfoList({"*.bin"}, QDir::Files, QDir::Time); // Newest first.
        qint64 total = 0;
        for (const auto &e : std::as_const(entries))
            if ((total += e.size()) > 512ll * 1024 * 1024 && e.absoluteFilePath() != ResultCache::file(what))
                QFile::remove(e.absoluteFilePath());
    } catch (const std::exception &) {
        // Without the entry the file is only parsed again.
    }
}
ResultCache::ResultCache(const QString &folder, const QString &path)
    : folder(folder), stamp(folder.isEmpty() ? QString() : resultSourceStamp(path)) {}
ResultCache::operator bool() const {
    return !stamp.isEmpty();
}
std::optional<std::pair<ResultData, MoldenData>> ResultCache::molden() const {
    return read<std::pair<ResultData, MoldenData>>("molden", [](QDataStream &s) {
        QByteArray base;
        MoldenData m;
        s >> base >> m.blocks >> m.coefficients;
        return std::pair{resultFromBytes(base), m};
    });
}
void ResultCache::store(const ResultData &base, const MoldenData &m) const {
    ResultCache::write("molden",
                       [&](QDataStream &s) { s << resultBytes(base) << m.blocks << m.coefficients; });
}
std::optional<std::pair<ResultData, GridValues>> ResultCache::cube(int dataset) const {
    return read<std::pair<ResultData, GridValues>>(QString("cube %1").arg(dataset), [](QDataStream &s) {
        QByteArray base;
        GridValues g;
        s >> base >> g.values;
        ResultCache::in(s, g);
        return std::pair{resultFromBytes(base), g};
    });
}
void ResultCache::store(int dataset, const ResultData &base, const GridValues &g) const {
    ResultCache::write(QString("cube %1").arg(dataset), [&](QDataStream &s) {
        s << resultBytes(base) << g.values;
        ResultCache::out(s, g);
    });
}
std::optional<std::pair<GridValues, QString>> ResultCache::orbital(const QString &key) const {
    return read<std::pair<GridValues, QString>>(key, [](QDataStream &s) {
        GridValues g;
        QString normalization;
        s >> g.values >> normalization;
        ResultCache::in(s, g);
        return std::pair{g, normalization};
    });
}
void ResultCache::store(const QString &key, const GridValues &g, const QString &normalization) const {
    ResultCache::write(key, [&](QDataStream &s) {
        s << g.values << normalization;
        ResultCache::out(s, g);
    });
}

QString ResultCache::file(const QString &what) const {
    const auto key = QCryptographicHash::hash((stamp + '\n' + what).toUtf8(), QCryptographicHash::Sha256);
    return QDir(folder).filePath(QString::fromLatin1(key.toHex()) + ".bin");
}
void ResultCache::out(QDataStream &s, const GridValues &g) {
    for (int i = 0; i < 3; ++i)
        s << g.origin[i];
    for (int i = 0; i < 9; ++i)
        s << g.axes.data()[i];
}
void ResultCache::in(QDataStream &s, GridValues &g) {
    for (int i = 0; i < 3; ++i)
        s >> g.origin[i];
    for (int i = 0; i < 9; ++i)
        s >> g.axes.data()[i];
}

} // namespace mf::result_detail
