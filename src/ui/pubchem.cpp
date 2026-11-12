// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/pubchem.h"
#include "foundation/i18n.h"
#include "ui/pdb_download.h"
namespace mf {
PubchemRecord downloadPubchem(QWidget *parent, const PubchemQuery &query, const QString &folder,
                              const QString &source) {
    PubchemDownload download(query, folder, source);
    const auto shown = query.kind == "cid" ? "CID " + query.value : query.value;
    runDownload(parent, download, tr("PubChemから取得"),
                tr("%1 の構造をPubChemから取得しています…").arg(shown));
    return download.record();
}
} // namespace mf
