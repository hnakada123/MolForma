// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "io/pdb_download.h"
class QWidget;
namespace mf {
// Presentation only: show progress and cancellation for an existing transfer.
QString runDownload(QWidget *parent, Download &download, const QString &title, const QString &label);
QString downloadPdb(QWidget *parent, const QString &id, const QString &folder,
                    const QString &source = PdbSource);
} // namespace mf
