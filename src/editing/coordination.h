// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/operations.h"
namespace mf {
QStringList geometryNames();
QStringList geometryLabels();
QVector<Vec> siteVectors(const QString &geometry);
QString addCenter(Document &d, const QString &element, const QString &geometry, const Vec &position,
                  double distance, int oxidation);
Center center(const Document &d, const QString &atom);
void updateCenter(Document &d, const Center &c); // Replaces the center of its atom, or adds it.
// The oxidation state is structure preparation's: the total charge follows it (followingCharge).
void setOxidation(Document &d, const QString &centerId, int oxidation);
// The sites a ligand takes: one per donor ("connections") and one per eta group ("hapto").
int ligandSites(const QJsonObject &ligand);
int ligandSites(const Part &part);
// Places a ligand on sites of a center. A ligand whose template gives the place of its metal ("center"), with
// eta groups or three donors or more, is turned so that its donors and eta groups point along the sites
// (matched in the order that fits best, each within 25°), at its own distances; the others go by their
// donors onto the sites (one or two, a bidentate one on neighboring sites). Every donor and eta atom gets a
// dative bond to the center.
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
