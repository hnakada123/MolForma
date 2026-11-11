// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/pubchem.h"
#include "model/document.h"
#include "model/i18n.h"
#include "ui/pdb_download.h"
#include <QCryptographicHash>
#include <QDir>
#include <QRegularExpression>
#include <QUrl>

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
PubchemRecord downloadPubchem(QWidget *parent, const PubchemQuery &query, const QString &folder,
                              const QString &source) {
    // The file of a name keeps what it can of it, and a digest that tells names apart.
    QString stem = query.kind == "cid" ? "CID_" + query.value : query.value;
    if (query.kind == "name") {
        stem.replace(QRegularExpression("[^A-Za-z0-9_.-]+"), "_");
        stem =
            stem.left(40) + '_' +
            QString::fromLatin1(
                QCryptographicHash::hash(query.value.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
    }
    const auto shown = query.kind == "cid" ? "CID " + query.value : query.value;
    const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(query.value));
    PubchemRecord record;
    for (const auto *kind : {"3d", "2d"}) {
        FileDownload download(QUrl(QString(source).arg(query.kind, encoded, kind)),
                              QDir(folder).filePath(stem + '_' + kind + ".sdf"), shown,
                              tr("PubChemに %1 が見つかりません。名前・CIDを確かめてください。").arg(shown));
        try {
            record.path =
                runDownload(parent, download, tr("PubChemから取得"),
                            kind == QString("3d") ? tr("%1 の3D構造をPubChemから取得しています…").arg(shown)
                                                  : tr("%1 の2D構造をPubChemから取得しています…").arg(shown));
        } catch (const std::exception &) {
            // No conformer: the 2D structure, from which MolForma makes the 3D one.
            if (download.notFound() && kind == QString("3d"))
                continue;
            throw;
        }
        record.threeD = kind == QString("3d");
        return record;
    }
    return record;
}
} // namespace mf
