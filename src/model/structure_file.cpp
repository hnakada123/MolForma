// SPDX-License-Identifier: GPL-2.0-or-later
#include "model/structure_file.h"
#include "model/document.h"
#include "model/i18n.h"
#include <QFileInfo>
#include <QScopeGuard>
#include <new>
#include <zlib.h>

namespace mf {
bool compressedFile(const QString &path) {
    return path.endsWith(".gz", Qt::CaseInsensitive);
}
QString structureSuffix(const QString &path) {
    auto name = QFileInfo(path).fileName().toLower();
    if (name.endsWith(".gz"))
        name.chop(3);
    return QFileInfo(name).suffix();
}
QByteArray readStructureFile(const QString &path, qint64 limit) {
    const auto data = readFile(path);
    if (!compressedFile(path))
        return data;
    const auto name = QFileInfo(path).fileName();
    const auto tooLarge = [&] {
        fail(tr("%1 は展開すると %2 MiB を超えるため読み込めません。").arg(name).arg(limit >> 20));
    };
    if (data.size() > limit)
        tooLarge();
    z_stream stream{};
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) // 16: with the gzip header and trailer.
        throw std::bad_alloc();
    const auto end = qScopeGuard([&] { inflateEnd(&stream); });
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = uInt(data.size());
    QByteArray out;
    char buffer[1 << 16];
    for (;;) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof buffer;
        const int status = inflate(&stream, Z_NO_FLUSH);
        // Z_BUF_ERROR: the data ended before the compressed stream did.
        if (status != Z_OK && status != Z_STREAM_END)
            fail(tr("%1 をgzipの圧縮データとして読めません（壊れているか、途中で切れています）。").arg(name));
        out.append(buffer, sizeof buffer - stream.avail_out);
        if (out.size() > limit)
            tooLarge();
        if (status == Z_STREAM_END) {
            if (!stream.avail_in)
                return out;
            inflateReset(&stream); // Another member follows, as in files joined with cat.
        }
    }
}
} // namespace mf
