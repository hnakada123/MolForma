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
PdbDownload::PdbDownload(const QString &id, const QString &folder, const QString &source, QObject *parent)
    : QObject(parent), id(id), target(QDir(folder).filePath(id + ".cif.gz")), url(QString(source).arg(id)),
      network(new QNetworkAccessManager(this)) {}
PdbDownload::~PdbDownload() {
    if (reply)
        reply->abort();
}
void PdbDownload::start() {
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
            failure = tr("%1 のファイルが %2 MiB を超えるため中止しました。").arg(id).arg(MaxBytes >> 20);
            reply->abort();
        } else if (file->write(bytes) != bytes.size()) {
            failure = file->errorString();
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, &PdbDownload::progress);
    connect(reply, &QNetworkReply::finished, this, &PdbDownload::finish);
}
void PdbDownload::cancel() {
    stopped = true;
    if (reply)
        reply->abort();
}
void PdbDownload::finish() {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (failure.isEmpty() && !stopped) {
        if (status == 404 || reply->error() == QNetworkReply::ContentNotFoundError)
            failure = tr("RCSB PDBに %1 のmmCIFファイルがありません。PDB IDを確かめてください。").arg(id);
        else if (reply->error() != QNetworkReply::NoError)
            failure = tr("%1 をダウンロードできません: %2").arg(id, reply->errorString());
        else if (!file->commit())
            failure = file->errorString();
    }
    if (!failure.isEmpty() || stopped)
        file->cancelWriting();
    reply->deleteLater();
    emit finished(stopped ? QString() : failure);
}
QString downloadPdb(QWidget *parent, const QString &id, const QString &folder, const QString &source) {
    PdbDownload download(id, folder, source);
    QProgressDialog dialog(tr("%1 をRCSB PDBからダウンロードしています…").arg(id), tr("中止"), 0, 0, parent);
    dialog.setObjectName("pdbDownloadProgress");
    dialog.setWindowTitle(tr("PDBからダウンロード"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(0);
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    QEventLoop loop;
    bool done = false;
    QString error;
    QObject::connect(&download, &PdbDownload::progress, &dialog, [&](qint64 received, qint64 total) {
        if (total > 0) { // In KiB, which a progress bar's int holds.
            dialog.setMaximum(int(total >> 10));
            dialog.setValue(int(std::min(received, total) >> 10));
        }
    });
    QObject::connect(&download, &PdbDownload::finished, &loop, [&](const QString &message) {
        error = message;
        done = true;
        loop.quit();
    });
    QObject::connect(&dialog, &QProgressDialog::canceled, &download, &PdbDownload::cancel);
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
