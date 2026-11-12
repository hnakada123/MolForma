// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/pdb_download.h"
#include "model/document.h"
#include "model/i18n.h"
#include <QEventLoop>
#include <QProgressDialog>
namespace mf {
QString downloadPdb(QWidget *parent, const QString &id, const QString &folder, const QString &source) {
    PdbDownload download(id, folder, source);
    return runDownload(parent, download, tr("PDBからダウンロード"),
                       tr("%1 をRCSB PDBからダウンロードしています…").arg(id));
}
QString runDownload(QWidget *parent, Download &download, const QString &title, const QString &label) {
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
    QObject::connect(&download, &Download::progress, &dialog, [&](qint64 received, qint64 total) {
        if (total > 0) { // In KiB, which a progress bar's int holds.
            dialog.setMaximum(int(total >> 10));
            dialog.setValue(int(std::min(received, total) >> 10));
        }
    });
    QObject::connect(&download, &Download::finished, &loop, [&](const QString &message) {
        error = message;
        done = true;
        loop.quit();
    });
    QObject::connect(&dialog, &QProgressDialog::canceled, &download, &Download::cancel);
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
