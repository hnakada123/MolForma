// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/file_download.h"
#include "model/i18n.h"
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
namespace mf {
namespace {
constexpr qint64 MaxBytes = qint64(256) << 20;
}
FileDownload::FileDownload(const QUrl &url, const QString &target, const QString &name,
                           const QString &missing, QObject *parent)
    : Download(parent), name(name), missing(missing), target(target), url(url),
      network(new QNetworkAccessManager(this)) {}
FileDownload::~FileDownload() {
    if (reply)
        reply->abort();
}
void FileDownload::start() {
    QDir().mkpath(QFileInfo(target).absolutePath());
    file = new QSaveFile(target, this);
    if (!file->open(QIODevice::WriteOnly)) {
        failure = file->errorString();
        emit finished(failure);
        return;
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QString("MolForma/%1").arg(MOLFORMA_VERSION));
    // Redirects stay on HTTPS; a transfer that stalls for a minute stops.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60000);
    reply = network->get(request);
    connect(reply, &QNetworkReply::readyRead, this, [this] {
        if (!reply->isOpen()) // Aborted, or failed to open.
            return;
        const auto bytes = reply->readAll();
        received += bytes.size();
        if (received > MaxBytes) {
            failure = tr("%1 のファイルが %2 MiB を超えるため中止しました。").arg(name).arg(MaxBytes >> 20);
            reply->abort();
        } else if (file->write(bytes) != bytes.size()) {
            failure = file->errorString();
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, &FileDownload::progress);
    connect(reply, &QNetworkReply::finished, this, &FileDownload::finish);
}
void FileDownload::cancel() {
    stopped = true;
    if (reply)
        reply->abort();
}
void FileDownload::finish() {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (failure.isEmpty() && !stopped) {
        if (status == 404 || reply->error() == QNetworkReply::ContentNotFoundError) {
            failure = missing;
            absent = true;
        } else if (reply->error() != QNetworkReply::NoError)
            failure = tr("%1 をダウンロードできません: %2").arg(name, reply->errorString());
        else if (!file->commit())
            failure = file->errorString();
    }
    if (!failure.isEmpty() || stopped) {
        // Destroying it removes the temporary file now: on Windows it has a name in the folder until then.
        file->cancelWriting();
        delete file;
        file = nullptr;
    }
    reply->deleteLater();
    emit finished(stopped ? QString() : failure);
}
} // namespace mf
