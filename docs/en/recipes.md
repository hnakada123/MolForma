# Recipes by topic (from building a molecule to a calculation)

These recipes take one molecule each from an empty window (or a structure file, or an earlier result) to an NWChem calculation and its results, using MolForma's interface only. Pick the topic closest to the molecule you want to build; along the way you try the features it needs (atoms and bonds, skeletons and substituents, coordination centers and ligands, peptides from a sequence, stereochemistry, point charges, BSSE correction, transition-state searches, reusing results, isotopes, your own parts and more).

The calculations on these pages were run on 2026-10-26 with NWChem 7.3.1 (Linux, 2–8 MPI processes). Another NWChem version or process count may change the last digits of energies or the number of optimization steps.

## Using these recipes

- **By feature**: “[Features and coverage](features.md)” describes each part of the window and what every button does.
- **By topic**: the 16 recipes on this page. Each starts from a new molecule (or a structure file or a calculation result) and goes through the calculation settings, the run and the results.
- **The pages**: “What you build” → steps → calculation settings → run and results → “Troubleshooting”. Names in quotes (“Clean up all”) are as shown in the window.
- **Practice data**: The finished projects (`.kgui.json`) are linked under “Practice data” in the table of each recipe (`docs/recipes/samples/` of the repository). The web manual downloads them from the links; in the app's “Guide and manual”, clicking a link saves a copy where you choose and opens it (a part file `.kpart.json` goes to “My parts”). Use them to start halfway or to compare with your own. They carry no run settings (NWChem executable, job folder and so on), so check “3 Run settings” before running.

### Built structures and calculated ones

A structure right after placing parts, or after “Clean up all”, is a **starting structure** for a calculation. “Clean up all” shapes the molecule with an Open Babel force field (MMFF94, UFF); it is not NWChem's geometry optimization. To use an optimized structure for the next calculation, bring it in with “Apply as candidate” in the “Result viewer” ([recipe 14](recipes/reuse_result.md)).

The total charge and multiplicity of the calculation are set in “Charge and spin” of “2 Calculation”. The formal charges of atoms (the “Attributes” page) prepare the structure, for instance for adding hydrogens, and are kept apart from the total charge. When residues, fragments, coordination centers or ligands are added, a total charge that matched the value the structure suggests (the sum of formal charges, with the oxidation numbers of coordination centers) follows the new value. The multiplicity is never guessed: set it yourself for radicals and open-shell metals.

## Recipes

| No. | Topic | Molecule | Main features | Calculation |
| --- | --- | --- | --- | --- |
| 1 | [Building from atoms and bonds](recipes/atoms_and_bonds.md) | Ethanol (9 atoms), acetaldehyde, dimethyl ether | Placing atoms and replacing hydrogens, deletion, bond types, inserting an atom into a bond, cleanup, measuring distances | B3LYP/6-31G* optimization → frequencies |
| 2 | [Coordination compounds](recipes/cobalt_complex.md) | trans-[Co(en)₂Cl₂]⁺ (27 atoms, charge +1) | Coordination centers, geometry and oxidation number, bidentate ligands, placing ligands one after another, the charge following them | B3LYP/def2-SVP optimization |
| 3 | [Substituted aromatics](recipes/aromatic_substitution.md) | p-Nitroaniline (16 atoms) | Skeleton templates, substituents replacing hydrogens, ortho/meta/para | B3LYP/6-31G* optimization → frequencies |
| 4 | [Heterocycles from SMILES and structure files](recipes/caffeine_smiles.md) | Caffeine (24 atoms) | From SMILES, 2D structure files (.sdf), adding hydrogens, Regenerate 3D | B3LYP/6-31G* optimization |
| 5 | [Chains and conformers](recipes/butane_conformers.md) | Anti and gauche n-butane | Measuring and setting dihedrals, Save as, relaxed scans | B3LYP/6-31G* relaxed scan (13 points) |
| 6 | [Stereochemistry](recipes/stereochemistry.md) | R- and S-lactic acid, E- and Z-2-butene | Record stereo (R/S, E/Z), Invert stereocenter, Switch E/Z | B3LYP/6-31G* optimization → frequencies (E/Z) |
| 7 | [Peptides](recipes/capped_peptide.md) | ACE–Ala–His–Glu–NME (54 atoms, charge −1) | Build from sequence, terminal caps, side-chain states, the sequence bar | B3LYP/6-31G* single point (COSMO, water) |
| 8 | [Mutating residues and editing chains](recipes/residue_mutation.md) | His→Phe mutant (57 atoms), ACE–Ala–Glu–NME (37 atoms) | Replace selected residue, Delete residue (split chain), relinking two ends | B3LYP/6-31G* single point (COSMO, water) |
| 9 | [Complexes of molecules and BSSE](recipes/benzene_water.md) | Benzene–water dimer (15 atoms) | Placing a second molecule, Align principal axes, editing the coordinate table, BSSE (counterpoise) correction | MP2/6-31G* single point (BSSE corrected) |
| 10 | [Ions and radicals](recipes/phenoxyl_radical.md) | Phenol, phenoxyl radical (doublet), phenolate (charge −1) | Deleting a hydrogen, unpaired electrons and formal charges, multiplicity and reference | B3LYP/6-31+G* optimization → frequencies |
| 11 | [Main-group molecules](recipes/main_group.md) | SF₆ (octahedral), PF₅ (trigonal bipyramidal) | Non-metal coordination centers, geometries, fluoride ligands, rotational symmetry numbers | B3LYP/6-31G* optimization → frequencies |
| 12 | [Solvated clusters and point charges](recipes/hydrated_sodium.md) | [Na(H₂O)₄]⁺ and TIP3P charges for an outer water | Duplicate at an offset, water to point charges, writing external point charges | B3LYP/6-31G* optimization and single points |
| 13 | [Transition states and reaction paths](recipes/sn2_transition_state.md) | Transition state of F⁻ + CH₃Cl, [F···CH₃···Cl]⁻ | Placing a lone atom, setting distances and angles, transition-state search, IRC and its direction | M06-2X/aug-cc-pVDZ TS search → frequencies, IRC |
| 14 | [From a result to the next calculation](recipes/reuse_result.md) | Optimized p-nitroaniline | History, Result viewer, Apply as candidate, excited states (TDDFT) | CAM-B3LYP/6-31+G* TDDFT |
| 15 | [Isotopes and atom order](recipes/isotopes.md) | Ethanol-OD | Isotopes and masses, the coordinate table, Sort atoms | B3LYP/6-31G* frequencies |
| 16 | [Your own parts (My parts)](recipes/my_parts.md) | Benzene with a trimethylsilyl group | Saving selected atoms as a part, part files, placing my parts | B3LYP/6-31G* optimization |

## Not in these recipes

MolForma builds finite molecules (and external point charges) and runs NWChem's molecular methods. Not covered:

- FMO (fragment molecular orbitals) and SAPT0 (energy decomposition). NWChem cannot run them, so there is no recipe that splits a protein's active site into fragments. For reading, checking and repairing PDB files, see the “Residues” page of “1 Molecule” in “[Features and coverage](features.md)”.
- Crystals and periodic systems, surfaces, automatic polymers, automatic solvent boxes, and NEB paths between two structures. Place solvent molecules by hand ([recipe 12](recipes/hydrated_sodium.md)) or use a continuum model (COSMO, SMD).

An MCP client (AI) can be asked to do the same in words. The tools and examples are in “[MCP server](mcp.md)”.
