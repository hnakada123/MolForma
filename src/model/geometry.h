#pragma once
#include "model/document.h"
#include <Eigen/Geometry>
namespace mf {
Vec unit(const Vec &v);
Mat rotation(const Vec &axis, double degrees);
Mat alignVectors(const Vec &from, const Vec &to);
std::pair<Mat, Vec> fitPoints(const QVector<Vec> &from, const QVector<Vec> &to);
void transform(Document &d, const Ids &ids, const Mat &r = Mat::Identity(), const Vec &shift = Vec::Zero(),
               const Vec &origin = Vec::Zero());
double measure(const Document &d, const Ids &ids);
void setDimension(Document &d, const Ids &ids, double value);
void orient(Document &d, const Ids &ids);
void centerMass(Document &d, const Ids &ids, bool principal);
// Number of proper rotations (identity included) that map the real atoms onto themselves within
// 0.05 Å: the rotational symmetry number used in thermochemistry. 0 if not determined (isotopes
// or explicit masses, fewer than 2 or more than 60 atoms).
int rotationalSymmetryNumber(const Document &d);
Vec internalPoint(const Vec &a, const Vec &b, const Vec &c, double length, double angle, double dihedral);
} // namespace mf
