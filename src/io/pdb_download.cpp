// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/pdb_download.h"
#include "model/i18n.h"
#include <QDir>
#include <QRegularExpression>
namespace mf {
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
    : FileDownload(QUrl(QString(source).arg(id)), QDir(folder).filePath(id + ".cif.gz"), id,
                   tr("RCSB PDBに %1 のmmCIFファイルがありません。PDB IDを確かめてください。").arg(id),
                   parent) {}
} // namespace mf
