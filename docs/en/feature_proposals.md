# Candidate additions to MolForma

Created: 2026-11-10

The scope implemented in 1.1.0 is summarized in [Changes](changes.md). This page records the proposals and possible later extensions.

The proposals below distinguish the starting capabilities, intended use, initial implementation and possible extensions. Priorities are suggestions, not a committed delivery schedule. NWChem capabilities are based on its documentation; support must be checked with actual calculations for each version, method and reference wavefunction.

## Building on the capabilities present before implementation

MolForma already covered building molecules, running NWChem and inspecting results. The main opportunities were to connect multiple calculations and make calculated properties easier to interpret.

- Saving, running, stopping, notifications, history and reuse were available. At the start of this work, each window ran one calculation, without a queue or NWChem restart. See [Run settings](features/execution.md) and [History](features/history.md) for the current behavior.
- Conformer search and structure superposition were available, with a chosen conformer proposed as an editing candidate. See [molecular editing](features/molecule_pages.md).
- IR, vibration animation, TDDFT UV–Vis absorption, IRC/scan graphs, thermochemistry and history ΔE comparisons were available. See [Results](features/results.md) and [History](features/history.md).
- Electron/spin densities and molecular-orbital isosurfaces were available. NMR shieldings, dipoles and polarizabilities could be read numerically. See [Output settings](features/advanced.md) and [Results](features/results.md).

## Implementation status (1.1.0, 2026-11-10)

Following the preference for local features, the initial scopes of A1/A2, B1/B2, B3, A3/A4, B4 and C1/C2 have been implemented. B5 supports multiple-wavelength GIAO calculations and charts of isotropic β in atomic units; conversion to specific rotation is not implemented. Instructions and limits are in [Run settings](features/execution.md), [Results](features/results.md), [History](features/history.md) and [Conformer sets](features/molecule_pages.md).

D1 remote execution (SSH, Slurm, PBS) is part of 1.1.0. Of the extensions considered later, restarts from NWChem's database, calculations that outlive the window with the queue restored, thermochemistry corrections, conformer-averaged spectra, reaction paths (NEB and chains of calculations that go on from earlier ones), preparing metal complexes with xTB, and exporting figures and videos are part of 1.1.0 too ([changes](changes.md)). Concurrent jobs, NMR J splitting, resonance Raman and powder/high-spin ESR remain later extensions. Recalculation from the final geometry is available as a new job without overwriting the original log.

Actual NWChem 7.3.1 checks used water for Raman, methyl radical for ESR and hydrogen peroxide for ECD/ORD. Inputs and expected reader values are retained as regression fixtures. These small-basis checks do not establish quantitative agreement with experiment.

## Candidate list

High means a candidate to start first, medium builds on existing or earlier additions, and use-dependent calls for checking the intended workload. Items within a priority do not have a strict order.

| ID | Addition | Initial scope | Priority |
| --- | --- | --- | --- |
| A1 | Local calculation queue | Register, reorder, run sequentially and cancel jobs | High |
| A2 | Failure explanations and resuming work | Classify failures, locate settings and recalculate from the last geometry | High |
| A3 | Free-energy comparisons and reaction diagrams | Compare results with thermal corrections; stage tables and charts | Medium |
| A4 | Conformer sets and batch evaluation | Save/select conformers, batch-optimize and list results | Medium |
| B1 | NMR spectra | Unsplit ¹H/¹³C spectra with an explicit reference | High |
| B2 | Raman spectra | Normal Raman calculations, reading and linked vibration selection | High |
| B3 | ESR/EPR spectra | Simple solution spectra for S = 1/2 radicals | Medium |
| B4 | Electronic circular dichroism (ECD) | Signed bands from TDDFT | Medium |
| B5 | Optical rotatory dispersion (ORD) | Calculate and chart optical rotation at multiple wavelengths | Use-dependent |
| C1 | Electrostatic potential and field magnitude | Color molecular surfaces and show grid slices | Medium |
| C2 | Dipoles and polarizabilities in 3D | Dipole arrows, polarizability axes and ellipsoids | Medium |
| D1 | SSH and Slurm/PBS integration | Register execution environments, submit, monitor and retrieve results | Use-dependent |

## A. Working with multiple calculations

### A1. Local calculation queue

Register calculations with different conformers, solvents or bases and run them in order, reducing the need to start each one manually.

- Save a fixed copy of the molecule, calculation settings and run settings on registration.
- Reorder or cancel waiting jobs, pause the queue and stop the active calculation.
- Choose whether a failure pauses subsequent jobs.
- Connect job states and results to history.

Start with one active job while the app is open. Resource-aware concurrency, running after the app exits and monitoring after restarting it require further execution management.

### A2. Failure explanations and resuming work

Distinguish SCF nonconvergence, optimization limits, insufficient memory, timeout and launch failure. Show log evidence and settings to review. Do not change conditions automatically: let the user inspect the change before making another job.

Keep two operations distinct:

- **Recalculation from the last geometry:** start a new calculation using the readable final structure and chosen conditions. This does not carry over a Hessian or internal state.
- **Restart from saved data:** reuse the NWChem database and required companion files. Check the task, files, names and paths, and retain the original job when recording a derived one.

Introduce database restart for specific supported tasks rather than assuming any interrupted calculation can continue at the same point. See [NWChem START/RESTART](https://nwchemgit.github.io/Start_Restart.html).

### A3. Free-energy comparisons and reaction diagrams

Connect ΔE and thermochemistry to compare electronic energies, ZPE, enthalpy and Gibbs corrections.

- Save source jobs, reference, units, temperature, solvent and standard state.
- Track electronic-energy and correction provenance separately and prevent double-counting.
- Identify combinations of a single-point energy and frequencies from different calculations.
- Arrange reactants, intermediates, transition states and products to show relative energies and barriers.
- Export tables as CSV and figures as images.

A stage containing multiple molecules is a sum with coefficients. Check temperatures, standard states, conditions, compositions and charge balance. Add quasi-harmonic corrections and conformer averaging only with recorded models and provenance.

### A4. Saving conformer sets and batch evaluation

Extend force-field search into retaining multiple conformers and evaluating them with quantum chemistry.

- Save named conformers and list structures and force-field energies.
- Queue selected conformers with common calculation settings.
- Group structures that converge to the same geometry, using RMSD or a comparable criterion.
- Compare convergence, imaginary modes and relative energies; calculate temperature-dependent Boltzmann populations when conditions agree.

Keep force-field and quantum-chemical energies in separate columns and types. State degeneracies and counting rules. A distribution over found conformers does not imply a complete search. Conformer-averaged spectra are a later extension.

## B. Spectroscopy

### B1. NMR spectra

Extend the atom-by-atom isotropic shielding and anisotropy table into chemical-shift charts. See the [current result viewer](features/results.md).

Start with ¹H/¹³C of ordinary diamagnetic molecules. Enter a reference such as TMS calculated under the same conditions, with selecting a reference job as a possible extension. With shieldings in ppm:

```text
sample δ [ppm] ≈ reference σ [ppm] − sample σ [ppm]
```

- Show sticks and broadened curves with larger ppm to the left.
- Link peaks to atoms and expose shielding, reference and chemical shift.
- Save the reference, width, weights and range; export tables and images.

Doublets, triplets and other splitting require J couplings and a spin-system calculation. NWChem provides `SPINSPIN`; supported references, input generation, reading and spectrum calculation need verification. See [NWChem NMR and EPR](https://nwchemgit.github.io/Properties.html#nmr-and-epr).

Experimental intensities and integrals depend on measurement conditions; atomic-count weights are not experimental intensities. Equivalent-nucleus, motional and conformer averaging and paramagnetic NMR require additional models.

### B2. Raman spectra

Plot intensity against Raman shift and link peaks to vibration animation, sharing mode selection with IR to aid comparison.

NWChem has a `RAMAN` block and `task dft raman`, with stick and Lorentz/Gauss-broadened output. Add input generation, output reading and display. See [NWChem Raman](https://nwchemgit.github.io/Properties.html#raman).

Start with normal Raman. Record calculation conditions such as excitation frequency separately from display width. Resonance Raman is a later extension after supported conditions and actual calculations are checked.

### B3. ESR/EPR spectra

Show absorption and its first derivative against magnetic field for radicals and other molecules with unpaired electrons. Combine this with spin-density visualization and atom selection to inspect the nuclei responsible for hyperfine splitting.

Read g and hyperfine A tensors from NWChem `GSHIFT` and `HYPERFINE`, retaining the g-shift conversion, units, isotope choices and coordinate frame. See [NWChem EPR and Paramagnetic NMR](https://nwchemgit.github.io/EPR-pNMR.html).

Start with **rapidly tumbling S = 1/2 radicals in solution**:

- Use isotropic g and A and nuclear identities to calculate splitting.
- Specify microwave frequency, field range, linewidth and isotope treatment.
- List per-nucleus A values and link them to atom selection.
- Consider displaying g/A principal axes on the molecule later.

Calculated g and A alone do not determine a unique spectrum: experimental conditions and a line-shape model are needed. See the [solution-spectrum inputs and example](https://easyspin.org/documentation/userguide_garlic.html).

Keep powder/frozen-solution orientation averaging, slow motion and high-spin zero-field splitting outside the initial scope. Citing EasySpin does not imply a runtime dependency on it.

### B4. Electronic circular dichroism (ECD)

Show signed bands for the difference in absorption of left/right circularly polarized light against wavelength or energy. Link to the same excited states used by UV–Vis display.

NWChem TDDFT offers `CDSPECTRUM`, `GIAO` and `VELOCITY`. Read excitation energies and rotatory strengths for supported calculations and broaden them with an explicit width. Do not derive ECD from ordinary absorption oscillator strengths alone. See [NWChem CDSpectrum](https://nwchemgit.github.io/Excited-State-Calculations.html#cdspectrum-optical-rotation-calculations).

Conformer spectra, weighted averages and overlays with experiment are future extensions. Raise priority if chiral molecules are a main use case.

### B5. Optical rotatory dispersion (ORD)

Chart optical rotation across wavelengths using tensors from NWChem `RESPONSE`, `ORBETA` and `GIAO`. Since response calculations specify one frequency at a time, manage the series and collect its results. See [NWChem Response Calculations](https://nwchemgit.github.io/Properties.html#response-calculations).

State the conversion conventions between atomic-unit tensors and displayed rotation or specific rotation. Comparison with ECD and conformer averaging are possible extensions.

## C. Properties in 3D

### C1. Electrostatic potential and field magnitude

Extend CUBE isosurfaces to:

- Color an electron-density surface with a separate electrostatic-potential grid.
- Display potential or field-magnitude slices as heatmaps.
- Specify unit-labeled color bars, bounds and common scales for multiple molecules.

NWChem `PROPERTY` with `ESP` or `EFIELD` and `GRID` can write potential or field-magnitude CUBE files. Manage these separately from ESP atomic charges. See [NWChem Gaussian Cube Files](https://nwchemgit.github.io/Properties.html#gaussian-cube-files).

Check atom positions, frames, coverage and units before overlaying grids, and interpolate as needed. A magnitude-only field grid does not provide vectors or field lines.

### C2. Dipoles and polarizabilities in 3D

Overlay dipole arrows and polarizability principal axes/ellipsoids to make polarity and directional response easier to inspect, using properties already read numerically.

State arrow convention, units and scales. Apply the same transform to molecule and property when rotating the camera or superposing structures. A charged molecule's dipole depends on origin, so retain the calculation coordinate information and origin provenance. See [Results](features/results.md) and [NWChem CENTER](https://nwchemgit.github.io/Properties.html#center-center-of-expansion-for-multipole-calculations).

## D. Remote execution

### D1. SSH and Slurm/PBS integration

Prioritize this when calculation servers are used frequently. Register host, work directory and resource settings; transfer input, submit, monitor, cancel and retrieve logs/results.

Start with one submission backend and associate remote job IDs with local history. Handle disconnection and reconnection independently of the local queue. Do not embed credentials in shared projects or run records. See the [current execution scope](features/execution.md).

Implemented in 1.1.0. Destinations are registered in Preferences only (projects keep their names), and the supervisor copies, submits, checks, stops and fetches back with the system’s `ssh` and `scp`, recording the job ID in `run.json`. A job whose supervisor ended can be reconnected from History. Shell, Slurm and PBS submissions are supported, with headers made from templates ([Run on a remote computer](features/execution.md#run-on-a-remote-computer-ssh-slurm-pbs)).

## Suggested order

1. **Calculation workflow:** local queue and failure explanations, followed by restart for specific tasks.
2. **Spectroscopy:** unsplit NMR and normal Raman, followed by S = 1/2 solution ESR.
3. **Comparing results:** free energies, conformer sets and batch calculations, then conformer-averaged spectra.
4. **Use-dependent extensions:** ECD/ORD, properties in 3D and remote execution, with priority adjusted to chiral-molecule and server use.

## Shared implementation principles

- Distinguish calculated values, display processing and user settings, and retain links to source jobs/files.
- Save widths, references, weights and comparison conditions needed for reproducing a display.
- Share core input generation, validation, numerical processing and readers between GUI and MCP. Keep Qt Widgets in the UI.
- Make long calculations, reading and spectrum work cancellable and reject stale results.
- Apply geometry changes through candidates, confirmation and Undo; viewing results must not change the editor molecule.
- Implement in C++ without a Python runtime dependency. Check GPLv2 compatibility before adopting libraries.
- Add deterministic checks for units, atom correspondence, missing output, repeated tasks, tensor rotations and save/reload.
- Document supported scope and limits in both languages, including MCP changes in both MCP manuals.
