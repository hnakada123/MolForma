# SPDX-License-Identifier: GPL-2.0-or-later
"""Adds the polydentate and hapto ligands and the solvents of the builder to assets/library.json.

A development tool (RDKit), not used by MolForma at run time. Each ligand is embedded with its donor atoms
(or the atoms of an eta group) fixed where a metal at the origin holds them: on MolForma's coordination
sites at 2.05 A (siteVectors in src/editing/coordination.cpp; terpy at its own bite angles), or around the
axis of a site for an eta group.
UFF then relaxes the rest, the fixed atoms kept and every other atom at least 2.5 A (H) or 2.75 A from the
metal, so that the ligand has the shape of a bound one. The metal is not part of the template: its position
goes into "center", the donors into "connections" and the eta groups into "hapto".
"""
import json
import math
import sys
from pathlib import Path

import numpy as np
import rdkit
from rdkit import Chem
from rdkit.Chem import AllChem
from rdkit.Geometry import Point3D

REPO = Path(__file__).resolve().parents[2]
LIBRARY = REPO / "assets" / "library.json"
SEED = 20261111
S3 = math.sqrt(3) / 2
OCTAHEDRAL = [(1, 0, 0), (0, 1, 0), (-1, 0, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
SQUARE = OCTAHEDRAL[:4]
BIPYRAMID = [(1, 0, 0), (-0.5, S3, 0), (-0.5, -S3, 0), (0, 0, 1), (0, 0, -1)]


def ring(n, radius, height):
    return [(radius * math.cos(2 * math.pi * k / n), radius * math.sin(2 * math.pi * k / n), height)
            for k in range(n)]


# id, name_ja, name_en, SMILES, donors (atom index -> site direction) or eta groups (atom indices ->
# positions with the metal at the origin).
KAPPA = [
    ("ligand:terpy", "2,2′:6′,2″-テルピリジン terpy（mer三座）", "2,2′:6′,2″-Terpyridine terpy (mer tridentate)",
     "n1ccccc1-c1cccc(n1)-c1ccccn1", {0: (2.02 * math.cos(math.radians(10)), 2.02 * math.sin(math.radians(10)), 0),
                                      11: (0, 1.95, 0),
                                      17: (-2.02 * math.cos(math.radians(10)), 2.02 * math.sin(math.radians(10)), 0)}),
    ("ligand:dien", "ジエチレントリアミン dien（fac三座）", "Diethylenetriamine dien (fac tridentate)",
     "NCCNCCN", {0: OCTAHEDRAL[0], 3: OCTAHEDRAL[1], 6: OCTAHEDRAL[4]}),
    ("ligand:tacn", "1,4,7-トリアザシクロノナン tacn（fac三座）", "1,4,7-Triazacyclononane tacn (fac tridentate)",
     "C1CNCCNCCN1", {2: OCTAHEDRAL[0], 5: OCTAHEDRAL[1], 8: OCTAHEDRAL[4]}),
    ("ligand:tp", "ヒドロトリス(ピラゾリル)ボラト Tp−（fac三座）", "Hydrotris(pyrazolyl)borate Tp− (fac tridentate)",
     "[BH-](n1cccn1)(n1cccn1)n1cccn1", {5: OCTAHEDRAL[0], 10: OCTAHEDRAL[1], 15: OCTAHEDRAL[4]}),
    ("ligand:tren", "トリス(2-アミノエチル)アミン tren（四座、三方両錐）",
     "Tris(2-aminoethyl)amine tren (tetradentate, trigonal bipyramid)",
     "NCCN(CCN)CCN", {3: BIPYRAMID[3], 0: BIPYRAMID[0], 6: BIPYRAMID[1], 9: BIPYRAMID[2]}),
    ("ligand:cyclam", "シクラム cyclam（四座、平面）", "Cyclam (tetradentate, planar)",
     "C1CNCCNCCCNCCNC1", {2: SQUARE[0], 5: SQUARE[1], 9: SQUARE[2], 12: SQUARE[3]}),
    ("ligand:porphine", "ポルフィナト P2−（四座、平面）", "Porphinate P2− (tetradentate, planar)",
     "C1=CC2=CC3=CC=C([N-]3)C=C4C=CC(=N4)C=C5C=CC(=CC1=N2)[N-]5", None),
    ("ligand:salen", "サレン salen2−（N2O2四座、平面）", "Salen salen2− (N2O2 tetradentate, planar)",
     "[O-]c1ccccc1C=NCCN=Cc1ccccc1[O-]", None),
    ("ligand:edta", "エチレンジアミン四酢酸 EDTA4−（六座）", "Ethylenediaminetetraacetate EDTA4− (hexadentate)",
     "[O-]C(=O)CN(CC([O-])=O)CCN(CC([O-])=O)CC([O-])=O", None),
]
ETA = [
    ("ligand:cp", "シクロペンタジエニド Cp−（η5）", "Cyclopentadienide Cp− (η5)", "[cH-]1cccc1",
     [ring(5, 1.208, 1.66)]),
    ("ligand:cpstar", "ペンタメチルシクロペンタジエニド Cp*−（η5）", "Pentamethylcyclopentadienide Cp*− (η5)",
     "C[c-]1c(C)c(C)c(C)c1C", [ring(5, 1.208, 1.66)]),
    ("ligand:benzene", "ベンゼン（η6）", "Benzene (η6)", "c1ccccc1", [ring(6, 1.40, 1.62)]),
    ("ligand:ethylene", "エチレン（η2）", "Ethylene (η2)", "C=C", [[(-0.70, 0, 1.93), (0.70, 0, 1.93)]]),
    ("ligand:allyl", "アリル C3H5−（η3）", "Allyl C3H5− (η3)", "C=C[CH2-]",
     [[(-1.21, -0.233, 1.70), (0, 0.467, 1.70), (1.21, -0.233, 1.70)]]),
    ("ligand:butadiene", "1,3-ブタジエン（η4）", "1,3-Butadiene (η4)", "C=CC=C",
     [[(-1.42, -0.615, 1.69), (-0.71, 0.615, 1.69), (0.71, 0.615, 1.69), (1.42, -0.615, 1.69)]]),
]
SOLVENTS = [
    ("skeleton:methanol", "メタノール", "Methanol", "CO"),
    ("skeleton:dmso", "ジメチルスルホキシド DMSO", "Dimethyl sulfoxide DMSO", "CS(C)=O"),
]


def donors_of(identifier, mol):
    """Donor atoms of the ligands whose indices are found by pattern."""
    if identifier == "ligand:porphine":
        nitrogens = [a.GetIdx() for a in mol.GetAtoms() if a.GetSymbol() == "N"]
        return order_around(mol, nitrogens, SQUARE)
    if identifier == "ligand:salen":
        o1, n1, n2, o2 = ([a.GetIdx() for a in mol.GetAtoms() if a.GetSymbol() == "O"][0],
                          *[a.GetIdx() for a in mol.GetAtoms() if a.GetSymbol() == "N"],
                          [a.GetIdx() for a in mol.GetAtoms() if a.GetSymbol() == "O"][1])
        return {o1: SQUARE[0], n1: SQUARE[1], n2: SQUARE[2], o2: SQUARE[3]}
    if identifier == "ligand:edta":
        # Each nitrogen with its two arms: one arm in the plane of the nitrogens, one axial.
        n1, n2 = [a.GetIdx() for a in mol.GetAtoms() if a.GetSymbol() == "N"]
        arms = {}
        for n in (n1, n2):
            oxygens = []
            for c in mol.GetAtomWithIdx(n).GetNeighbors():
                for carboxyl in c.GetNeighbors():
                    if carboxyl.GetSymbol() == "C" and carboxyl.GetIdx() != n and any(
                            o.GetSymbol() == "O" for o in carboxyl.GetNeighbors()):
                        oxygens.append(next(o.GetIdx() for o in carboxyl.GetNeighbors()
                                            if o.GetSymbol() == "O" and o.GetFormalCharge() == -1))
            arms[n] = oxygens
        return {n1: OCTAHEDRAL[0], n2: OCTAHEDRAL[1], arms[n1][0]: OCTAHEDRAL[4], arms[n1][1]: OCTAHEDRAL[3],
                arms[n2][0]: OCTAHEDRAL[5], arms[n2][1]: OCTAHEDRAL[2]}
    raise KeyError(identifier)


def order_around(mol, atoms, sites):
    """Atoms of a macrocycle in their order around it, onto consecutive sites."""
    start, ordered = atoms[0], [atoms[0]]
    while len(ordered) < len(atoms):
        last = ordered[-1]
        rest = [a for a in atoms if a not in ordered]
        ordered.append(min(rest, key=lambda a: len(Chem.GetShortestPath(mol, last, a))))
    return {a: s for a, s in zip(ordered, sites)}


def bound(mol, fixed, label):
    """The ligand embedded with `fixed` (index -> position, the metal at the origin) and relaxed by UFF."""
    mol = Chem.AddHs(mol)
    coords = {i: Point3D(*p) for i, p in fixed.items()}
    best = None
    for k in range(30):
        if AllChem.EmbedMolecule(mol, coordMap=coords, randomSeed=SEED + k, useRandomCoords=True) != 0:
            continue
        work = Chem.RWMol(mol)
        center = work.AddAtom(Chem.Atom("He"))
        candidate = work.GetMol()
        Chem.SanitizeMol(candidate)
        conformer = candidate.GetConformer()
        conformer.SetAtomPosition(center, Point3D(0, 0, 0))
        for i, p in coords.items():
            conformer.SetAtomPosition(i, p)
        field = AllChem.UFFGetMoleculeForceField(candidate)
        for i in list(fixed) + [center]:
            field.AddFixedPoint(i)
        for i in range(candidate.GetNumAtoms() - 1):
            if i not in fixed:
                least = 2.5 if candidate.GetAtomWithIdx(i).GetSymbol() == "H" else 2.75
                field.UFFAddDistanceConstraint(i, center, False, least, 100.0, 500.0)
        field.Minimize(maxIts=20000)
        energy = field.CalcEnergy()
        if best is None or energy < best[0]:
            best = (energy, candidate)
    if best is None:
        sys.exit(f"{label}: no embedding")
    candidate = best[1]
    result = Chem.RWMol(candidate)
    result.RemoveAtom(candidate.GetNumAtoms() - 1)
    out = result.GetMol()
    Chem.SanitizeMol(out)
    positions = np.array(out.GetConformer().GetPositions())
    closest = min(np.linalg.norm(positions[i]) for i in range(len(positions)) if i not in fixed)
    print(f"{label}: UFF {best[0]:.1f}, nearest other atom to the metal {closest:.2f} A", file=sys.stderr)
    return out


def template(identifier, name_ja, name_en, smiles, mol, category, connections, hapto, center, generation):
    Chem.Kekulize(mol, clearAromaticFlags=True)
    positions = np.array(mol.GetConformer().GetPositions())
    middle = positions.mean(axis=0)
    kinds = {Chem.BondType.SINGLE: "single", Chem.BondType.DOUBLE: "double", Chem.BondType.TRIPLE: "triple"}
    atoms = [{"element": a.GetSymbol(), "position": [round(float(x), 6) for x in positions[a.GetIdx()] - middle],
              "formal_charge": a.GetFormalCharge(), "radical": a.GetNumRadicalElectrons(), "name": "", "stereo": "",
              "chiral_tag": 0, "map_number": 0} for a in mol.GetAtoms()]
    bonds = [{"a": b.GetBeginAtomIdx(), "b": b.GetEndAtomIdx(), "kind": kinds[b.GetBondType()]}
             for b in mol.GetBonds()]
    t = {"id": identifier, "name_ja": name_ja, "name_en": name_en, "aliases": [identifier, name_en, smiles],
         "category": category, "smiles": smiles, "atoms": atoms, "bonds": bonds, "connections": connections,
         "leaving": None, "state": None, "formula": Chem.rdMolDescriptors.CalcMolFormula(mol),
         "formal_charge": Chem.GetFormalCharge(mol),
         "provenance": {"generator": "tools/templates/coordination.py", "tool": "RDKit", "version": rdkit.__version__,
                        "seed": SEED, **generation,
                        "source_license": "MolForma authored SMILES/geometry: GPL-2.0-or-later; RDKit: BSD-3-Clause"},
         "validation": {"sanitized": True, "finite_coordinates": bool(np.isfinite(positions).all()), "cip": {}}}
    if hapto:
        t["hapto"] = hapto
    if center is not None:
        t["center"] = [round(float(x), 6) for x in np.array(center) - middle]
    return t


def main():
    library = json.loads(LIBRARY.read_text(encoding="utf-8"))
    made = []
    bound_generation = {"embedding": "ETKDGv3 with the donor (or eta) atoms fixed for a metal at the origin",
                        "force_field": "UFF, those atoms fixed, the others at least 2.5 A (H) / 2.75 A from the metal"}
    for identifier, name_ja, name_en, smiles, donors in KAPPA:
        mol = Chem.MolFromSmiles(smiles)
        donors = donors or donors_of(identifier, mol)
        for i in donors:
            assert mol.GetAtomWithIdx(i).GetSymbol() in ("N", "O"), (identifier, i)
        # A site direction at 2.05 A, or a position where a rigid ligand holds its donor (terpy's bite).
        fixed = {i: tuple(np.array(d, float) if np.linalg.norm(d) > 1.5 else 2.05 * np.array(d) / np.linalg.norm(d))
                 for i, d in donors.items()}
        made.append(template(identifier, name_ja, name_en, smiles, bound(mol, fixed, identifier), "ligand",
                             list(donors), None, (0, 0, 0), bound_generation))
    for identifier, name_ja, name_en, smiles, groups in ETA:
        mol = Chem.MolFromSmiles(smiles)
        if identifier == "ligand:cpstar":
            members = [list(r) for r in mol.GetRingInfo().AtomRings()]
        elif identifier in ("ligand:cp", "ligand:benzene"):
            members = [list(mol.GetRingInfo().AtomRings()[0])]
        else:
            members = [[a.GetIdx() for a in mol.GetAtoms() if a.GetSymbol() == "C"]]
        fixed = {}
        for group, points in zip(members, groups):
            assert len(group) == len(points), identifier
            fixed.update({i: p for i, p in zip(group, points)})
        made.append(template(identifier, name_ja, name_en, smiles, bound(mol, fixed, identifier), "ligand", [],
                             members, (0, 0, 0), bound_generation))
    for identifier, name_ja, name_en, smiles in SOLVENTS:
        mol = Chem.AddHs(Chem.MolFromSmiles(smiles))
        AllChem.EmbedMolecule(mol, randomSeed=SEED)
        AllChem.MMFFOptimizeMolecule(mol, maxIters=5000)
        made.append(template(identifier, name_ja, name_en, smiles, mol, "skeleton", [], None, None,
                             {"embedding": "ETKDGv3", "force_field": "MMFF94"}))
    ids = {t["id"] for t in made}
    library["templates"] = [t for t in library["templates"] if t["id"] not in ids] + made
    LIBRARY.write_text(json.dumps(library, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{len(made)} templates written to {LIBRARY.relative_to(REPO)}", file=sys.stderr)


if __name__ == "__main__":
    main()
