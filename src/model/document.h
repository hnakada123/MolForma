#pragma once
#include <Eigen/Core>
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
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
    QJsonObject data;     // Project metadata, stable-ID references, residues, parts and settings.
    QJsonObject envelope; // Preserves execution/result fields on project round trips.
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
    void remove(const Ids &ids);
    QStringList validate() const;
    QString formula() const;
    int formalCharge() const;
    QJsonObject toJson() const;
    static Document fromJson(const QJsonObject &o);
    QJsonObject project() const;
    static Document fromProject(const QJsonObject &o);
    void save(const QString &path) const;
    static Document load(const QString &path);
    QString xyz() const;
    static Document fromXyz(const QString &text);
    void inferBonds();
    void invalidateAssignments();
    QJsonObject job() const;
    QJsonObject emittedJob() const;
    void setJobValue(const QString &key, const QJsonValue &value);
};
QByteArray readFile(const QString &path);
void writeFile(const QString &path, const QByteArray &bytes);
QJsonObject parseJson(const QByteArray &bytes);
} // namespace mf
