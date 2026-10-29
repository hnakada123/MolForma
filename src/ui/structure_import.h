// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <optional>
class QWidget;
namespace mf {
// Shared choices for opening, replacing and appending a structure. Cancellation is not an error;
// callers retain their document and check its freshness after these modal dialogs return.
struct StructureImportOptions {
    int model = 1;
    QString altloc;
    int frame = -1;
};
std::optional<StructureImportOptions> chooseStructureImport(QWidget *parent, const QString &path);
} // namespace mf
