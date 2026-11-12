// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QPointer>
#include <QUrl>
class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
namespace mf {
// A cancellable transfer, usable with a GUI or a QCoreApplication. One start per instance.
class Download : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual void start() = 0;
    virtual void cancel() = 0;
    virtual bool cancelled() const = 0;
    virtual QString path() const = 0;
  signals:
    void progress(qint64 received, qint64 total);
    void finished(const QString &error); // Empty on success or cancellation; cancelled() tells them apart.
};
// Downloads into a temporary file and replaces the target only on success. Redirects cannot
// downgrade HTTPS; stalled transfers stop after one minute. The receiver chooses the URL.
class FileDownload : public Download {
    Q_OBJECT
  public:
    FileDownload(const QUrl &url, const QString &target, const QString &name, const QString &missing,
                 QObject *parent = nullptr);
    ~FileDownload() override;
    void start() override;
    void cancel() override;
    bool cancelled() const override {
        return stopped;
    }
    QString path() const override {
        return target;
    }
    bool notFound() const {
        return absent;
    }

  private:
    QString name, missing, target, failure;
    QUrl url;
    QNetworkAccessManager *network;
    QPointer<QNetworkReply> reply;
    QSaveFile *file = nullptr;
    qint64 received = 0;
    bool stopped = false, absent = false;
    void finish();
};
} // namespace mf
