// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <Eigen/Core>
#include <QColor>
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
[[noreturn]] void fail(const QString &message);
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
};
struct Bond {
    QString a, b, kind = "single", stereo, source = "explicit";
    double order() const;
    QJsonObject toJson() const;
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
};
// The atoms placed from a template (skeleton, substituent, ligand, amino acid, user part).
struct Part {
    QString id = newId(), templateId;
    Ids atoms, connections;
    // The template's names and category, a user part's template_data, and for a ligand at a
    // coordination center its atom (coordination_center), sites and twist.
    QJsonObject metadata, extra;
    QString center() const {
        return metadata["coordination_center"].toString();
    }
    QJsonObject toJson() const;
    static Part fromJson(const QJsonObject &o);
};
struct CoordinationSite {
    Vec direction = Vec::UnitZ();
    double distance = 0;
    QString donor, part; // The donor atom and its part while a ligand occupies the site.
    QJsonObject extra;
};
struct Center {
    QString atom, geometry;
    int oxidation = 0;
    QVector<CoordinationSite> sites;
    QJsonObject extra;
    QJsonObject toJson() const;
    static Center fromJson(const QJsonObject &o);
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
};
struct Element {
    QString symbol;
    int number;
    double mass, radius;
    QColor color;
    QString nameJa, nameEn;
    QString name() const; // In the interface language.
};
const QVector<Element> &elements();
const Element &element(const QString &symbol);

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
    Atom &atom(const QString &id);
    const Atom &atom(const QString &id) const;
    bool contains(const QString &id) const;
    int index(const QString &id) const;
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
    void save(const QString &path) const;
    static Document load(const QString &path);
    QString xyz() const;
    static Document fromXyz(const QString &text);
    void inferBonds();
};
QByteArray readFile(const QString &path);
void writeFile(const QString &path, const QByteArray &bytes);
QJsonObject parseJson(const QByteArray &bytes);
} // namespace mf
