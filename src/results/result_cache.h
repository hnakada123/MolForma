// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/result_fields.h"
#include <optional>
class QDataStream;
namespace mf::result_detail {
// The readings a result loader keeps for its session, in a folder it owns: a CUBE or MOLDEN file read
// once is not parsed again for another dataset, orbital or isovalue, and an orbital evaluated on its grid
// is not evaluated again for another isovalue. Every key holds the file's stamp (paths, size and times),
// so a changed file is read anew. Entries are written with QSaveFile (a worker cancelled while writing
// leaves none); unreadable ones count as missing, and the oldest go beyond 512 MiB.
class ResultCache {
  public:
    ResultCache(const QString &folder, const QString &path);
    explicit operator bool() const;
    std::optional<std::pair<ResultData, MoldenData>> molden() const;
    void store(const ResultData &base, const MoldenData &data) const;
    std::optional<std::pair<ResultData, GridValues>> cube(int dataset) const;
    void store(int dataset, const ResultData &base, const GridValues &grid) const;
    std::optional<std::pair<GridValues, QString>> orbital(const QString &key) const;
    void store(const QString &key, const GridValues &grid, const QString &normalization) const;

  private:
    QString folder, stamp;
    QString file(const QString &what) const;
    static void out(QDataStream &stream, const GridValues &grid);
    static void in(QDataStream &stream, GridValues &grid);
    template <typename T, typename Read> std::optional<T> read(const QString &what, Read &&entry) const;
    template <typename Write> void write(const QString &what, Write &&entry) const;
};
} // namespace mf::result_detail
