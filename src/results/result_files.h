// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QStringList>
#include <QVector>
namespace mf {
// Physical files only; job formats belong to their engine adapters.
QStringList resultFiles(const QString &directory);
struct ResultFileGroup {
    QStringList files; // Names relative to the directory, in step order for an animation.
    QString animationName;
    int mode = -1; // -1 for an ordinary file.
};
QVector<ResultFileGroup> resultFileGroups(const QStringList &files);
// The same file selection and cancellable reader for GUI and headless sessions.
QString preferredResultFile(const QString &directory, const QStringList &files);
QString resultSourceStamp(const QString &path);
} // namespace mf
