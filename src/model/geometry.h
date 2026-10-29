// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/document.h"
#include <Eigen/Geometry>
#include <array>
namespace mf {
Vec unit(const Vec &v);
Mat rotation(const Vec &axis, double degrees);
Mat alignVectors(const Vec &from, const Vec &to);
std::pair<Mat, Vec> fitPoints(const QVector<Vec> &from, const QVector<Vec> &to);
void transform(Document &d, const Ids &ids, const Mat &r = Mat::Identity(), const Vec &shift = Vec::Zero(),
               const Vec &origin = Vec::Zero());
// A grid over [low, high] with the same spacing on every axis: `divisions` intervals along the
// longest side; the other sides grow about their centers to whole intervals.
struct UniformGrid {
    Vec origin = Vec::Zero();
    double spacing = 0;
    std::array<int, 3> divisions{};
};
UniformGrid uniformGrid(const Vec &low, const Vec &high, int divisions);
// Geometric center (unweighted mean position) of the atoms.
Vec centroid(const Document &d, const Ids &ids);
double measure(const Document &d, const Ids &ids);
void setDimension(Document &d, const Ids &ids, double value);
void orient(Document &d, const Ids &ids);
void centerMass(Document &d, const Ids &ids, bool principal);
// The rotational symmetry number used in thermochemistry: the proper rotations (identity included)
// of the point group of the real atoms at the default tolerance, as "点群・対称化" shows it, with
// isotopes and masses as distinct kinds (HDO 1, D2O 2). 0 if not determined (one atom, more than
// 200 atoms).
int rotationalSymmetryNumber(const Document &d);
Vec internalPoint(const Vec &a, const Vec &b, const Vec &c, double length, double angle, double dihedral);
struct Superposition {
    double raw = 0;       // RMSD of the coordinates as they are (Å).
    double rmsd = 0;      // After the best rotation and translation, all atoms.
    double heavyRmsd = 0; // Same fit, non-hydrogen atoms only.
    double maximum = 0;   // Largest single-atom distance after the fit.
    Mat rotation = Mat::Identity();
    Vec shift = Vec::Zero(); // rotation * moving + shift ≈ reference
};
// Kabsch superposition of `moving` onto `reference`: same number of atoms with the same elements
// in the same order.
Superposition superpose(const Document &moving, const Document &reference);
struct SymmetryOperation {
    Mat matrix;         // About PointGroup::center.
    QVector<int> image; // The operation carries atom i onto atom image[i].
};
struct PointGroup {
    QString name; // Schoenflies symbol such as "C2v", "Td" or "D∞h"; empty if not determined.
    Vec center = Vec::Zero();
    Vec axis = Vec::Zero();                // Principal axis (also the axis of linear molecules).
    QVector<SymmetryOperation> operations; // The whole group, identity first.
    double deviation = 0;                  // Largest distance between an atom's image and its partner (Å).
};
// Point group of all atoms within tolerance (Å); ghosts, isotopes and masses count as distinct
// kinds. Structures of more than 200 atoms are not analyzed (empty name).
PointGroup pointGroup(const Document &d, double tolerance = .05);
// Averages the coordinates over the group, so the structure becomes exactly symmetric. Fixed
// atoms must not move. Returns the largest displacement (Å).
double symmetrize(Document &d, const PointGroup &group);
} // namespace mf
