# Changes

## 1.1.0 (2026-11-10)

This update adds tools for running multiple calculations locally and visualizing properties calculated by NWChem.

| Addition or change | Where to start | Scope |
| --- | --- | --- |
| Calculation queue | [3 Run settings](features/execution.md#calculation-queue) → Add to queue | Save the molecule and settings at registration, then run jobs one at a time. Reorder, cancel, pause and choose the failure policy |
| Failure diagnostics | [Run panel](features/execution.md#failure-diagnosis) | Explain recognized SCF convergence, optimization-limit, memory, basis, launch and timeout failures with log evidence |
| Recalculate from a result | [Result viewer](features/results.md#prepare-a-new-calculation-from-a-result-geometry) → Prepare a new calculation | Restore the original job settings and propose the displayed geometry as a candidate |
| Thermal corrections and reaction profiles | [History](features/history.md#thermal-corrections-and-reaction-energy-diagrams) → More | Compare E, E + ZPE, H or G, sum weighted components at each stage, save JSON and export CSV/PNG |
| Conformer sets | [Tools → Conformer set…](features/molecule_pages.md#conformer-sets) | Save conformer-search results, queue optimizations and frequencies, compare results and estimate populations within the set |
| NMR | [Result viewer → Spectra](features/results.md#nmr-normal-raman-and-solution-esr-spectra) | Unsplit ¹H and ¹³C charts using an explicitly supplied reference shielding |
| Normal Raman | [2 Calculation → Properties](features/calculation.md#additional-spectroscopy-and-property-grids) | Closed-shell DFT frequencies and Raman activity charts from a log or `.normal` file |
| ESR/EPR | [Result viewer → Spectra](features/results.md#nmr-normal-raman-and-solution-esr-spectra) | S = 1/2 solution absorption and first derivatives from doublet DFT/ZORA g and A tensors |
| ECD and ORD | [Result viewer](features/results.md#ecd-and-ord) | Length-gauge TDDFT ECD and isotropic GIAO optical-rotation β at multiple wavelengths |
| Properties in 3D and grid slices | [Result viewer](features/results.md#properties-in-3d-and-slices) | ESP-colored surfaces, ESP/field-magnitude slices, dipole arrows and polarizability axes/ellipsoids |
| MCP | [MCP server](mcp.md) | Queue operations, spectrum curves and peaks, and energy comparisons with thermal corrections |
| Author credit | Help → About MolForma | Shows the author, Hiroshi Nakada |

### Before using these features

- The queue runs sequentially while MolForma is open. Closing the app stops its running calculation and leaves the waiting jobs' saved bundles in history. Queue restoration, automatic resumption, concurrent jobs, SSH and Slurm/PBS are unsupported.
- “Prepare a new calculation” prepares a new job. It does not perform NWChem `restart` with a saved database or Hessian.
- NMR J splitting and paramagnetic NMR, resonance Raman, powder/high-spin ESR and conformer-averaged spectra are unsupported. ECD is not converted to Δε, nor ORD β to specific rotation.
- For free-energy comparisons, check correction temperatures, sources and imaginary frequencies. The standard-state field is a record label; it does not apply concentration or pressure corrections. Conformer populations describe only the selected set, with degeneracy 1 per group.
- Check CUBE units against the generating calculation. A color grid must have matching atom order and coordinates and cover the entire surface; it is not aligned automatically.

### Saving and compatibility

Existing MolForma projects (`molforma-project`, format version 1) remain readable and writable. New calculation settings are saved in the project. Explicitly save conformer sets, energy comparisons and spectrum display settings to separate JSON files from their respective windows. Spectrum display settings are tied to the original result file.

### Verification

The 1.1.0 build, all 16 CTest suites and the Japanese/English documentation site build passed on Linux. Readers and property conventions were checked against small-molecule NWChem 7.3.1 calculations for Raman, ESR, ECD, ORD and ESP/electric-field CUBE output. These small-basis calculations check behavior, not quantitative agreement with experiment. Historical verification records in the OS installation guides refer to the versions stated there.

See also: [Features](features.md), [Recipes](recipes.md).
