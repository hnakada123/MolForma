# Changes

## Next version (in development)

This update adds ways to keep long calculations going, reads values that the logs already hold, and adds thermochemistry post-processing.

| Addition or change | Where to start | Scope |
| --- | --- | --- |
| Restarting calculations | [History](features/history.md#restarting-and-carrying-over-calculations) → Prepare to continue | Go on, as a new job, from the structure, molecular orbitals and Hessian that a failed, stopped or timed-out calculation left in NWChem’s database; an interrupted numerical Hessian after the displacements already done |
| Carrying over Hessians and orbitals | [History](features/history.md#restarting-and-carrying-over-calculations) → Next calculation from this result | A transition-state search with the Hessian of a frequency job, frequencies for other temperatures or isotopes that reuse the Hessian, a recalculation that starts from the molecular orbitals |
| Calculations that outlive the window | [3 Run settings](features/execution.md#keep-calculations-running-after-the-window-closes) | A supervisor process manages the calculation, which goes on when the window closes. The next session follows it again, and History can stop calculations of other windows |
| Restoring the queue | [3 Run settings](features/execution.md#calculation-queue) | The waiting calculations and their order are saved and restored, paused, at the next startup |
| Job scripts | [3 Run settings](features/execution.md#run-on-another-computer-job-script) → Job script | “Save as job” adds a `job.sh` for a shell, Slurm or PBS |
| Importing calculations run elsewhere | [History](features/history.md#import-calculations-run-elsewhere) | Register the output brought back from another computer, or turn the output of an NWChem run made without MolForma into a job |
| Orbital contributions of excited states | [Result viewer](features/results.md) → Excited states | The contributions and weights of occupied-to-virtual orbital pairs for each TDDFT excited state |
| Bond indices | [Result viewer](features/results.md) → Properties | The bond indices and valencies of the Mulliken analysis |
| Thermochemistry corrections and rate constants | [History](features/history.md#thermochemistry-corrections-and-rate-constants) → More | Frequency scaling, quasi-RRHO entropies for low-frequency modes (Grimme/Truhlar), a 1 mol/L standard state, Eyring rate constants and exp(−ΔG/RT) |
| Conformer-averaged spectra | [Tools → Conformer set…](features/molecule_pages.md#conformer-averaged-spectra) | Population-weighted IR, normal Raman, UV–Vis and ECD spectra and mean NMR shieldings |
| MCP | [MCP server](mcp.md) | Restarting, carrying over and importing from the history; `--mcp-keep-runs` (calculations go on when the connection ends) |

### Before using the next version

- A restart copies the database of the original job into a new job folder and begins with `restart`. Coordinate scans, IRC and BSSE-corrected calculations, and calculations whose `permanent_dir` or `scratch_dir` is outside the job, cannot be restarted. A restarted calculation cannot change the molecule or the constraints.
- What goes on after the window closes is the one running calculation. The waiting queue is restored at the next startup but starts nothing until you resume it. Whether a calculation survives a restart of the computer or a logout depends on the operating system’s settings.
- Transfer, submission and monitoring over SSH are not supported. `job.sh` is a template for one node; copying to the other computer and bringing the results back is manual.
- The thermochemistry corrections replace only the vibrational parts of the values NWChem printed. The defaults (scale 1, harmonic oscillators, 1 atm) keep NWChem’s values. Rate constants use the Eyring equation with a transmission coefficient of 1, taking the free-energy difference from the reference stage as the barrier.
- Bond indices depend on the basis set and can be unreliable with diffuse functions.

### Saving and compatibility of the next version

Existing projects, jobs, energy comparisons and conformer sets are read and written as before. The restart settings (`document.nwchem.restart`) and the kind of job script (`document.nwchem_execution.job_script`, `job_queue`) are saved in the project. Jobs run with earlier versions can be restarted as long as their folder still holds the database. Records left as running by earlier versions have no supervisor, so History cannot stop them.

### Verification of the next version

The build and all 16 CTest suites were checked on Linux. Restarts were checked with real NWChem 7.3.1 calculations of small molecules (going on with an “Optimize → Frequencies” stopped by its step limit, going on with a numerical Hessian, a transition-state search with the Hessian of a frequency job, and `reuse` of `freq`). Going on after the window closes and following again were checked with the test process; they have not been checked on Windows, macOS or from an AppImage.

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
