// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <optional>
class QWidget;
namespace mf {
// Compounds of PubChem by name or CID, as SDF files of its PUG REST service. Only the name or the CID goes
// out, to this address alone. %1: "cid" or "name", %2: the CID or the name (percent-encoded), %3: "3d" or
// "2d".
constexpr auto PubchemSource = "https://pubchem.ncbi.nlm.nih.gov/rest/pug/compound/%1/%2/SDF?record_type=%3";
struct PubchemQuery {
    QString kind;  // "cid" or "name".
    QString value; // The CID's digits, or the name.
};
// The query of a text: a CID ("2244", "CID 2244", "cid:2244") or a name (up to 300 characters, one line);
// nothing for an empty text.
std::optional<PubchemQuery> pubchemQuery(const QString &text);
struct PubchemRecord {
    QString path;        // Empty when cancelled.
    bool threeD = false; // The 3D conformer; otherwise the 2D structure (no conformer for this compound).
};
// Downloads the compound's 3D conformer into folder, or its 2D structure where PubChem has no conformer
// (large or flexible molecules, metal complexes, mixtures), with a dialog that can cancel it. Fails when
// PubChem has no such compound.
PubchemRecord downloadPubchem(QWidget *parent, const PubchemQuery &query, const QString &folder,
                              const QString &source = PubchemSource);
} // namespace mf
