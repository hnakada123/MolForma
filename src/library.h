#pragma once
#include "document.h"
namespace mf {
const QJsonArray &templates();
QJsonObject findTemplate(const QString &id);
struct PlacedPart {
    QString id;
    Ids atoms, connections;
    QMap<int, QString> mapping;
};
PlacedPart instantiate(Document &d, const QJsonObject &t, const QVector<Vec> &positions = {},
                       const QSet<int> &omit = {});
PlacedPart placeTemplate(Document &d, const QJsonObject &t, const Vec &position);
QJsonObject makePart(const Document &d, const Ids &ids, const QString &name, bool ligand);
void validateTemplate(const QJsonObject &t);
} // namespace mf
