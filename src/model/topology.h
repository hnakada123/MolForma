// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <span>

namespace mf {
// The atoms of a document by index: each atom's index by ID and its bonded neighbors. Document's own
// lookups scan every atom or bond, so loops over many atoms build one of these once instead. It
// describes the atoms and bonds as they were when it was built; bonds to unknown atoms are left out.
class Topology {
  public:
    explicit Topology(const Document &d);
    int size() const {
        return int(offsets.size()) - 1;
    }
    int index(const QString &id) const {
        return indices.value(id, -1);
    }
    // The neighbors of atom i in the order of the bonds, and those bonds (indices into Document::bonds).
    std::span<const int> neighbors(int i) const {
        return {adjacent.constData() + offsets[i], size_t(offsets[i + 1] - offsets[i])};
    }
    std::span<const int> bonds(int i) const {
        return {bondIndices.constData() + offsets[i], size_t(offsets[i + 1] - offsets[i])};
    }
    bool bonded(int i, int j) const;
    // Document's lookups by ID (atom, neighbors and bond), answered from the index.
    Atom &atom(Document &d, const QString &id) const;
    const Atom &atom(const Document &d, const QString &id) const;
    Ids neighbors(const Document &d, const QString &id, bool includeDative = true) const;
    const Bond *bond(const Document &d, const QString &a, const QString &b) const;
    // The atoms connected to atom i in breadth-first order (as Document::component), without
    // crossing the bond between cutA and cutB.
    QVector<int> component(int i, int cutA = -1, int cutB = -1) const;
    // The connected molecule of each atom, numbered 0, 1, … in the order of their first atoms.
    QVector<int> molecules() const;

  private:
    QHash<QString, int> indices;
    QVector<int> offsets, adjacent, bondIndices;
};
} // namespace mf
