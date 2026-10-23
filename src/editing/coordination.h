#pragma once
#include "editing/operations.h"
namespace mf {
QStringList geometryNames();
QStringList geometryLabels();
QVector<Vec> siteVectors(const QString &geometry);
QString addCenter(Document &d, const QString &element, const QString &geometry, const Vec &position,
                  double distance, int oxidation);
QJsonObject center(const Document &d, const QString &atom);
void updateCenter(Document &d, const QJsonObject &c);
PlacedPart placeLigand(Document &d, const QString &centerId, const QList<int> &sites, const QJsonObject &t,
                       double twist = 0);
void adjustCenter(Document &d, const QString &centerId, const QString &geometry, double distance,
                  int oxidation);
void moveLigand(Document &d, const QString &partId, const QList<int> &sites,
                const QJsonObject &replacement = {}, double twist = 0);
void setSiteDirection(Document &d, const QString &centerId, const QList<int> &sites, const Vec &direction,
                      double distance);
void swapLigands(Document &d, const QString &centerId, int first, int second);
} // namespace mf
