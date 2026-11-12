// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/files.h"
#include "foundation/error.h"
#include "foundation/i18n.h"
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
namespace mf {
QByteArray readFile(const QString &p) {
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    return f.readAll();
}
void writeFile(const QString &p, const QByteArray &bytes) {
    QSaveFile f(p);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        fail(f.errorString());
}
QJsonObject parseJson(const QByteArray &b) {
    QJsonParseError error;
    auto d = QJsonDocument::fromJson(b, &error);
    if (error.error != QJsonParseError::NoError || !d.isObject())
        fail(tr("JSONを読めません: %1").arg(error.errorString()));
    return d.object();
}
} // namespace mf
