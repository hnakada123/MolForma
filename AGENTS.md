# Repository Guidelines

## Structure

MolForma is a native Qt6 Widgets / C++20 molecular editor. Sources are grouped
by responsibility, with paired headers and implementations in the same directory:

- `src/model/`: `document.*` owns the stable-ID document and the `.kgui.json`
  project serialization (format version 2); `geometry.*` owns double-precision geometry.
- `src/editing/`: `library.*`, `operations.*`, `peptide.*` and `coordination.*`
  implement editing. `chemistry.*` uses the Open Babel C++ API.
  `stereochemistry.*` optionally uses native RDKit for CIP and residue recognition.
  `coordinates.*` implements coordinate edits and NWChem geometry fragments.
- `src/nwchem/`: `nwchem.*` validates computational settings and generates complete
  NWChem input; `nwchem_advanced.*` defines typed overrides and their applicability.
  `nwchem_output.*` validates visualization settings and generates CUBE/MOLDEN output
  tasks. `nwchem_execution.*` validates launch settings, saves immutable run bundles
  and controls cancellable NWChem processes. `history.*` scans saved bundles and
  validates snapshots.
- `src/results/`: `results.*` reads NWChem logs, XYZ, CUBE and MOLDEN and generates
  bounded isosurfaces; `molden_orbitals.*` evaluates normalized Gaussian MOs with
  Cartesian/spherical s–g bases.
- `src/ui/`: `main_window.*` and `editor_panels.cpp` provide the Qt UI, candidate
  edits and undo history. `molecule_view.*` owns camera/input, Qt annotations and
  the QPainter fallback; `vtk_molecule_renderer.*` provides optional retained
  native VTK/OpenGL rendering. `template_delegate.*` renders and caches the palette's
  molecular thumbnails; `periodic_table.*` provides the element picker.
  `coordinates_panel.*`, `calculation_panel.*`, `advanced_panel.*` and
  `execution_panel.*` provide the native tabs "2 Coordinates" through
  "5 Execution settings". `input_preview_panel.*` provides the read-only
  "6 Input preview" tab using the shared NWChem input generator and execution plan.
  `results_panel.*` provides the independent result viewer with cancellable native
  readers. `history_panel.*` provides calculation history, log browsing, reuse and
  result navigation. `mcp_gui.*` exposes GUI, execution and result tools to MCP
  by driving the editor window.
- `src/mcp/`: `server.*` implements the MCP JSON-RPC server (protocols 2026-07-28
  and legacy 2025-11-25) with document tools, candidates and undo; editor hooks
  let it operate a GUI document, otherwise it owns a headless one. `headless.*`
  adds NWChem runs and result loading to `--mcp`; `tools.*` holds tool
  definitions and result views shared with `mcp_gui.*`; `policy.*` holds the
  command-line access policy (protected execution settings, allowed directories).
  `stdio_transport.*` provides newline-delimited stdio transport. See `docs/mcp.md`.

`src/main.cpp` is the entry point; `--mcp` runs the headless MCP server and
`--mcp-gui` runs it with the editor window. Include project headers relative to
`src/`, for example `#include "model/document.h"`. The `molforma_core` target
contains model, editing, nwchem and results; `molforma_ui` contains ui and links
to core and mcp; `molforma_mcp` contains mcp and links to core.

Tests are in `tests/`, documentation in `docs/`, and resource data in `assets/`.
Keep `LICENSE` (Apache-2.0) and `NOTICE` current.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Dependencies: Qt6 Widgets/Test, Eigen3, Open Babel 3, CMake >= 3.21.
CTest uses `QT_QPA_PLATFORM=offscreen`. Tests require no credentials or network.
Format with `rg --files src tests -g '*.h' -g '*.cpp' -0 | xargs -0 clang-format -i`.

## Conventions

Use `.clang-format`. Store physical coordinates as doubles in angstrom;
camera movement must not edit them. Store atom-dependent references using
stable IDs and translate them to indices at export. UI edits operate on a
copy and commit only after validation. Candidate edits stay separate until
confirmed. Fixed atoms must not move. Long chemistry operations run in a
cancellable process, and stale results must be rejected.

Add deterministic regression tests, especially for file compatibility,
connectivity, geometry, undo and candidates. Preserve project envelope fields.
Tests must not write the user's settings or data: redirect
`QSettings::NativeFormat` (MolForma's `QSettings("MolForma", "MolForma")`) and
`XDG_CONFIG_HOME` / `XDG_DATA_HOME` (autosave) to temporary directories.
Document limitations in `docs/features.md` (Japanese); MCP tool changes belong
in `docs/mcp.md`.

## Changes and security

Use short imperative commit subjects. PRs describe behavior, checks and
screenshots for UI changes. Keep builds, logs, credentials and generated
outputs out of version control. Do not add runtime dependence on Python.
