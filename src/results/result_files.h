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
// The same file selection and cancellable reader for GUI and headless sessions: the file named
// `preferred` when the folder has it (the log of a job; its name is the caller's to know), else the first.
QString preferredResultFile(const QString &directory, const QStringList &files, const QString &preferred);
QString resultSourceStamp(const QString &path);
} // namespace mf
