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
  native VTK/OpenGL rendering. `editor_chrome.*` holds the window header, tool rail,
  wrapping option bar layout, 3D-view overlays and palette-derived chrome colors.
  `template_delegate.*` renders and caches the palette's molecular thumbnails and
  tiles; `periodic_table.*` provides the element picker.
  The numbered tabs are "1 分子" through "4 入力プレビュー". `coordinates_panel.*`
  provides the coordinate table of "1 分子", shown under the 3D view or in its place
  (layouts 3D / 3D＋表 / 表). `calculation_panel.*` provides "2 計算条件"; each
  section opens its `nwchem_advanced.*` overrides as "詳細設定". `execution_panel.*`
  provides the settings of "3 実行設定" and the run panel over the tabs (state,
  `run_progress.*` and log), opened from the header run pill. `input_preview_panel.*`
  provides the read-only "4 入力プレビュー" tab using the shared NWChem input
  generator and execution plan.
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
MolForma is GPL-2.0-or-later (it links the GPL-2.0-only Open Babel); keep new
dependencies GPLv2-compatible and `LICENSE` and `NOTICE` current.

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
in `docs/mcp.md`. Keep the English manuals `docs/en/features.md` and
`docs/en/mcp.md` in step with them.

The interface is Japanese or English (`src/model/i18n.h`). Write user-visible
text in Japanese inside `tr("…")` (`QT_TR_NOOP("…")` in tables, shown through
`translated()`), with `%1` templates instead of concatenated fragments and
`tr("…%n…", nullptr, n)` for counts; core functions return translated text.
Add the English text to `assets/translations/en.json` (`["one", "other"]` for
`%n` plurals). The `translations` test fails on unmarked Japanese literals in
`src/` (exempt one with `// i18n-exempt`), missing or unused entries and
placeholder mismatches. Never translate NWChem input, identifiers or text that
is saved and read back. Tests run without a translator, so they see Japanese.

## Changes and security

Use short imperative commit subjects. PRs describe behavior, checks and
screenshots for UI changes. Keep builds, logs, credentials and generated
outputs out of version control. Do not add runtime dependence on Python.
