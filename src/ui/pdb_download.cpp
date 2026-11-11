// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/pdb_download.h"
#include "model/document.h"
#include "model/i18n.h"
#include <QDir>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSaveFile>

namespace mf {
namespace {
// Far above the largest entries of the PDB, compressed.
constexpr qint64 MaxBytes = qint64(256) << 20;
} // namespace
QString pdbId(const QString &text) {
    static const QRegularExpression classic("^[0-9][A-Za-z0-9]{3}$"),
        extended("^pdb_[0-9]{4}[0-9a-z]{4}$", QRegularExpression::CaseInsensitiveOption);
    const auto id = text.trimmed();
    if (classic.match(id).hasMatch())
        return id.toUpper();
    if (extended.match(id).hasMatch())
        return id.toLower();
    return {};
}
FileDownload::FileDownload(const QUrl &url, const QString &target, const QString &name,
                           const QString &missing, QObject *parent)
    : QObject(parent), name(name), missing(missing), target(target), url(url),
      network(new QNetworkAccessManager(this)) {}
FileDownload::~FileDownload() {
    if (reply)
        reply->abort();
}
PdbDownload::PdbDownload(const QString &id, const QString &folder, const QString &source, QObject *parent)
    : FileDownload(QUrl(QString(source).arg(id)), QDir(folder).filePath(id + ".cif.gz"), id,
                   tr("RCSB PDBに %1 のmmCIFファイルがありません。PDB IDを確かめてください。").arg(id),
                   parent) {}
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
QString downloadPdb(QWidget *parent, const QString &id, const QString &folder, const QString &source) {
    PdbDownload download(id, folder, source);
    return runDownload(parent, download, tr("PDBからダウンロード"),
                       tr("%1 をRCSB PDBからダウンロードしています…").arg(id));
}
QString runDownload(QWidget *parent, FileDownload &download, const QString &title, const QString &label) {
    QProgressDialog dialog(label, tr("中止"), 0, 0, parent);
    dialog.setObjectName("pdbDownloadProgress");
    dialog.setWindowTitle(title);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(0);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    QEventLoop loop;
    bool done = false;
    QString error;
    QObject::connect(&download, &FileDownload::progress, &dialog, [&](qint64 received, qint64 total) {
        if (total > 0) { // In KiB, which a progress bar's int holds.
            dialog.setMaximum(int(total >> 10));
            dialog.setValue(int(std::min(received, total) >> 10));
        }
    });
    QObject::connect(&download, &FileDownload::finished, &loop, [&](const QString &message) {
        error = message;
        done = true;
        loop.quit();
    });
    QObject::connect(&dialog, &QProgressDialog::canceled, &download, &FileDownload::cancel);
    download.start();
    if (!done)
        loop.exec();
    if (download.cancelled())
        return {};
    if (!error.isEmpty())
        fail(error);
    return download.path();
}
} // namespace mf
