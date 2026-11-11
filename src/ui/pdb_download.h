// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QPointer>
#include <QUrl>
class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
class QWidget;
namespace mf {
// Entries of the Protein Data Bank downloaded by their ID: the gzip-compressed mmCIF files of RCSB PDB. Only
// the ID goes out, to this address alone.
constexpr auto PdbSource = "https://files.rcsb.org/download/%1.cif.gz";
// The PDB ID in a text: four characters (7DMU, in upper case) or an extended ID (pdb_00007dmu, in lower
// case); empty when the text is no PDB ID.
QString pdbId(const QString &text);
// A file downloaded over HTTPS (redirects stay on HTTPS, a transfer that stalls for a minute stops) into
// target, which it replaces only once complete. name: what the messages call it ("7DMU"); missing: the
// message when the server has no such file.
class FileDownload : public QObject {
    Q_OBJECT
  public:
    FileDownload(const QUrl &url, const QString &target, const QString &name, const QString &missing,
                 QObject *parent = nullptr);
    ~FileDownload() override;
    void start();
    void cancel();
    bool cancelled() const {
        return stopped;
    }
    // The server answered that it has no such file (HTTP 404).
    bool notFound() const {
        return absent;
    }
    QString path() const {
        return target;
    }
  signals:
    void progress(qint64 received, qint64 total);
    void finished(const QString &error); // Empty when the file is there, or when cancelled.

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
class PdbDownload : public FileDownload {
    Q_OBJECT
  public:
    // The entry's file goes to folder as <ID>.cif.gz. source has %1 for the ID.
    PdbDownload(const QString &id, const QString &folder, const QString &source = PdbSource,
                QObject *parent = nullptr);
};
// Runs a download with a dialog that shows the progress and can cancel it. Returns the file, or an empty
// string when cancelled; fails with the reason.
QString runDownload(QWidget *parent, FileDownload &download, const QString &title, const QString &label);
QString downloadPdb(QWidget *parent, const QString &id, const QString &folder,
                    const QString &source = PdbSource);
} // namespace mf
