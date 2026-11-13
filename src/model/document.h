// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "foundation/error.h"
#include "model/elements.h"
#include <Eigen/Core>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <stdexcept>

namespace mf {
using Vec = Eigen::Vector3d;
using Mat = Eigen::Matrix3d;
using Ids = QStringList;
QString newId();
Vec vectorFromJson(const QJsonValue &value);
QJsonArray vectorToJson(const Vec &v);
Ids idsFromJson(const QJsonValue &value);
QJsonArray idsToJson(const Ids &ids);

struct Atom {
    QString id = newId(), element = "C", name, stereo;
    Vec position = Vec::Zero();
    int charge = 0, radical = 0, isotope = 0;
    double mass = 0;
    bool ghost = false, fixed = false, automaticH = false;
    QJsonObject extra;
    QJsonObject toJson() const;
    static Atom fromJson(const QJsonObject &o);
    bool operator==(const Atom &) const = default;
};
struct Bond {
    QString a, b, kind = "single", stereo, source = "explicit";
    double order() const;
    QJsonObject toJson() const;
    bool operator==(const Bond &) const = default;
};
// A residue of a peptide chain. Its atoms are named as in PDB files ("CA").
struct Residue {
    QString id = newId(), code, chain;
    QMap<QString, QString> atoms; // Atom IDs by atom name.
    int number = 0;
    QString insertionCode, chirality = "L", state = "default", nTerminal = "neutral", cTerminal = "neutral";
    bool matched = true, modified = false;
    QJsonObject metadata, extra; // extra: fields MolForma does not know, kept on round trips.
    QJsonObject toJson() const;
    static Residue fromJson(const QJsonObject &o);
    bool operator==(const Residue &) const = default;
};
// The atoms placed from a template (skeleton, substituent, ligand, amino acid, user part).
struct Part {
    QString id = newId(), templateId;
    Ids atoms, connections;
    // The template's names and category, a user part's template_data, and for a ligand at a
    // coordination center its atom (coordination_center), sites, twist and eta groups (hapto).
    QJsonObject metadata, extra;
    QString center() const {
        return metadata["coordination_center"].toString();
    }
    // The eta groups of a ligand ("hapto" of the metadata): the atoms of each group, which bind to the
    // center together on one site. They are atom references like `connections`: Document::remove and
    // renameIds keep them.
    QVector<Ids> hapto() const;
    void setHapto(const QVector<Ids> &groups);
    QJsonObject toJson() const;
    static Part fromJson(const QJsonObject &o);
    bool operator==(const Part &) const = default;
};
struct CoordinationSite {
    Vec direction = Vec::UnitZ();
    double distance = 0;
    QString donor, part; // The donor atom and its part while a ligand occupies the site.
    QJsonObject extra;
    bool operator==(const CoordinationSite &) const = default;
};
struct Center {
    QString atom, geometry;
    int oxidation = 0;
    QVector<CoordinationSite> sites;
    QJsonObject extra;
    QJsonObject toJson() const;
    static Center fromJson(const QJsonObject &o);
    bool operator==(const Center &) const = default;
};
// An external point charge (Å, e).
struct PointCharge {
    QString id = newId();
    Vec position = Vec::Zero();
    double charge = 0;
    QString source = "manual", sourceUnit = "angstrom"; // Entered by hand ("manual") or from water ("TIP3P").
    QJsonObject extra;
    QJsonObject toJson() const;
    static PointCharge fromJson(const QJsonObject &o);
    bool operator==(const PointCharge &) const = default;
};
struct Document {
    QVector<Atom> atoms;
    QVector<Bond> bonds;
    QVector<Residue> residues;
    QMap<QString, Ids> chains; // The residue IDs of each chain, from the N to the C terminus.
    QVector<Part> parts;
    QVector<Center> centers;
    QVector<PointCharge> pointCharges;
    int charge = 0, multiplicity = 1; // Total charge and spin multiplicity of the structure.
    // The project's ID, units and metadata, the calculation and execution settings ("nwchem",
    // "nwchem_execution") and fields MolForma does not know.
    QJsonObject data;
    QJsonObject envelope; // Preserves unknown top-level fields on project round trips.
    Document();
    bool operator==(const Document &) const = default;
    Atom &atom(const QString &id);
    const Atom &atom(const QString &id) const;
    bool contains(const QString &id) const;
    int index(const QString &id) const;
    // The indices of these atoms in the same order, found in one pass; fails for an unknown atom.
    QVector<int> indices(const Ids &ids) const;
    Ids ids() const;
    Ids neighbors(const QString &id, bool includeDative = true) const;
    Ids component(const QString &id, const QString &cutA = {}, const QString &cutB = {}) const;
    Ids bondSide(const QString &fixed, const QString &moving) const;
    const Bond *bond(const QString &a, const QString &b) const;
    void setBond(const QString &a, const QString &b, const QString &kind);
    const Residue *findResidue(const QString &id) const;
    const Center *findCenter(const QString &atom) const;
    // Removes atoms and every reference to them: bonds, residues, parts, chains, coordination
    // centers and their sites, and the atoms of NWChem constraints and the scan coordinate.
    void remove(const Ids &ids);
    // Replaces atom, residue, part and chain IDs in every reference to them (renamed: old → new).
    void renameIds(const QHash<QString, QString> &renamed);
    QStringList validate() const;
    QString formula() const;
    int formalCharge() const;
    // The total charge the structure suggests: the formal charges, with the oxidation numbers of the
    // coordination centers in place of the centers' own formal charges.
    int expectedCharge() const;
    bool hasCenters() const;
    QJsonObject toJson() const;
    static Document fromJson(const QJsonObject &o);
    QJsonObject project() const;
    static Document fromProject(const QJsonObject &o);
    // The XYZ dialect (xyz.cpp): reading infers the bonds.
    QString xyz() const;
    static Document fromXyz(const QString &text);
    // Single bonds by distance and covalent radii (bond_inference.cpp): between the atoms that
    // bondDistance holds for, at most 16 to an atom, none to ghosts.
    void inferBonds();
};
// Whether two atoms this far apart (Å) are bonded, where positions are all there is to go by (XYZ files,
// crystals, the structures of a log): closer than 1.22 times the sum of their covalent radii (bondReach),
// and not in one place.
inline double bondReach(double radiusA, double radiusB) {
    return 1.22 * (radiusA + radiusB);
}
inline bool bondDistance(double distance, double radiusA, double radiusB) {
    return distance > .25 && distance < bondReach(radiusA, radiusB);
}
} // namespace mf
