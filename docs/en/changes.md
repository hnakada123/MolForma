# Changes

## 1.1.0 (2026-11-10)

This update adds tools for running multiple calculations locally, keeping long calculations going, and visualizing properties calculated by NWChem.

| Addition or change | Where to start | Scope |
| --- | --- | --- |
| Calculation queue | [3 Run settings](features/execution.md#calculation-queue) → Add to queue | Save the molecule and settings at registration, then run jobs one at a time. Reorder, cancel, pause and choose the failure policy. The waiting calculations and their order are saved and restored, paused, at the next startup |
| Calculations that outlive the window | [3 Run settings](features/execution.md#keep-calculations-running-after-the-window-closes) | A supervisor process manages the calculation, which goes on when the window closes. The next session follows it again, and History can stop calculations of other windows |
| Job scripts | [3 Run settings](features/execution.md#run-on-another-computer-job-script) → Job script | “Save as job” adds a `job.sh` for a shell, Slurm or PBS |
| Failure diagnostics | [Run panel](features/execution.md#failure-diagnosis) | Explain recognized SCF convergence, optimization-limit, memory, basis, launch and timeout failures with log evidence |
| Recalculate from a result | [Result viewer](features/results.md#prepare-a-new-calculation-from-a-result-geometry) → Prepare a new calculation | Restore the original job settings and propose the displayed geometry as a candidate |
| Restarting calculations | [History](features/history.md#restarting-and-carrying-over-calculations) → Prepare to continue | Go on, as a new job, from the structure, molecular orbitals and Hessian that a failed, stopped or timed-out calculation left in NWChem’s database; an interrupted numerical Hessian after the displacements already done |
| Carrying over Hessians and orbitals | [History](features/history.md#restarting-and-carrying-over-calculations) → Next calculation from this result | A transition-state search with the Hessian of a frequency job, frequencies for other temperatures or isotopes that reuse the Hessian, a recalculation that starts from the molecular orbitals |
| Importing calculations run elsewhere | [History](features/history.md#import-calculations-run-elsewhere) | Register the output brought back from another computer, or turn the output of an NWChem run made without MolForma into a job |
| Thermal corrections and reaction profiles | [History](features/history.md#thermal-corrections-and-reaction-energy-diagrams) → More | Compare E, E + ZPE, H or G, sum weighted components at each stage, save JSON and export CSV/PNG |
| Thermochemistry corrections and rate constants | [History](features/history.md#thermochemistry-corrections-and-rate-constants) → More | Frequency scaling, quasi-RRHO entropies for low-frequency modes (Grimme/Truhlar), a 1 mol/L standard state, Eyring rate constants and exp(−ΔG/RT) |
| Conformer sets | [Tools → Conformer set…](features/molecule_pages.md#conformer-sets) | Save conformer-search results, queue optimizations and frequencies, compare results and estimate populations within the set |
| Conformer-averaged spectra | [Tools → Conformer set…](features/molecule_pages.md#conformer-averaged-spectra) | Population-weighted IR, normal Raman, UV–Vis and ECD spectra and mean NMR shieldings |
| Orbital contributions of excited states | [Result viewer](features/results.md) → Excited states | The main transition of each TDDFT excited state and the contributions of occupied-to-virtual orbital pairs (X and Y amplitudes and weights) |
| Bond indices | [Result viewer](features/results.md) → Properties | The bond indices (bond orders) and valencies of the Mulliken analysis |
| NMR | [Result viewer → Spectra](features/results.md#nmr-normal-raman-and-solution-esr-spectra) | Unsplit ¹H and ¹³C charts using an explicitly supplied reference shielding |
| Normal Raman | [2 Calculation → Properties](features/calculation.md#additional-spectroscopy-and-property-grids) | Closed-shell DFT frequencies and Raman activity charts from a log or `.normal` file |
| ESR/EPR | [Result viewer → Spectra](features/results.md#nmr-normal-raman-and-solution-esr-spectra) | S = 1/2 solution absorption and first derivatives from doublet DFT/ZORA g and A tensors |
| ECD and ORD | [Result viewer](features/results.md#ecd-and-ord) | Length-gauge TDDFT ECD and isotropic GIAO optical-rotation β at multiple wavelengths |
| Properties in 3D and grid slices | [Result viewer](features/results.md#properties-in-3d-and-slices) | ESP-colored surfaces, ESP/field-magnitude slices, dipole arrows and polarizability axes/ellipsoids |
| MCP | [MCP server](mcp.md) | Queue operations, spectrum curves and peaks, energy comparisons with thermal corrections, restarting, carrying over and importing from the history, and `--mcp-keep-runs` (calculations go on when the connection ends) |
| Author credit | Help → About MolForma | Shows the author, Hiroshi Nakada |

### Before using these features

- The queue runs sequentially while MolForma is open. What goes on after the window closes is the one running calculation; the waiting jobs are restored at the next startup but start nothing until you resume the queue. Concurrent jobs are unsupported. Whether a calculation survives a restart of the computer or a logout depends on the operating system’s settings.
- Transfer, submission and monitoring over SSH are unsupported. `job.sh` is a template for one node; copying to the other computer and bringing the results back is manual.
- “Prepare a new calculation” prepares a new job from the displayed geometry. To carry over NWChem’s database or a Hessian, use “Prepare to continue” or “Next calculation from this result” in “History”. A restart copies the database of the original job into a new job folder and begins with `restart`. Coordinate scans, IRC and BSSE-corrected calculations, and calculations whose `permanent_dir` or `scratch_dir` is outside the job, cannot be restarted, and a restarted calculation cannot change the molecule or the constraints.
- NMR J splitting and paramagnetic NMR, resonance Raman and powder/high-spin ESR are unsupported. ECD is not converted to Δε, nor ORD β to specific rotation.
- For free-energy comparisons, check correction temperatures, sources and imaginary frequencies. The thermochemistry corrections replace only the vibrational parts of the values NWChem printed; the defaults (scale 1, harmonic oscillators, 1 atm) keep NWChem’s values. Rate constants use the Eyring equation with a transmission coefficient of 1, taking the free-energy difference from the reference stage as the barrier. Conformer populations describe only the selected set, with degeneracy 1 per group.
- Bond indices depend on the basis set and can be unreliable with diffuse functions. The orbital weights of an excited state come only from the contributions NWChem printed (amplitudes above 0.05 in magnitude).
- Check CUBE units against the generating calculation. A color grid must have matching atom order and coordinates and cover the entire surface; it is not aligned automatically.

### Saving and compatibility

Existing MolForma projects (`molforma-project`, format version 1) remain readable and writable. New calculation settings, the restart settings (`document.nwchem.restart`) and the kind of job script (`document.nwchem_execution.job_script`, `job_queue`) are saved in the project. Jobs run with 1.0.0 can be restarted as long as their folder still holds the database. Records left as running by 1.0.0 have no supervisor, so History cannot stop them. Explicitly save conformer sets, energy comparisons and spectrum display settings to separate JSON files from their respective windows. Spectrum display settings are tied to the original result file.

### Verification

The 1.1.0 build, all 16 CTest suites and the Japanese/English documentation site build passed on Linux. Readers and property conventions were checked against small-molecule NWChem 7.3.1 calculations for Raman, ESR, ECD, ORD and ESP/electric-field CUBE output. Restarts were checked with real NWChem 7.3.1 calculations of small molecules too (going on with an “Optimize → Frequencies” stopped by its step limit, going on with a numerical Hessian, a transition-state search with the Hessian of a frequency job, and `reuse` of `freq`). Going on after the window closes and following again were checked with the test process. The build and all 16 CTest suites also passed in the build environment of the AppImage (Ubuntu 24.04, Qt 6.4), and with the AppImage made there a calculation went on after its session ended and was followed and stopped by the next one. Neither has been checked on Windows or macOS. These small-basis calculations check behavior, not quantitative agreement with experiment. Historical verification records in the OS installation guides refer to the versions stated there.

See also: [Features](features.md), [Recipes](recipes.md).
