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
class PdbDownload : public QObject {
    Q_OBJECT
  public:
    // The entry's file goes to folder as <ID>.cif.gz, which it replaces only once complete. source has %1 for
    // the ID.
    PdbDownload(const QString &id, const QString &folder, const QString &source = PdbSource,
                QObject *parent = nullptr);
    ~PdbDownload() override;
    void start();
    void cancel();
    bool cancelled() const {
        return stopped;
    }
    QString path() const {
        return target;
    }
  signals:
    void progress(qint64 received, qint64 total);
    void finished(const QString &error); // Empty when the file is there, or when cancelled.

  private:
    QString id, target, failure;
    QUrl url;
    QNetworkAccessManager *network;
    QPointer<QNetworkReply> reply;
    QSaveFile *file = nullptr;
    qint64 received = 0;
    bool stopped = false;
    void finish();
};
// Downloads with a dialog that shows the progress and can cancel it. Returns the file, or an empty string
// when cancelled; fails with the reason.
QString downloadPdb(QWidget *parent, const QString &id, const QString &folder,
                    const QString &source = PdbSource);
} // namespace mf
