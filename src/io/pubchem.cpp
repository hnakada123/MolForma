// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/pubchem.h"
#include "foundation/i18n.h"
#include "model/document.h"
#include <QCryptographicHash>
#include <QDir>
#include <QRegularExpression>
namespace mf {
std::optional<PubchemQuery> pubchemQuery(const QString &text) {
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return std::nullopt;
    static const QRegularExpression cid("^(?:cid\\s*:?\\s*)?([0-9]{1,12})$",
                                        QRegularExpression::CaseInsensitiveOption);
    if (const auto match = cid.match(trimmed); match.hasMatch())
        return PubchemQuery{"cid", QString::number(match.captured(1).toLongLong())};
    if (trimmed.size() > 300 || trimmed.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
        fail(tr("名前は改行を含めず300文字以内で入力してください。"));
    return PubchemQuery{"name", trimmed};
}
PubchemDownload::PubchemDownload(const PubchemQuery &query, const QString &folder, const QString &source,
                                 QObject *parent)
    : Download(parent), query(query), folder(folder), source(source) {
    // The file of a name keeps what it can of it, and a digest that tells names apart.
    stem = query.kind == "cid" ? "CID_" + query.value : query.value;
    if (query.kind == "name") {
        stem.replace(QRegularExpression("[^A-Za-z0-9_.-]+"), "_");
        stem =
            stem.left(40) + '_' +
            QString::fromLatin1(
                QCryptographicHash::hash(query.value.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
    }
}
void PubchemDownload::start() {
    if (started)
        return;
    started = true;
    if (stopped) {
        emit finished({});
        return;
    }
    fetch(true);
}
void PubchemDownload::cancel() {
    stopped = true;
    if (current)
        current->cancel();
}
void PubchemDownload::fetch(bool threeD) {
    const QString kind = threeD ? "3d" : "2d";
    const auto shown = query.kind == "cid" ? "CID " + query.value : query.value;
    const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(query.value));
    auto *download =
        new FileDownload(QUrl(QString(source).arg(query.kind, encoded, kind)),
                         QDir(folder).filePath(stem + '_' + kind + ".sdf"), shown,
                         tr("PubChemに %1 が見つかりません。名前・CIDを確かめてください。").arg(shown), this);
    current = download;
    connect(download, &Download::progress, this, &Download::progress);
    connect(download, &Download::finished, this, [this, download, threeD](const QString &error) {
        current = nullptr;
        download->deleteLater();
        if (!stopped && !error.isEmpty() && download->notFound() && threeD) {
            fetch(false);
            return;
        }
        if (!stopped && error.isEmpty())
            result = {download->path(), threeD};
        emit finished(stopped ? QString() : error);
    });
    download->start();
}
} // namespace mf
