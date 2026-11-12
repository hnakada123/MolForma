// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"

namespace mf {
// A reaction path of a result: the points of a coordinate scan, the images of an NEB or the points of an
// IRC, read once from what the log reader keeps in ResultData::properties ("scan", "neb", "irc"). Views
// and calculations that go on from a path take its order, energies and structures from here.
enum class ResultPathKind { None, Scan, Neb, Irc };
// Whether a query counts the two ends of a path. The ends of an NEB are fixed structures, never its
// highest point; a scan or an IRC may be highest at an end.
enum class PathEnds { Included, Excluded };
struct ResultPathPoint {
    // A scan point or an image between the ends of an NEB (Point), the two ends of an NEB, a point of an
    // IRC by its direction, or the transition state an IRC starts from.
    enum class Role { Point, Start, End, Forward, Backward, TransitionState };
    Role role = Role::Point;
    // The scan point, the NEB image or the IRC point of its direction, as the log numbers them; the role
    // tells the transition state, which has no number.
    int number = 0;
    int frame = -1;          // Its structure in ResultData::frames; -1 without one.
    double energy = qQNaN(); // Hartree.
    // The place on the path: the scanned value (Å, °); the length of the NEB path up to the image (Å), or
    // the image's number when the log holds no structures; the IRC coordinate s (amu½·Bohr), negative
    // backward and 0 at the transition state.
    double x = 0;
    double y = qQNaN();              // The second value of a two-dimensional scan.
    QString coordinate, coordinate2; // The scanned coordinates as the log names them ("bond 1 2").
};
struct ResultPath {
    ResultPathKind kind = ResultPathKind::None;
    // In the order of the path: a scan by its points, an NEB from start to end, an IRC from its backward
    // end through the transition state to its forward end.
    QVector<ResultPathPoint> points;
    bool plane = false;   // A two-dimensional scan: the points have y and coordinate2.
    bool lengths = false; // An NEB whose x is the length of the path.
    // The energy differences are taken from (Hartree): the lowest point of a scan, the start of an NEB,
    // the transition state of an IRC (its first point when the log has none). NaN without points.
    double reference = qQNaN();
    // The energy of a point above the reference (Hartree).
    double relative(int point) const {
        return points[point].energy - reference;
    }
    // The index of the point with the highest energy, the first of equal ones; -1 when there is none.
    // Points without an energy never count, nor with `structures` those without a structure.
    int highest(PathEnds ends, bool structures = false) const;
};
// The path of one kind in a result; kind None, without points, when the result has no such path.
ResultPath resultPath(const ResultData &data, ResultPathKind kind);
// The path the result viewer shows: a scan, else an NEB, else an IRC.
ResultPath resultPath(const ResultData &data);
} // namespace mf
