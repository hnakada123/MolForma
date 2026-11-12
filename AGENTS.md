# Repository Guidelines

## Structure

MolForma is a native Qt6 Widgets / C++20 molecular editor. Sources are grouped
by responsibility, with paired headers and implementations in the same directory:

- `src/foundation/`: `files.*` owns byte-level file reads, atomic writes and JSON parsing;
  `structure_file.*` reads plain/gzip structure files with bounded decompression. `i18n.*` owns translation and
  `error.h` the common exception helper. This layer uses Qt Core and zlib, with no domain dependencies.
- `src/project/`: `atom_references.*` adapts the persisted atom references in version-1 project
  settings when IDs are removed or renamed, preserving unknown fields. It depends only on Qt Core;
  the molecular model delegates this schema traversal to it. Its one list of the settings that hold
  atom IDs (`nwchemAtomReferenceKeys`) must name the same settings as the specifications mark
  (`NwchemSettingSpec::atomReferences`); `test_core atomReferenceSettings` compares the two.
- `src/model/`: `document.*` owns the stable-ID document and the `.molforma.json`
  project serialization (format `molforma-project`, version 1); `geometry.*` owns double-precision geometry,
  point groups and the passes that symmetrize a structure onto one.
  Atom references kept in a part's metadata (the eta groups of a ligand, `Part::hapto`) go through typed
  accessors, and `Document::remove`/`renameIds`/`validate` keep them like any other reference.
  `setting_target.h` defines semantic owners (molecule, calculation, execution), independent of
  workspace tabs, and the one mapping to the legacy numbers of old job records and MCP responses
  (`legacySettingTab`, `settingTargetFromLegacyTab`). `diagnostic.h` defines diagnostics with stable codes, setting targets and typed fixes, and
  `ValidationFailure`, which throws a refused report whole. `units.h` owns coordinate units and
  physical conversion constants; `numbers.h` reads numbers with Fortran exponents ("1.5D-03").
  `topology.*` indexes a document's atoms by ID with their bonded neighbors for loops over many atoms
  (`Document::atom`/`index`/`neighbors`/`bond` scan the whole document), and `memo.h` keeps the last
  results of checks that several panels run on the same document.
  `settings.h` owns the user's preferences as a store: its format (`SettingsFormat`), the one factory
  that constructs it (`userPreferences()`; never write `QSettings(…)` elsewhere) and the names of every
  key (`Preference::…`; never a key as a literal). Keys and the formats of their values are those of the
  files users have (`test_services preferenceKeysStayAsStored`).
- `src/editing/`: `library.*`, `operations.*`, `peptide.*` and `coordination.*`
  implement editing: `operations.*` also the orders of atoms (`sortAtoms`) and mirror images as copies,
  `peptide.*` also a sequence built beside or in place of a structure (`buildSequence`). Panels and MCP
  call these; a widget holds no such rule of its own. `editor_session.*` owns document edits, candidates and undo for both GUI and MCP,
  and the revision that stale checks and the MCP `revision` compare (never serialize documents for that).
  `atom_properties.*` shares atom-property edits and validation across panels and MCP.
  `chemistry.*` uses the Open Babel C++ API; `chemistry_job.*` owns cancellable chemistry
  processes, stale-result checks and candidate application, including runs of the external `xtb`
  (`xtb.*`: its input, `$fix` of fixed atoms and reading `xtbopt.xyz`; the program comes from
  Preferences `xtb/executable`, never from MCP).
  `stereochemistry.*` optionally uses native RDKit for CIP and residue recognition.
  `coordinates.*` implements coordinate edits, structure replacement and Z-matrices.
- `src/io/`: `structure_io.*` routes project, XYZ, NWChem, Gaussian and native Open Babel
  import/export, with the notes of what an NWChem input holds that was not imported
  (`importStructureWithNotes`), and the file an NEB end is read from (`nebEndFromFile`: which files are
  logs, the last structure of a structure file, the 128 MiB limit; the GUI reads logs through its
  cancellable `ResultLoader` and `nebEndFromLog`). `document_storage.*` owns project persistence, recent files and locked
  autosave/recovery copies. `file_download.*` owns cancellable, atomic file transfers;
  `pdb_download.*` owns PDB identifiers/URLs and `pubchem.*` the query, cache naming and 3D-to-2D fallback.
  These services use Qt Network without widgets; recovery choices, progress and file dialogs stay in the UI.
- `src/nwchem/`: `nwchem.*` owns computational setting specifications, defaults, presets and applicability
  (also of the sections of "2 計算条件": `NwchemSection` with its keys, and `nwchemSectionUse`, whose
  result the panel puts into words; never compare translated text),
  and the settings that follow an edit: the point charges of a site cut out (`cutOutNwchemSite`,
  `replaceWithNwchemSite`) and the task and name of a calculation derived from another
  (`deriveNwchemCalculation`: "_ts", "_irc", within `MaximumNwchemNameLength`).
  `nwchem_catalog.*` shares basis/functionals and pasted-basis parsing; `nwchem_paths.*` handles
  constraints, scans, fragments and NEB endpoints. `nwchem_validation.*` validates primitive settings
  and chemical combinations. `nwchem_plan.*` owns the task plan, the one source of
  which tasks a calculation has: ordered steps with their phase, command and whether a continued restart
  skips them (the first `finished` generated ones). Restart validation compares its commands with the
  finished tasks and must not generate input to discover tasks. `nwchem_input.cpp`, which generates the
  complete input, and `nwchem_output.*` write every task as the plan's next generated step
  (`NwchemTaskCursor`, `NwchemInputBuilder::task`) and nothing for a skipped one; never take tasks out of
  generated text. `task` lines of the additional input are the user's text: steps that are counted, but
  neither skipped nor written by the generator. `nwchem_plan.*` also reads what the input of a saved job
  says of its run for the progress view (`nwchemInputStages`, `nwchemInputDriverSteps`); the results layer
  reads what NWChem wrote, never its input. `input_builder.h` records source lines as input is generated; do not infer them
  by diffing or parsing generated text. `nwchem_advanced.*` defines typed overrides and applicability.
  `nwchem_geometry.*` generates geometry fragments and fixed-atom constraints.
  `nwchem_output.*` validates visualization settings and generates CUBE/MOLDEN output
  tasks. `execution_environment.*` owns the machine-wide part of running calculations: the functions
  that read and save what Preferences keep of it (the execution settings of new documents, the remote
  destinations, the preparation lines and header templates of job scripts) and the snapshot of all of
  it, `ExecutionEnvironment`, with its one loader `executionEnvironment()`.
  `nwchem_execution.*` validates launch settings (each `ExecutionCheck` names its setting by
  `key`; find a check by that, not by its label), saves immutable run bundles
  (with `job.sh` for runs on another computer) and controls cancellable NWChem processes:
  as a child process, or through a supervisor (`runJobMain`, the executable's `--run-job <job>`
  mode) that outlives the window and is followed and stopped through `run.json` and `stop.request`.
  Its checks and generators (`validateExecution`, `executionPlan`, `executionPreviewPlan`,
  `executionJobScript`, `saveExecutionBundle`, `untrustedNetworkPath`; `remoteJobPreview`) take the
  `ExecutionEnvironment` as an argument and read no preference themselves, nor does anything they
  call: the argument defaults to the loader for single calls, and `NwchemRun::start`, the queue
  (registering an entry, its turn), `RunSession::saveBundle` and the remote supervisor load one snapshot
  where a run or a save begins and pass it down. A test hands them a hand-built snapshot
  (`test_services executionUsesTheEnvironmentItIsGiven`).
  `nwchem_queue.*` runs saved bundles in sequence and keeps the waiting ones in a file between
  sessions; a chained entry (`QueueChain`) takes the structure of an earlier entry's job
  (`chainedDocument`) when that has finished and saves its bundle only then. `run_session.*` owns the
  calculations of one session: the run, the queue over it, the bundle saved last, the conformer batch and
  what the session leaves for the next one (the supervisor, the stored queue with its lock, the running
  job in Preferences `execution/running`: besides `execution_environment.*` the only preference this
  layer reads or writes). It has no widgets and no say in who may run what: the window
  and the headless MCP server each hold one and check a request at their own gate before asking it.
  NEB (`task` "neb") keeps
  its end in the setting `neb_end` (positions by atom ID, kept through `Document::remove`/`renameIds` and the project adapter). `nwchem_restart.*` prepares documents that go on from a job's NWChem database
  (settings `restart`: the input says `restart`, has no geometry and skips finished tasks) and
  copies its files into the new job. `run_record.*` owns the job folder and its record `run.json`:
  the file names (`JobFile`), the one reader and writer of the record (`readRunRecord`, `writeRunRecord`),
  what the end of a run records (`recordRunEnd`), the plan a record was saved from (`runRecordPlan`) and
  the typed view `RunRecord` (state, times, exit code, summary, diagnostics, remote part). UI and MCP read
  records only through it; do not name a file of a job folder or parse `run.json` elsewhere.
  `history.*` scans saved bundles and validates snapshots;
  `history_import.cpp` registers runs made outside MolForma. `result_files.*` interprets
  job manifests and supplies the result-file list, including visualization outputs and excluding
  intermediate NEB paths, to the generic result readers.
  `conformer_set.*` owns version-1 conformer-set persistence, job associations, cancellable result
  loading and population/spectrum-member preparation; its UI only supplies settings and actions.
- `src/results/`: `results.*` holds result data and routes readers; `nwchem_log.cpp`,
  `xyz_reader.cpp`, `cube_reader.cpp` and `molden_reader.cpp` parse their formats. `result_parsing.*`
  shares lexical readers and bounds, `result_surface.cpp` generates bounded isosurfaces,
  `result_cache.*` keeps parsed data and orbital grids, and `result_codec.*` owns the versioned binary
  worker/cache payload. `result_worker.cpp` owns the worker entry points. `result_files.*` only
  discovers physical files, groups animations and checks source stamps; job schemas stay in nwchem.
  `NwchemLog` shares one optimization state (task, steps, thresholds and convergence) between live
  progress and loaded results. `result_presentation.*` owns frame/mode selection, playback and the
  reference/visible geometry used by the result viewer and its exports. `molden_orbitals.*` evaluates
  normalized Gaussian MOs with Cartesian/spherical s–g bases. `result_loader.*` owns cancellable
  reader processes and source freshness checks shared by GUI and headless MCP. `result_properties.*` gives the
  facts of `ResultData::properties` that more than one file reads as typed values (the calculation's
  energy, whether the optimization converged, the dipole); read them there, not by their keys, which
  `results.h` lists. `result_path.*` gives the
  reaction path of a result as a typed `ResultPath` (a scan, an NEB or an IRC: its points in the order of
  the path with frame, energy and place, and the reference energy) and its highest point with the ends
  counted or not; the result viewer's path table and `chainedDocument` take the points of a path from
  it, not from `ResultData::properties` or from table cells.
- `src/ui/`: `main_window.*` owns the application window, project lifecycle, shared editor context and
  the session's `RunSession` (supervising, restoring and ending its runs; `main.cpp` asks the window).
  It implements the four capabilities of `editor_context.h`, which panels/dialogs receive (only those
  they use) with a separate widget parent; only composition/adapters (`main_window`,
  `workspace_controller`, `mcp_gui`) include MainWindow. `EditorContext` is the document: reading it and
  its candidate (`document`, `displayDocument`, `revision`, `hasCandidate`, `candidateName`,
  `candidateSerial`, `ownsCandidate`: whether the one acting now may confirm the candidate in passing,
  which a person never does to a candidate of MCP), the selection, and asking for edits, candidates and
  chemistry jobs; never the `EditorSession` or its undo stack. `UiServices` is what every widget needs of
  the window: `safe`, preferences, messages, the status bar, and MCP's folders as two checks
  (`checkMcpPath` refuses a path during a request of MCP; `outsideMcpFolders` says whether a path is
  outside them whoever acts, for the `mcpPath` mark of a widget shown between requests).
  `WorkspaceActions` is navigation only: `showPanel(QWidget *)`, `showSetting(SettingTarget, setting,
  group)`, `showInputLine`, `refreshPanel`. A place is named by its panel or by the owner of a setting,
  never by a tab number; a caller that has only the key of a setting asks `nwchemSettingTarget`
  (`nwchem/nwchem.h`: the memory is the execution's), and diagnostics carry their target.
  `ExecutionAccess` is the gate of a calculation as checks (`runCheck`: MCP's folders for a request of
  MCP, else none; `mayChooseExecutionSettings`; `mayConfirmExecution`: the user can be asked, a request
  of MCP is refused; `untrustedExecution`, `trustCurrentExecution`); no panel sees the `McpPolicy`.
  Anything else one panel needs of another is a signal of the panel that asks, connected where the two
  are composed: do not add a method to these interfaces for one caller.
  The window holds the preferences store (`userPreferences()`, also handed to `DocumentStorage` for
  the recent files); widgets reach preferences only through `UiServices::preference`/`setPreference`
  with the keys of `model/settings.h`, never through a `QSettings` of their own: what a dialog remembers
  (`figure_export.*`) goes the same way, and the renderer of a `MoleculeView` is passed to its
  constructor (`RendererChoice`, from Preferences `view/renderer`) by the panel that makes it.
  `translations.*` reads the language before there is a window.
  `molecule_panel.*` and `molecule_properties.cpp` own the molecular workspace, selection, palette,
  tools and property pages; document transactions go through the `EditorContext` (the window's
  `EditorSession` behind it).
  `candidate_controls.*` shares the candidate controls across workspace tabs.
  `structure_import.*` shares the interactive XYZ frame and PDB MODEL/ALTLOC choices.
  `workspace_controller.*` coordinates history reuse, navigation, loading completed runs and the
  structure the result viewer shows as the end of an NEB path (`CalculationPanel::resultStructureRequested`,
  answered with `setNebEndFromResultStructure` or a failure); panels emit requests instead of directly
  accessing sibling panels. The window itself connects what its header and menus show of a panel: the
  run (`ExecutionPanel::runDisplayChanged`, `runPanelToggled`) and the state of the input
  (`InputPreviewPanel::badgeChanged`, `settingsCommitRequested`; the panel gets the section titles of
  "2 計算条件" as a callback). The molecular workspace composes its coordinate table and connects the
  table's requests itself (`CoordinatesPanel::sortRequested`, `tableRequested`, `fitRequested`), telling
  it when it stands in place of the 3D view (`setViewHidden`). `molecule_view.*` owns camera/input, Qt annotations and
  the QPainter fallback; `vtk_molecule_renderer.*` provides optional retained
  native VTK/OpenGL rendering; both draw bonds and isosurfaces by `bond_style.h`. `editor_chrome.*` holds the window header, tool rail,
  wrapping option bar layout, 3D-view overlays and palette-derived chrome colors.
  `template_delegate.*` renders and caches the palette's molecular thumbnails and
  tiles; `periodic_table.*` provides the element picker.
  The numbered tabs are "1 分子" through "4 入力プレビュー". `coordinates_panel.*`
  provides the coordinate table of "1 分子" (a `CoordinateModel` over the shown document),
  shown under the 3D view or in its place (layouts 3D / 3D＋表 / 表). `calculation_panel.*` provides "2 計算条件"; each
  section opens its `nwchem_advanced.*` overrides as "詳細設定". NEB endpoints read from output logs use
  `ResultLoader`, with cancellation and a revision check before applying them. `execution_panel.*`
  provides the settings of "3 実行設定" and the run panel below the tabs (state,
  `run_progress.*` and log), opened from the header run pill; its form (`execution_form.*`) is
  also that of Preferences. The panel shows the window's `RunSession` and asks it for calculations once
  its gate has let them pass (no candidate, programs the user confirmed, MCP's folders for a request over
  MCP: all asked of `ExecutionAccess`); it owns neither the run nor the queue. `setting_navigation.h` maps semantic setting targets to today's UI tabs
  (`workspaceTab`): the window uses it to show the tab of an owner, a panel only for the number it writes
  beside a link; the owner of an old job diagnostic is `RunDiagnostic::owner`. `run_progress.*` also holds the state labels of runs and their colors. `input_preview_panel.*`
  provides the read-only "4 入力プレビュー" tab using the shared NWChem input
  generator and execution plan.
  `results_panel.*` provides the independent result viewer with cancellable native
  readers. UV/IR curves use `results/spectrum.*`; the panel owns axis limits and units.
  `figure_export.*` writes pictures of the 3D view (`MoleculeView::renderImage`: scale and
  background, VTK in tiles), animations (its own GIF encoder, MP4 through `ffmpeg` from PATH, PNG
  files) and figures of charts (PNG, PDF, and SVG with the optional Qt SVG). `history_panel.*` provides calculation history (its list a `HistoryModel`), log
  browsing, reuse and result navigation. `mcp_gui.*` exposes GUI, execution and result tools to MCP
  through public editor/panel operations and state snapshots; panels do not access
  MainWindow or one another through friendship. Playback kind is explicit, independent of timer intervals.
- `src/mcp/`: `server.*` implements the MCP JSON-RPC server (protocols 2026-07-28
  and legacy 2025-11-25) with document tools, candidates and undo; editor hooks
  let it operate a GUI document, otherwise it owns a headless one. `headless.*`
  adds NWChem runs (a `RunSession` of its own) and result loading to `--mcp`; `tools.*` holds tool
  definitions and result views shared with `mcp_gui.*`; `policy.*` holds the
  command-line access policy (protected execution settings, allowed directories).
  `stdio_transport.*` provides newline-delimited stdio transport. See `docs/mcp.md`.

`src/main.cpp` is the entry point; `--mcp` runs the headless MCP server and
`--mcp-gui` runs it with the editor window. Include project headers relative to
`src/`, for example `#include "model/document.h"`. The `molforma_core` target
is an interface aggregate of `molforma_model`, `molforma_editing`, `molforma_results`,
`molforma_nwchem` and `molforma_io`. Dependencies flow from io to nwchem, from nwchem
to editing/results, and from editing/results to model, then from model to the project-schema adapter
and `molforma_foundation`. All layers except project may use foundation. Only `molforma_ui` links
Qt Widgets; core libraries use Qt Core/Gui, with Qt Network private to io. `molforma_ui` contains ui and links to
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
`model/settings.h`: an INI file on Windows and macOS; `userPreferences()` there is the store the
redirection moves) and `XDG_CONFIG_HOME` / `XDG_DATA_HOME`
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

The interface is Japanese or English (`src/foundation/i18n.h`). Write user-visible
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
