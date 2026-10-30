// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QString>

namespace mf {
// Structure files may be gzip-compressed, as the PDB hands them out (10mu.cif.gz, pdb10mu.ent.gz).
bool compressedFile(const QString &path);
// The suffix that names the format, without .gz: "cif" for 10mu.cif.gz.
QString structureSuffix(const QString &path);
// The contents, decompressed when the name ends with .gz. Decompressed contents larger than limit (below
// 4 GiB) are refused, so that a small file cannot fill the memory.
QByteArray readStructureFile(const QString &path, qint64 limit = qint64(512) << 20);
} // namespace mf
