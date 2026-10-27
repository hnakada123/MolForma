# SPDX-License-Identifier: GPL-2.0-or-later
"""Turns the 3D view of MolForma so that atoms land where an earlier screenshot shows them.

The view projects orthographically: screen = center + scale · (R (p − c))_xy with y downwards, where
R = Rz(roll)·Rx(pitch)·Ry(yaw) comes from the rotation sliders. From the window pixels of a few atoms read
off the earlier screenshot, the orientation is fitted, then the zoom (wheel: ×1.0015 per step) and the
pan (right drag) are matched.
"""
import math

import numpy as np


def labels(atoms):
    return {f"{a['element']}{i + 1}": a for i, a in enumerate(atoms)}


def angles_of(R):
    """(yaw, pitch, roll) in degrees of R = Rz(roll)·Rx(pitch)·Ry(yaw), as MoleculeView::angles()."""
    pitch = math.asin(max(-1, min(1, R[2, 1])))
    yaw = math.atan2(-R[2, 0], R[2, 2])
    roll = math.atan2(-R[0, 1], R[1, 1])
    return tuple(math.degrees(v) for v in (yaw, pitch, roll))


def planar_rotation(P, Q):
    """Orthographic pose of points in a plane: the in-plane affine image fixes the tilt up to a mirror."""
    c = P.mean(0)
    _, _, vt = np.linalg.svd(P - c)
    u, v = vt[0], vt[1]
    ab = np.array([[(p - c) @ u, (p - c) @ v] for p in P])
    sol, *_ = np.linalg.lstsq(np.hstack([ab, np.ones((len(ab), 1))]), Q, rcond=None)
    m1, m2 = sol[0], sol[1]  # Images of u and v, times the scale.
    A, B, C = m1 @ m1, m2 @ m2, m1 @ m2
    roots = np.roots([A * B - C * C, -(A + B), 1]) if abs(A * B - C * C) > 1e-12 else [1 / (A + B)]
    k = min(r.real for r in roots if abs(r.imag) < 1e-9 and r.real > 0)  # 1 / scale²
    z1 = math.sqrt(max(0, 1 - k * A))
    z2 = -k * C / z1 if z1 > 1e-9 else math.sqrt(max(0, 1 - k * B))
    out = []
    for sign in (1, -1):
        Ru = np.array([*(m1 * math.sqrt(k)), sign * z1])
        Rv = np.array([*(m2 * math.sqrt(k)), sign * z2])
        R = np.column_stack([Ru, Rv, np.cross(Ru, Rv)]) @ np.vstack([u, v, np.cross(u, v)])
        uu, _, ww = np.linalg.svd(R)
        out.append(uu @ ww)
    return out


def rotation_from(targets, atoms):
    """(yaw, pitch, roll) that put the atoms of targets ({label: (x, y)} in window pixels) in place."""
    by = labels(atoms)
    P = np.array([by[k]["position"] for k in targets])
    Q = np.array([(x, -y) for x, y in targets.values()], float)
    if np.linalg.svd(P - P.mean(0), compute_uv=False)[2] < 0.15:
        return angles_of(planar_rotation(P, Q)[0])
    sol, *_ = np.linalg.lstsq(np.hstack([P, np.ones((len(P), 1))]), Q, rcond=None)
    u, _, vt = np.linalg.svd(sol[:3].T, full_matrices=False)  # Nearest orthonormal rows.
    r0, r1 = u @ vt
    return angles_of(np.array([r0, r1, np.cross(r0, r1)]))


_origins = {}


def origin(g, target):
    """Window position of a widget's top-left corner (the 3D view does not move: cached per window)."""
    key = (id(g), target)
    if key not in _origins:
        x = y = 0
        item = g.item(target)
        while item and item.get("geometry") and item.get("parent"):
            x, y = x + item["geometry"][0], y + item["geometry"][1]
            item = g.item(item["parent"])
        _origins[key] = (x, y)
    return _origins[key]


def screen(g):
    """Projected atom positions of the molecule view, in window pixels, by label."""
    ox, oy = origin(g, "moleculeView")
    return {label: (x + ox, y + oy) for label, (x, y) in g.view_positions().items()}


def residual(g, targets):
    """The largest distance in pixels between an atom and where the earlier screenshot shows it."""
    now = screen(g)
    d = [math.dist(now[k], v) for k, v in targets.items() if k in now]
    return max(d) if d else None


def displayed_atoms(g):
    d = g.tool("get_document", include=["project", "candidate"])
    return (d.get("candidate") or d["project"])["document"]["atoms"]


def match(g, targets, angles=None):
    """Orientation (fitted, or given as (yaw, pitch, roll)), then zoom and pan onto the targets."""
    if angles is None:
        angles = tuple(round(v) for v in rotation_from(targets, displayed_atoms(g)))
    # A slider set to the value it shows does not signal: step off it first, so that the view takes all
    # three angles (after opening a file the view is turned while the sliders still show 0).
    g.set("editorYaw", int(angles[0]) + 1, seconds=0.1)
    for name, value in zip(("editorYaw", "editorPitch", "editorRoll"), angles):
        g.set(name, int(value), seconds=0.2)
    w, h = g.item("moleculeView", limit=1)["geometry"][2:4]
    for _ in range(2):
        now = screen(g)
        keys = [k for k in targets if k in now]
        A = np.array([targets[k] for k in keys], float)
        B = np.array([now[k] for k in keys], float)
        ratio = math.sqrt(((A - A.mean(0)) ** 2).sum() / ((B - B.mean(0)) ** 2).sum())
        delta = int(round(math.log(ratio) / math.log(1.0015)))
        if delta:
            g.wait(g.tool("ui_input", target="moleculeView", action="wheel", x=w / 2, y=h / 2, delta=delta), 0.3)
        now = screen(g)
        dx, dy = A.mean(0) - np.array([now[k] for k in keys], float).mean(0)
        if abs(dx) >= 1 or abs(dy) >= 1:
            g.wait(g.tool("ui_input", target="moleculeView", action="drag", button="right", x=w / 2, y=h / 2,
                          endX=w / 2 + dx, endY=h / 2 + dy), 0.3)
    return angles, residual(g, targets)


def fit(g, targets, tries=((0, 0, 0), (30, 20, 0), (45, 20, 0), (19, 20, 0), (0, 20, 0))):
    """match(), and for a poor fit the orientations the screenshots of the manual used."""
    angles, error = match(g, targets)
    if error > 5:
        for candidate in tries:
            a, e = match(g, targets, angles=candidate)
            if e < error:
                angles, error = a, e
            if error < 5:
                break
        match(g, targets, angles=angles)
    return angles, error
