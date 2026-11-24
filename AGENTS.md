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
  project serialization (format `molforma-project`, version 1) as JSON (`project`, `fromProject`); it
  reads and writes no file (`saveProject`/`loadProject` of `io/document_storage.*` do).
  `elements.*` owns the element table (`Element`, `elements()`, `element()`), with the covalent and van
  der Waals radii and the one test of what a metal is (`Element::metal`), and whose colors are plain
  channels (`Rgb`): the model uses Qt Core only, and the UI makes a `QColor` of one (`toColor` in
  `ui/theme.h`). `xyz.cpp` holds the XYZ dialect (`Document::xyz`, `fromXyz`) and `bond_inference.cpp`
  the bonds by distance (`Document::inferBonds`), by the one rule of the distance that bonds two atoms
  (`bondDistance`), which the crystal reader uses for its molecules too. `geometry.*` owns
  double-precision geometry, the hydrogen bonds of a structure (`hydrogenBonds`),
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
  `chemistry.*` uses the Open Babel C++ API (the bonds of atoms that come with none, `perceiveBonds`:
  by distance with their orders from the geometry, for XYZ text, crystals and NWChem inputs; SMILES,
  refinement, conformers); `babel_molecule.h` declares its conversion between a document and an Open Babel
  molecule (`toOB`, `fromOB`) for `io/babel_io.cpp`, the only other file that includes it. Editing reads
  no structure file: the format readers are in `src/io/`, and what editing needs of a residue
  is in `peptide.*` (`aminoResidue`, `polymerResidue`, `waterResidue`, and `backboneTrace`: the line of
  the backbone display). `chemistry_job.*` owns cancellable chemistry
  processes, stale-result checks and candidate application, including runs of the external `xtb`
  (`xtb.*`: its input, `$fix` of fixed atoms and reading `xtbopt.xyz`; the program comes from
  Preferences `xtb/executable`, never from MCP).
  `stereochemistry.*` optionally uses native RDKit for CIP and residue recognition.
  `coordinates.*` implements coordinate edits, structure replacement and Z-matrices.
- `src/io/`: `structure_io.*` routes project, XYZ, NWChem, Gaussian and native Open Babel
  import/export (`babel_io.*`: the formats read and written through the Open Babel API, mmCIF atoms
  made a molecule the same way, and the SMILES of a MOL/SDF file; `macromolecule.*`: what a PDB or
  PDBx/mmCIF entry holds and the part of it to read, the same for both formats (`MacromoleculeSelection`,
  the count of chains and residues `MacromoleculeCount`, the alternate location of each residue
  `AltlocChoice`); `pdb.*`: the records of a PDB file; `mmcif.*`: PDBx/mmCIF entries and the blocks of
  any CIF file, read by one walk of the CIF grammar; `crystal.*`: small-molecule crystal structures of
  CIF files and their molecules), with
  the notes of what an NWChem input holds that was not imported
  (`importStructureWithNotes`), and the file an NEB end is read from (`nebEndFromFile`: which files are
  logs, the last structure of a structure file, the 128 MiB limit; the GUI reads logs through its
  cancellable `ResultLoader` and `nebEndFromLog`). `document_storage.*` owns project persistence (`saveProject`,
  `loadProject`: the project file of a document), recent files and locked
  autosave/recovery copies. `file_download.*` owns cancellable, atomic file transfers;
  `pdb_download.*` owns PDB identifiers/URLs and `pubchem.*` the query, cache naming and 3D-to-2D fallback.
  The download services use Qt Network without widgets; recovery choices, progress and file dialogs stay in
  the UI.
- `src/nwchem/`: `nwchem.*` owns setting specifications, defaults, presets, applicability and
  "2 計算条件" sections (`NwchemSection`, `nwchemSectionUse`; panels verbalize results, never compare
  translated text). It also owns site-cut point charges (`cutOutNwchemSite`, `replaceWithNwchemSite`)
  and derived tasks/names (`deriveNwchemCalculation`: "_ts", "_irc", within `MaximumNwchemNameLength`).
  `nwchem_catalog.*` shares basis/functionals and pasted-basis parsing; `nwchem_paths.*` handles
  constraints, scans, fragments and NEB endpoints; `nwchem_validation.*` checks settings and chemistry.
  `nwchem_plan.*` is the sole task source: ordered phases/commands and restart skips (the first
  `finished` generated steps). Restart validation compares plan commands with finished tasks;
  never generate input to discover tasks or extract tasks from generated text. `nwchem_input.cpp`
  and `nwchem_output.*` emit the next generated step through `NwchemTaskCursor`/
  `NwchemInputBuilder::task`, omitting skipped steps. Additional-input `task` lines remain user text:
  counted, never skipped or emitted by the generator. `nwchemInputStages`/`nwchemInputDriverSteps`
  in `nwchem_plan.*` read saved input for progress; results readers read output only.
  `input_builder.h` records source lines during generation; never infer them by diffing/parsing input.
  `nwchem_advanced.*` owns typed overrides/applicability; `nwchem_geometry.*` generates geometry and
  fixed-atom constraints; `nwchem_output.*` checks visualization settings and generates CUBE/MOLDEN tasks.
  `execution_environment.*` reads/saves machine-wide Preferences (new-document execution defaults,
  remote destinations, script preparation lines/header templates) and loads their `ExecutionEnvironment`
  snapshot through `executionEnvironment()`.
  `nwchem_execution.h` aggregates execution headers for outside callers; internal files include only
  what they use. Responsibilities: `execution_settings.*` owns document launch defaults/ranges/accessors,
  paths (`absolutePath`, `networkPath`, `untrustedNetworkPath`) and `validateExecution` (find each
  `ExecutionCheck` by `key`, never label); `execution_plan.*` owns `ExecutionPlan`, `executionPlan`,
  `executionPreviewPlan`, displayed `executionCommand` and immutable `saveExecutionBundle`;
  `job_script.*` owns remote `job.sh` (`executionJobScript`), scheduler templates/placeholders
  (`defaultJobHeader`, `jobHeaderErrors`, `renderJobHeader`), displayed `jobSubmitCommand` and
  `remoteJobPreview`; `run_process.*` owns process identity/liveness (`executionProcessIdentity`,
  `executionProcessGone`, `executionRunGone`, `runSupervised`) and `requestRunStop`.
  `nwchem_run.*` owns cancellable `NwchemRun`, as child or persistent supervisor (`run_supervisor.*`:
  `runJobMain`, `--run-job <job>`, `runSupervisorProgram`), followed/stopped through `run.json` and
  `stop.request`. `execution_detail.h` declares shared shell quoting, control-character, job-root and
  stop-request helpers; define each once.
  `remote_execution.h` aggregates remote headers: `remote_host.*` owns `RemoteHost` validation/text;
  `remote_shell.*` internal ssh/scp scripts/replies; `remote_supervisor.*` copy/submit/follow/fetch
  (`remoteJobMain`, `resumeRemoteRun`); `remote_run.*` record interpretation (`remoteRun`,
  `remoteRunText`); `remote_probe.*` connection checking (`RemoteProbe`, "接続を確認").
  `validateExecution`, `executionPlan`, `executionPreviewPlan`, `executionJobScript`,
  `saveExecutionBundle`, `untrustedNetworkPath` and `remoteJobPreview` take `ExecutionEnvironment`;
  neither they nor callees read preferences. Single calls default to the loader. `NwchemRun::start`,
  queue registration/execution, `RunSession::saveBundle` and the remote supervisor each load one
  snapshot at run/save entry and pass it down (`test_services executionUsesTheEnvironmentItIsGiven`).
  `nwchem_queue.*` executes bundles sequentially and persists waiting entries; `QueueChain` takes a
  completed predecessor's structure through `chainedDocument`, then saves its bundle.
  `run_session.*` owns the session's run, queue, last bundle, conformer batch and continuation state
  (supervisor, persisted queue/lock, Preferences `execution/running`: this layer's only preference
  outside `execution_environment.*`). It has no widgets or authorization rules: the window and
  headless MCP each hold a session and gate requests before calling it.
  NEB (`task` "neb") stores its endpoint in `neb_end` by atom ID, maintained by `Document::remove`/
  `renameIds` and the project adapter. `nwchem_restart.*` prepares database continuations (`restart`:
  input uses `restart`, omits geometry, skips finished tasks) and copies files to the new job.
  `run_record.*` alone owns job filenames (`JobFile`) and `run.json` reading/writing (`readRunRecord`,
  `writeRunRecord`), completion (`recordRunEnd`), saved plans (`runRecordPlan`) and typed `RunRecord`
  (state, times, exit code, summary, diagnostics, remote part). UI/MCP use it exclusively;
  never name job files or parse `run.json` elsewhere.
  `history.*` scans bundles/validates snapshots; `history_list.cpp` supplies filtered/sorted
  `historyList` with reference ΔE, state labels (`historyStateName`) and energy-comparison exclusions
  (`historyDifference`); the history tab only presents it. `history_import.cpp` registers external runs.
  `result_files.*` interprets manifests for generic readers, including visualization outputs and
  excluding intermediate NEB paths. `conformer_set.*` owns version-1 persistence, job associations,
  cancellable loading and population/spectrum-member preparation; its UI supplies settings/actions only.
- `src/results/`: `results.*` holds result data and routes readers; `nwchem_log.*`,
  `xyz_reader.cpp`, `cube_reader.cpp` and `molden_reader.cpp` parse their formats. `nwchem_log.h`
  declares the log reader (`NwchemLog`, whose state is in the .cpp, and `NwchemEnergySummary`); the
  wording of NWChem's output is known in `nwchem_log.cpp` alone: other layers ask it what a line says
  (`nwchemLineReports` for the failures the run diagnostics tell apart, `nwchemOutputStart`, the closing
  line and its wall time) and do not match log text themselves. `result_parsing.*`
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
- `src/ui/`: `main_window.*` owns project lifecycle, editor context and the session's `RunSession`
  (supervise/restore/end runs; `main.cpp` asks the window). Dialogs take capabilities and a parent,
  never MainWindow: `preferences_dialog.*` ("環境設定", document execution settings via callback),
  `xtb_dialog.*` (`chooseXtbOptions` returns `XtbOptions` for the window to run), `help_dialogs.*`
  (manual/practice-file callback, shortcuts, `initManualResources`). Menu wrappers stay thin:
  `preferences`, `xtbDialog`, `showManual`, `showShortcuts`.
  Panels/dialogs receive only needed `editor_context.h` capabilities plus a separate widget parent.
  Only composition/adapters (`main_window`, `workspace_controller`, `mcp_gui`) include MainWindow.
  `EditorContext` exposes document/candidate reads (`document`, `displayDocument`, `revision`,
  `hasCandidate`, `candidateName`, `candidateSerial`, `ownsCandidate`), selection and requests for
  edits/candidates/chemistry jobs; never `EditorSession` or its undo stack. `ownsCandidate` controls
  incidental confirmation; a person never implicitly confirms MCP's candidate.
  `UiServices` supplies `safe`, preferences, messages, status and MCP path checks: `checkMcpPath`
  refuses paths during MCP requests; `outsideMcpFolders` checks regardless of caller, supporting
  widgets' `mcpPath` marks between requests.
  `WorkspaceActions` is navigation only: `showPanel(QWidget *)`, `showSetting(SettingTarget, setting,
  group)`, `showInputLine`, `refreshPanel`. Name places by panel/setting owner, never tab number.
  Key-only callers use `nwchemSettingTarget` (`nwchem/nwchem.h`; memory belongs to execution);
  diagnostics carry targets. `ExecutionAccess` gates runs: `runCheck` checks MCP folders only for MCP;
  `mayChooseExecutionSettings`; `mayConfirmExecution` permits asking users, refuses MCP;
  `untrustedExecution`/`trustCurrentExecution` forward to `ExecutionTrust` in `mcp/execution_trust.*`,
  notified of every document change and its actor. Panels never see `McpPolicy`.
  Other inter-panel requests use signals connected at composition; do not expand interfaces for one caller.
  The window owns `userPreferences()` and passes it to `DocumentStorage` for recent files.
  Widgets, including `figure_export.*`, use `UiServices::preference`/`setPreference` with
  `model/settings.h` keys, never their own `QSettings`. Panels pass `RendererChoice` (Preferences
  `view/renderer`) to each `MoleculeView` constructor. `translations.*` reads language before the window.
  `molecule_panel.*` composes 3D tools/overlays, selection, coordinate layouts and tool options in
  `molecule_properties.cpp` ("編集", "配位", "残基": element, bond, ligand and amino-acid fields).
  Components take `EditorContext`/`UiServices`, never include panels (`cmake/CheckLayers.cmake`):
  `part_palette.*` (`PartPalette`) owns search/categories/elements and "マイ部品" (selection/file
  imports persisted in Preferences), emits `chosen`, `choiceChanged`, `refreshed`, `showRequested`,
  never places parts; `property_page.*` (`PropertyPage`) provides form cards, `UiServices::safe`
  button rows and selection-dependent `selection_controls.*`; `transform_page.*`, `attributes_page.*`,
  `charge_page.*` need only editor context/selection; `molecule_dialogs.*` returns choices for SMILES,
  reports/comparison, Z-matrix, point group, solvation shell and PDB diagnostics. The panel opens dialogs
  and edits through `EditorContext` (backed by the window's `EditorSession`). `candidate_controls.*`
  shares candidate UI; `structure_import.*` shares XYZ frame and PDB MODEL/ALTLOC choices.
  `workspace_controller.*` connects history reuse, navigation, completed-run loading and result
  geometry as an NEB endpoint (`CalculationPanel::resultStructureRequested` answered by
  `setNebEndFromResultStructure` or failure). Panels emit requests; never access siblings directly.
  The window connects header/menu state (`ExecutionPanel::runDisplayChanged`, `runPanelToggled`,
  `InputPreviewPanel::badgeChanged`, `settingsCommitRequested`; input preview gets calculation-section
  titles by callback). The molecule panel connects its table's `CoordinatesPanel::sortRequested`,
  `tableRequested`, `fitRequested` and reports when the table replaces 3D via `setViewHidden`.
  `molecule_view.*` owns camera/input, Qt annotations and QPainter fallback; `vtk_molecule_renderer.*`
  provides optional retained VTK/OpenGL rendering. Both use `molecule_style.h` for visibility,
  sphere/bond/backbone sizes, background, bond/isosurface colors; keep drawing rules there.
  Compute `MoleculeView::markerList` from document/camera, never cache handles from painting.
  `editor_chrome.*` owns `HeaderBar`, `ToolRailButton`, `OverlayHost`. Shared helpers use direct
  includes, no re-export headers: `theme.*` owns colors/styles (`QColor`, palette-derived colors,
  `ChromeColors`, `chromeRules` tokens, `settingsRules`, `repolish`); `line_icons.*` owns `lineIcon`,
  `stepIcon`, `changeIcon`; `chrome_layouts.*` owns `FlowLayout`, `FormLayout`, `ToolbarFit`/
  `setIconOnly`; `settings_widgets.*` owns `ChoiceButtons`, `NavList`, `SettingsCard`, `Disclosure`,
  `StatusCard`, `ElidedLabel`, link/log/copy buttons; `calculation_summary.*` owns
  `CalculationSummary`/`calculationBrief` (the sole NWChem-validation caller among these helpers);
  `view_controls.*` binds `ViewControls` to `MoleculeView`. `run_progress.*` owns run labels/colors
  (`pillColors`, `pillRules`). `template_delegate.*` renders/caches palette thumbnails/tiles;
  `periodic_table.*` supplies the element picker.
  Numbered tabs: `coordinates_panel.*` supplies "1 分子"'s `CoordinateModel` over the displayed
  document, below/in place of 3D (3D / 3D＋表 / 表); `calculation_panel.*` supplies "2 計算条件",
  with each section's `nwchem_advanced.*` overrides under "詳細設定". Log-based NEB endpoints use
  cancellable `ResultLoader` and check revision before applying. `execution_panel.*` supplies
  "3 実行設定" and the below-tabs run panel (state, `run_progress.*`, log), opened by the header
  run pill; `execution_form.*` is shared with Preferences. It displays the window's `RunSession`,
  owns neither run nor queue, and calls it only after `ExecutionAccess` checks no candidate,
  user-confirmed programs and MCP folders for MCP requests. `setting_navigation.h` maps owners to
  tabs (`workspaceTab`): the window navigates; panels use numbers only in link labels.
  `RunDiagnostic::owner` adapts old job diagnostics. `input_preview_panel.*` supplies read-only
  "4 入力プレビュー" through the shared input generator/execution plan.
  `results_panel.*` composes the independent result viewer: files/jobs loaded by cancellable
  `ResultLoader`, dataset/structure/mode/orbital selection and `results/result_presentation.*` playback,
  3D view, exports and bound tabs (概要, 軌道, 振動, CUBE / 等値面, 元ファイル), each built by a named
  function. Read-only `ResultData` tabs live in separate files, expose `setResult` and emit requests:
  `result_energies_view.*`, `result_optimization_view.*`, `result_excitations_view.*` (`orbitalRequested`,
  job MOLDEN file via callback), `result_path_view.*` (`frameRequested`, `saddleSearchRequested`,
  path from `results/result_path.*`), `result_properties_view.*` (`atomPairSelected`),
  `property_geometry_view.*`, `spectroscopy_view.*`. `result_measurement.*` tracks picked/kept atoms
  by number; every displayed structure passes through it. `cube_combine_dialog.*` asks for CUBE
  sum terms/output; `result_detail.h` declares shared tables, numeric text, summary cards and chart
  export helpers. UV/IR curves use `results/spectrum.*`; IR panel and UV view own axis limits/units.
  `figure_export.*` exports 3D pictures (`MoleculeView::renderImage`: scale/background, tiled VTK),
  animations (own GIF encoder, MP4 via PATH `ffmpeg`, PNG files) and charts (PNG/PDF, optional Qt SVG).
  `history_panel.*` shows `HistoryModel`, logs, reuse and result navigation. `mcp_gui.*` exposes
  GUI/execution/results through public editor/panel operations and snapshots; no MainWindow or sibling
  friendship. Playback kind is explicit, independent of timer intervals.
- `src/mcp/`: `server.*` implements the MCP JSON-RPC server (protocols 2026-07-28
  and legacy 2025-11-25) with document tools, candidates and undo; editor hooks
  let it operate a GUI document, otherwise it owns a headless one. `server.cpp` keeps the protocol,
  the Tasks extension, resources and dispatch; the other parts of `McpServer` are
  `document_adapter.cpp` (the document behind the tools: the editor's or its own session, summaries,
  edits with MCP's size limit) and `document_tools.cpp` (the registrations of the tools with their
  handlers), with `json_schema.*` (the schemas of tools and their validator), `prompts.*` (the prompt
  catalogue) and `server_detail.h` (what those files share) internal to this folder. `headless.*`
  adds NWChem runs (a `RunSession` of its own) and result loading to `--mcp`; `tools.*` holds tool
  definitions and result views shared with `mcp_gui.*`; `policy.*` holds the
  command-line access policy (protected execution settings, allowed directories).
  `execution_trust.*` owns the trust of those protected settings (`ExecutionTrust`, no widgets and no
  preference read): the values a run may use without asking are those the user entered in MolForma or
  confirmed before a run, and those saved in Preferences, which the caller hands it; values of loads,
  undo/redo and MCP count only once confirmed. The window keeps one for its session; `McpHeadless`,
  where nothing is entered or confirmed, asks one that stays empty (`test_mcp_server
  executionTrustCountsWhatTheUserEnteredOrConfirmed`).
  `stdio_transport.*` provides newline-delimited stdio transport. See `docs/mcp.md`.

`src/main.cpp` is the entry point; `--mcp` runs the headless MCP server and
`--mcp-gui` runs it with the editor window. Include project headers relative to
`src/`, for example `#include "model/document.h"`. The `molforma_core` target
is an interface aggregate of `molforma_model`, `molforma_editing`, `molforma_results`,
`molforma_nwchem` and `molforma_io`. Dependencies flow from io to nwchem, from nwchem
to editing/results, and from editing/results to model, then from model to the project-schema adapter
and `molforma_foundation`. All layers except project may use foundation. Only `molforma_ui` links
Qt Widgets; foundation, project, model and results use Qt Core alone, `molforma_editing` links Qt Gui
for the undo stack of `EditorSession` (`QUndoStack`; the layers above get it from there), and Qt
Network is private to io. Open Babel is linked by editing, nwchem, io and ui only. `molforma_ui` contains ui and links to
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
`tests/fake_nwchem.cpp`, `tests/fake_ssh.cpp` and `tests/fake_xtb.cpp` stand in for NWChem, ssh/scp and xtb.
CTest uses `QT_QPA_PLATFORM=offscreen`. Tests require no credentials or network.

Each `tests/test_*.cpp` is one executable and one CTest test, and the tests are laid out by layer.
Tests that need no widget are in executables that link `molforma_core` alone, include no `ui/` header
(`cmake/CheckLayers.cmake` checks their sources and the helpers they share) and build without the UI
(`cmake --build build --target test_runs`): `test_core` (model, editing, coordinates, file formats),
`test_services` (storage, downloads, chemistry jobs, run sessions), `test_nwchem` (settings and their
checks, task plan, generated input and its outline, advanced options, CUBE/MOLDEN tasks, input read
back), `test_runs` (launch settings, plans and bundles, runs of the stand-in NWChem, queue, restarts,
history scans), `test_result_readers` (NWChem logs, paths, spectra, energy profiles, XYZ/CUBE/MOLDEN
files and orbitals) and `test_mcp_server` (the MCP server and the headless runtime in the test's own
process; it links `molforma_mcp` too). The executables of the screens link `molforma_ui` and hold the
tests that construct a window, panel, dialog or view or need another `ui/` header: `test_editor`,
`test_coordinates`, `test_calculation`, `test_advanced`, `test_input_preview`, `test_output`,
`test_execution`, `test_history`, `test_results`, `test_molden`, `test_rendering`,
`test_translations` and `test_mcp`.
Tests that start the built `molforma` (`test_mcp`: `--mcp`, `--mcp-gui`, `--screenshot`;
`test_execution`: the supervisor of runs) stay with those, so that the core executables need it
neither linked nor built. Put a new test where its layer is, and a helper that several executables
need into `tests/*_support.h` (`test_support.h`, `nwchem_support.h`, `run_support.h`,
`result_support.h`, `mcp_support.h`) instead of copying it.
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
