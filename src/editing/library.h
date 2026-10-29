// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
namespace mf {
const QJsonArray &templates();
QJsonObject findTemplate(const QString &id);
QString templateName(const QJsonObject &t); // name_ja or name_en, in the interface language.
struct PlacedPart {
    QString id;
    Ids atoms, connections;
    QMap<int, QString> mapping;
};
PlacedPart instantiate(Document &d, const QJsonObject &t, const QVector<Vec> &positions = {},
                       const QSet<int> &omit = {});
PlacedPart placeTemplate(Document &d, const QJsonObject &t, const Vec &position);
// category: "substituent" (one external bond at the first atom), "ligand" (none or one) or
// "skeleton" (a stand-alone molecule without connection points, placed by clicking).
QJsonObject makePart(const Document &d, const Ids &ids, const QString &name, const QString &category);
void validateTemplate(const QJsonObject &t);
} // namespace mf
