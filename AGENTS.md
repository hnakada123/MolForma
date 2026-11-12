# Repository Guidelines

## Structure

MolForma is a native Qt6 Widgets / C++20 molecular editor. Sources are grouped
by responsibility, with paired headers and implementations in the same directory:

- `src/model/`: `document.*` owns the stable-ID document and the `.molforma.json`
  project serialization (format `molforma-project`, version 1); `geometry.*` owns double-precision geometry.
  `diagnostic.h` defines diagnostics with stable codes, setting targets and typed fixes, and
  `ValidationFailure`, which throws a refused report whole. `units.h` owns coordinate units and
  physical conversion constants; `numbers.h` reads numbers with Fortran exponents ("1.5D-03").
  `topology.*` indexes a document's atoms by ID with their bonded neighbors for loops over many atoms
  (`Document::atom`/`index`/`neighbors`/`bond` scan the whole document), and `memo.h` keeps the last
  results of checks that several panels run on the same document.
- `src/editing/`: `library.*`, `operations.*`, `peptide.*` and `coordination.*`
  implement editing. `editor_session.*` owns document edits, candidates and undo for both GUI and MCP,
  and the revision that stale checks and the MCP `revision` compare (never serialize documents for that).
  `atom_properties.*` shares atom-property edits and validation across panels and MCP.
  `chemistry.*` uses the Open Babel C++ API; `chemistry_job.*` owns cancellable chemistry
  processes, stale-result checks and candidate application, including runs of the external `xtb`
  (`xtb.*`: its input, `$fix` of fixed atoms and reading `xtbopt.xyz`; the program comes from
  Preferences `xtb/executable`, never from MCP).
  `stereochemistry.*` optionally uses native RDKit for CIP and residue recognition.
  `coordinates.*` implements coordinate edits, structure replacement and Z-matrices.
- `src/io/`: `structure_io.*` routes project, XYZ, NWChem, Gaussian and native Open Babel
  import/export; `document_storage.*` owns project persistence, recent files and locked
  autosave/recovery copies. Recovery choices and file dialogs stay in the UI.
- `src/nwchem/`: `nwchem.*` validates computational settings and generates complete
  NWChem input. Its setting specifications share defaults, types, ranges, choices and applicability
  with the UI; `nwchem_validation.*` validates basic values from these specifications, independently
  of the chemical combinations. `input_builder.h` records source lines as input is generated; do not infer them
  by diffing or parsing generated text. `nwchem_advanced.*` defines typed overrides and applicability.
  `nwchem_geometry.*` generates geometry fragments and fixed-atom constraints.
  `nwchem_output.*` validates visualization settings and generates CUBE/MOLDEN output
  tasks. `nwchem_execution.*` validates launch settings, saves immutable run bundles
  (with `job.sh` for runs on another computer) and controls cancellable NWChem processes:
  as a child process, or through a supervisor (`runJobMain`, the executable's `--run-job <job>`
  mode) that outlives the window and is followed and stopped through `run.json` and `stop.request`.
  `nwchem_queue.*` runs saved bundles in sequence and keeps the waiting ones in a file between
  sessions; a chained entry (`QueueChain`) takes the structure of an earlier entry's job
  (`chainedDocument`) when that has finished and saves its bundle only then. NEB (`task` "neb") keeps
  its end in the setting `neb_end` (positions by atom ID, kept through `Document::remove`/`renameIds`). `nwchem_restart.*` prepares documents that go on from a job's NWChem database
  (settings `restart`: the input says `restart`, has no geometry and skips finished tasks) and
  copies its files into the new job. `history.*` scans saved bundles and validates snapshots;
  `history_import.cpp` registers runs made outside MolForma.
- `src/results/`: `results.*` reads NWChem logs, XYZ, CUBE and MOLDEN and generates
  bounded isosurfaces; `NwchemLog` shares one optimization state (task, steps, thresholds and
  convergence) between live progress and loaded results. `result_presentation.*` owns frame/mode
  selection, playback and the reference/visible geometry used by the result viewer and its exports.
  `molden_orbitals.*` evaluates normalized Gaussian MOs with
  Cartesian/spherical s–g bases. `result_loader.*` owns cancellable reader processes and source
  freshness checks shared by GUI and headless MCP.
- `src/ui/`: `main_window.*` owns the application window, project lifecycle and shared editor context.
  `molecule_panel.*` and `molecule_properties.cpp` own the molecular workspace, selection, palette,
  tools and property pages; document transactions still go through `EditorSession`.
  `candidate_controls.*` shares the candidate controls across workspace tabs.
  `structure_import.*` shares the interactive XYZ frame and PDB MODEL/ALTLOC choices.
  `workspace_controller.*` coordinates history reuse, navigation and loading completed runs;
  panels emit requests instead of directly accessing sibling panels. The molecular workspace
  composes its coordinate table. `molecule_view.*` owns camera/input, Qt annotations and
  the QPainter fallback; `vtk_molecule_renderer.*` provides optional retained
  native VTK/OpenGL rendering; both draw bonds and isosurfaces by `bond_style.h`. `editor_chrome.*` holds the window header, tool rail,
  wrapping option bar layout, 3D-view overlays and palette-derived chrome colors.
  `template_delegate.*` renders and caches the palette's molecular thumbnails and
  tiles; `periodic_table.*` provides the element picker.
  The numbered tabs are "1 分子" through "4 入力プレビュー". `coordinates_panel.*`
  provides the coordinate table of "1 分子" (a `CoordinateModel` over the shown document),
  shown under the 3D view or in its place (layouts 3D / 3D＋表 / 表). `calculation_panel.*` provides "2 計算条件"; each
  section opens its `nwchem_advanced.*` overrides as "詳細設定". `execution_panel.*`
  provides the settings of "3 実行設定" and the run panel below the tabs (state,
  `run_progress.*` and log), opened from the header run pill; its form (`execution_form.*`) is
  also that of Preferences. `run_progress.*` also holds the state labels of runs and their colors. `input_preview_panel.*`
  provides the read-only "4 入力プレビュー" tab using the shared NWChem input
  generator and execution plan.
  `results_panel.*` provides the independent result viewer with cancellable native
  readers. `figure_export.*` writes pictures of the 3D view (`MoleculeView::renderImage`: scale and
  background, VTK in tiles), animations (its own GIF encoder, MP4 through `ffmpeg` from PATH, PNG
  files) and figures of charts (PNG, PDF, and SVG with the optional Qt SVG). `history_panel.*` provides calculation history (its list a `HistoryModel`), log
  browsing, reuse and result navigation. `mcp_gui.*` exposes GUI, execution and result tools to MCP
  through public editor/panel operations and state snapshots; panels do not access
  MainWindow or one another through friendship. Playback kind is explicit, independent of timer intervals.
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
is an interface aggregate of `molforma_model`, `molforma_editing`, `molforma_results`,
`molforma_nwchem` and `molforma_io`. Dependencies flow from io to nwchem, from nwchem
to editing/results, and from editing/results to model. Only `molforma_ui` links
Qt Widgets; core libraries use Qt Core/Gui. `molforma_ui` contains ui and links to
core and mcp; `molforma_mcp` contains mcp and links to core. `cmake/CheckLayers.cmake`
checks source-header dependency directions during configuration and in CTest.

Tests are in `tests/`, documentation in `docs/`, and resource data in `assets/`.
MolForma is GPL-2.0-or-later (it links the GPL-2.0-only Open Babel); keep new
dependencies GPLv2-compatible and `LICENSE` and `NOTICE` current.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Dependencies: Qt6 Widgets/Network/Test, Eigen3, zlib, Open Babel 3, CMake >= 3.21. Optional: RDKit,
Qt6-built VTK and Qt SVG (`MOLFORMA_HAS_QTSVG`); code must build and pass tests without them.
`tests/fake_nwchem.cpp` and `tests/fake_xtb.cpp` stand in for NWChem and xtb.
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
`QSettings::NativeFormat` and `IniFormat` (MolForma's settings use `SettingsFormat` from
`model/settings.h`: an INI file on Windows and macOS) and `XDG_CONFIG_HOME` / `XDG_DATA_HOME`
(autosave; MolForma honors them on Windows and macOS too) to temporary directories.
Document limitations in the page of the screen in `docs/features/` (Japanese;
`docs/features.md` lists the pages, screenshots in `docs/features/images/`); MCP
tool changes belong in `docs/mcp.md`. Keep `README.en.md` in step with `README.md`, and the
English manuals `docs/en/features*` and `docs/en/mcp.md` in step with them. The recipes (`docs/recipes.md`,
`docs/recipes/*.md`, screenshots in `docs/recipes/images/`, practice files
in `docs/recipes/samples/`, embedded with the manual) have English versions under `docs/en/`; update
both when a UI change alters a recipe's steps. `tools/screenshots` (see its
README) takes the screenshots by driving `molforma --mcp-gui`. `docs/` is also published as the
GitHub Pages site https://hnakada123.github.io/MolForma/: on pushes to main,
`.github/workflows/pages.yml` generates the HTML with Jekyll (`docs/Gemfile`,
`docs/Gemfile.lock`) and publishes it; preview with `bundle exec jekyll build`
or `serve` in `docs/` (output in the ignored `docs/_site`), after
`bundle config set --local path vendor/bundle && bundle install` there. The
pages link relative to themselves (`root` from `_includes/language.html`), so the
generated HTML also works opened as files. The site's sources are
`_config.yml`, `_layouts/`, `_includes/`, `assets/site.css`, the HTML
home pages `index.html` / `en/index.html` with feature and recipe cards from
`_data/features.yml` and `_data/recipes.yml`, which follow the tables of
`features.md` / `recipes.md` and their English versions). `_includes/head.html` sends browsers without Japanese
from the Japanese pages to their English versions (never the other way, and not search engines), unless the
language links chose a language (`remember_language` from `language.html`, kept in `localStorage`). Keep the manual pages plain Markdown without front
matter (the app shows them verbatim) and keep `{{` and `{%` out of them.

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
