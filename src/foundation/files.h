// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
namespace mf {
QByteArray readFile(const QString &path);
// Atomic replacement through QSaveFile; a failed write leaves the preceding file intact.
void writeFile(const QString &path, const QByteArray &bytes);
QJsonObject parseJson(const QByteArray &bytes);
} // namespace mf
