# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

MolForma is a native Qt6 Widgets / C++20 molecular editor and NWChem input/run/result front end.
The UI text and user docs (`README.md`, `docs/`) are Japanese, with an English UI, README (`README.en.md`) and manual (`docs/en/`).
`AGENTS.md` has the same repository guidelines in a per-file form.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^editor$' --output-on-failure   # one test executable
QT_QPA_PLATFORM=offscreen ./build/test_editor someTestFunction   # one Qt Test slot
```

- Dependencies: Qt6 (>= 6.4) Widgets/Network/Test, Eigen3, zlib (gzip-compressed structure files), Open Babel 3
  (pkg-config `openbabel-3`, C++ API only), CMake >= 3.21. Optional: RDKit C++ (defines `MOLFORMA_HAS_RDKIT`,
  enables strict CIP/residue recognition), Qt6-built VTK >= 9.1 (defines `MOLFORMA_HAS_VTK`) and Qt SVG
  (defines `MOLFORMA_HAS_QTSVG`, SVG figures). Code must build and pass tests with none of them. At run time
  MolForma may start the external `xtb` (Preferences `xtb/executable`) and `ffmpeg` (PATH); neither is shipped.
- Alternate trees exist locally: `build-no-vtk` (`-DMOLFORMA_ENABLE_VTK=OFF`) and `build-asan`
  (`-DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer'`). Check rendering changes against the
  QPainter path too.
- Each `tests/test_*.cpp` is its own executable/CTest test, laid out by layer. Tests that need no widget are in
  executables that link `molforma_core` alone (`test_mcp_server`: with `molforma_mcp`) and include no `ui/` header
  (`cmake/CheckLayers.cmake` checks their sources), so a check of the core builds nothing of the UI
  (`cmake --build build --target test_nwchem`): `core` (model, editing, coordinates, file formats), `services`
  (storage, downloads, chemistry jobs, run sessions), `nwchem` (settings and their checks, task plan, generated
  input and its outline, advanced options, CUBE/MOLDEN tasks, input read back), `runs` (launch settings, plans and
  bundles, runs, queue, restarts, history scans), `result_readers` (NWChem logs, paths, spectra, energy profiles,
  XYZ/CUBE/MOLDEN files and orbitals) and `mcp_server` (protocol, tools, policy and the headless runtime, in the
  test's own process). The tests of a screen link `molforma_ui` and hold what constructs a window, panel, dialog or
  view or needs another `ui/` header: `editor`, `coordinates`, `calculation`, `advanced`, `input_preview`, `output`,
  `execution`, `history`, `results`, `molden`, `rendering`, `translations`, `mcp`.
  A new test goes where its layer is. Tests that start the built `molforma` binary stay with the UI ones, so that
  the core ones need it neither linked nor built: `test_mcp` launches it (`--mcp`, `--mcp-gui`, `--screenshot`),
  `test_execution` uses it as the supervisor of runs. Helpers that several executables use are in
  `tests/*_support.h` (`test_support.h`, and by area `nwchem_support.h`, `run_support.h`, `result_support.h`,
  `mcp_support.h`), not copied.
  All run with `QT_QPA_PLATFORM=offscreen` and need no network, credentials or NWChem install:
  `tests/fake_nwchem.cpp` builds a stand-in NWChem used by runs/mcp_server/execution/history/output/mcp tests,
  `tests/fake_ssh.cpp` a stand-in ssh and scp for the remote runs of `test_execution`, and
  `tests/fake_xtb.cpp` a stand-in xtb used by services/editor tests.
  Fixtures live in `tests/fixtures/` (`FIXTURE_DIR` in `test_core`).
  Tests must not touch the user's files: MolForma's settings use `mf::SettingsFormat` (`model/settings.h`: NativeFormat,
  an INI file on Windows and macOS; `mf::userPreferences()` there is the one factory of the store), so redirect
  `QSettings::setPath` for both NativeFormat and IniFormat and set
  `XDG_CONFIG_HOME` / `XDG_DATA_HOME` (honored on Windows and macOS too)
  (autosave copies) to temporary directories, also for launched `molforma` processes.
- New source files must be added to the explicit lists in `CMakeLists.txt` (no globbing).
- Format: `rg --files src tests -g '*.h' -g '*.cpp' -0 | xargs -0 clang-format -i` (uses `.clang-format`).
- Headless screenshot for UI checks:
  `QT_QPA_PLATFORM=offscreen ./build/molforma tests/fixtures/ala_gly_ser.molforma.json --screenshot /tmp/molforma.png`.
  `MOLFORMA_RENDERER=painter` forces the QPainter renderer.

## Architecture

Libraries (all in namespace `mf`; include headers relative to `src/`, e.g. `#include "model/document.h"`):

- `molforma_core` — an interface aggregate of `molforma_model` (`src/model`), `molforma_editing`
  (`src/editing`), `molforma_results` (`src/results`), `molforma_nwchem` (`src/nwchem`) and `molforma_io`
  (`src/io`: structure-file readers and writers, project files, downloads), over `molforma_project`
  (`src/project`: the atom references of project settings) and `molforma_foundation` (`src/foundation`: files,
  errors, translation, and `assets/resources.qrc` — fragment template/element data embedded as Qt resources; no
  runtime file dependency). Includes go one way (foundation/project < model < editing, results < nwchem < io <
  mcp < ui; `cmake/CheckLayers.cmake`). No UI code here: the model uses Qt Core alone, and Qt Gui comes in with
  editing's undo stack only.
- `molforma_ui` — `src/ui`, links core and mcp (McpGui reuses the shared tool definitions and policy).
  `main_window.*` hosts the editor, with `molecule_panel.*` + `molecule_properties.cpp` as its molecular
  workspace (the composer of the palette `part_palette.*`, the property pages on `property_page.*` —
  `transform_page.*`, `attributes_page.*`, `charge_page.*` — and the tool dialogs `molecule_dialogs.*`); the
  window's own dialogs are files of their own (`preferences_dialog.*`, `xtb_dialog.*`, and `help_dialogs.*` for
  the manual viewer and the shortcuts window), opened by thin members of the window. `editor_chrome.*` holds the
  header (workflow steps that switch the hidden tab bar), tool rail and 3D-view overlays; the rest of the chrome is
  one file per responsibility, included by what uses it: `theme.*` (every color and the style rules),
  `line_icons.*`, `chrome_layouts.*` (flow layout, form layout, toolbar fit), `settings_widgets.*` (cards and
  controls of the settings tabs), `calculation_summary.*` and `view_controls.*`. The numbered tabs are "1 分子" (the 3D
  view with the `coordinates_panel.*` table under it or in its place), "2 計算条件" (`calculation_panel.*`; each
  section opens its advanced overrides as "詳細設定"), "3 実行設定" (`execution_panel.*`, settings only, with the form `execution_form.*` that Preferences shares; the run
  state and log are its run panel below the tabs, a `QSplitter` with them so that it covers nothing, opened from the header run pill) and "4 入力プレビュー"
  (`input_preview_panel.*`); result viewer and history are panels too (`results_panel.*` composes the
  tabs that only show a `ResultData`, `result_*_view.*`, with `result_measurement.*` and
  `cube_combine_dialog.*`). Panels never see `MainWindow`: they get
  the capabilities of `ui/editor_context.h` that it implements — `EditorContext` (the document, its candidate
  and edits; not the `EditorSession`), `UiServices` (`safe`, preferences, messages, MCP's folders),
  `WorkspaceActions` (navigation only: `showPanel(QWidget *)`, `showSetting(SettingTarget, …)`, `showInputLine`,
  `refreshPanel`; no tab numbers, `workspaceTab` of `ui/setting_navigation.h` is the window's) and
  `ExecutionAccess` (the gate of a run as checks, not the `McpPolicy`). What one panel needs of another is a
  signal of the asking panel, connected by whoever composes the two (`MainWindow`, `WorkspaceController`,
  `MoleculePanel` for its coordinate table); `cmake/CheckLayers.cmake` forbids the includes that would bypass this.
  The parts of a panel (views, pages, palette, dialogs of their own files) get the same capabilities and their
  data, ask their panel with signals and never include a panel.
- `molforma_mcp` — `src/mcp`, links core only. A JSON-RPC MCP server over stdio (protocols 2026-07-28 and legacy
  2025-11-25); see `docs/mcp.md`. `server.cpp` holds the protocol, tasks, resources and dispatch of `McpServer`,
  `document_adapter.cpp` the document behind its tools and `document_tools.cpp` the tools themselves
  (`json_schema.*`: their schemas and validator; `prompts.*`: the prompt catalogue). `headless.*` adds NWChem runs
  and result loading to `--mcp`, `tools.*` holds
  definitions/result views shared with `McpGui`, `policy.*` holds the command-line access policy and
  `execution_trust.*` which values of the protected execution settings a run may use without asking.
- `molforma` executable (`src/main.cpp`) links all of them and has several modes selected by argv:
  normal GUI, `--screenshot`, `--mcp` (headless, `QCoreApplication`), `--mcp-gui` (MCP driving a real
  `MainWindow` via `src/ui/mcp_gui.*`), and the internal `--worker` / `--result-worker <in> <out>` /
  `--result-worker-ready` modes (test executables that load results handle them through `resultWorkerMain`)
  and `--run-job <job folder>` (`runJobMain`: the supervisor of one NWChem run, see below).

Key cross-cutting designs:

- **Document model**: `mf::Document` (`model/document.*`) holds atoms with stable IDs, bonds, residues,
  coordination centers, fragments, fixed atoms, isotopes, point charges and calculation/execution settings, and
  serializes to the `.molforma.json` project envelope (format `molforma-project`, version 1) as JSON
  (`project`/`fromProject`; the file itself is read and written by `saveProject`/`loadProject` of
  `io/document_storage.*`). The element table is `model/elements.*`, the XYZ dialect `model/xyz.cpp` and the
  bonds by distance `model/bond_inference.cpp`. Atom references are
  stored by stable ID and only
  translated to indices at export (NWChem input, XYZ). Coordinates are doubles in Å; camera/view never edits them.
  `Document::atom`/`index`/`neighbors`/`bond` scan the document: loops over many atoms (proteins of 10k+ atoms)
  look atoms up once through `Document::indices` or `Topology` (`model/topology.*`: ID → index and adjacency).
  `validateNwchem`, `generateNwchemInput`, `nwchemGeometryFragment` and `Document::validate` keep their last
  results (`model/memo.h`); documents compare cheaply while copies share their data.
- **Edit → candidate → commit**: UI edits operate on a copy of the document and commit only after validation.
  Operations such as ligand placement, sequence building, deletion and refinement produce a *candidate* shown
  separately until the user confirms ("変更を確定"); Esc discards it, Ctrl+Z undoes committed edits. The MCP server
  mirrors this: `McpEditor` hooks (`mcp/server.h`) expose `document`/`candidate`/`edit`/`history`, and without hooks
  the server owns its own headless document and undo stack. Fixed atoms must never move.
- **MCP access policy**: MCP must not choose the NWChem executable, launcher or environment overrides (only
  `--mcp-allow-execution-settings` permits it). `ExecutionTrust` (`mcp/execution_trust.*`, no widgets; the window
  holds one and tells it of every change) keeps the values the user entered in MolForma or confirmed before a run;
  runs started over MCP must use those or the ones saved in Preferences (`McpHeadless` asks one that stays empty).
  `--mcp-allow-dir` confines MCP file paths, including paths reached through GUI actions
  (`MainWindow::checkMcpPath`). Widgets with `mcpExecutionSetting` or `mcpProtected` are refused by
  `McpGui`; `mcpPath` widgets only while `--mcp-allow-dir` is set. Edits made or confirmed over MCP
  never make execution settings trusted, even on undo/redo. Panels do not hold the policy: they ask
  `ExecutionAccess` (`runCheck`, `mayChooseExecutionSettings`, `mayConfirmExecution`, `untrustedExecution`)
  and `UiServices` (`checkMcpPath` during a request, `outsideMcpFolders` for a widget's `mcpPath` mark).
- **Out-of-process work**: long chemistry operations (Open Babel refinement, SMILES → 3D) and heavy result reading
  re-launch the same executable with `--worker` / `--result-worker` via `QProcess`, exchanging JSON through temp
  files, so they are cancellable. Results returning after the document changed must be rejected as stale
  (compare `EditorSession::revision()`, which also backs the MCP `revision`; don't serialize documents for it).
  `ResultLoader` keeps the next result worker started (`--result-worker-ready`, given its paths on stdin) and a
  session cache folder: a CUBE/MOLDEN file read before is not parsed again for another dataset, orbital or
  isovalue (keys hold the file's stamp; an explicit reload starts a new cache).
- **NWChem pipeline**: `nwchem/nwchem.*` validates settings and generates full input; `nwchem_advanced.*` layers typed
  overrides (blank = default) on the base settings; `nwchem_output.*` adds CUBE/MOLDEN tasks;
  `execution_settings.*` checks the launch settings, `execution_plan.*` saves immutable run bundles, `job_script.*`
  writes `job.sh` and `nwchem_run.*` (with `run_process.*` and `run_supervisor.*`) controls NWChem/MPI processes
  (`nwchem_execution.h` includes them all; `remote_execution.h` likewise the `remote_*` files of runs on another
  computer); `history.*` scans those bundles. The Input preview
  tab and the MCP tools reuse the same generator and execution plan — don't fork generation logic in the UI.
- **Execution environment**: what Preferences keep machine-wide about running calculations (the execution settings
  of new documents, remote destinations, the preparation lines and header templates of job scripts) is read and
  saved only by `nwchem/execution_environment.*`. `validateExecution`, `executionPlan`, `executionPreviewPlan`,
  `executionJobScript`, `saveExecutionBundle`, `untrustedNetworkPath` and `remoteJobPreview` take it as an
  `ExecutionEnvironment` snapshot and read no preference themselves; the argument defaults to the loader
  `executionEnvironment()`, and runs, queue entries, saved jobs and the remote supervisor load one snapshot where
  they begin and pass it down. Don't add a preference read below them.
- **Runs that outlive the window**: `NwchemRun` either owns NWChem as a child process (tests, `--mcp`) or, with
  `setSupervisor` (interactive sessions, `--mcp-keep-runs`), starts the executable again as `--run-job <job>`:
  that supervisor owns NWChem, writes every state to `run.json` and stops on `stop.request` in the job folder,
  so a run goes on when the window closes and another session can `attach` to it (`nwchem/run_record.*` owns
  the job folder's file names, the reading and writing of `run.json`, what the end of a run records and the
  typed view `RunRecord` that UI and MCP read). `NwchemQueue` keeps waiting
  entries in `queue.json` of the data folder (restored paused; a job whose `run.json` changed is dropped).
  `RunSession` (`nwchem/run_session.*`, no widgets) holds the run and the queue of one session with what it
  leaves for the next (the supervisor, the stored queue and its lock, Preferences `execution/running`);
  `MainWindow` and `McpHeadless` each own one and check requests at their own gate, and the execution panel
  only shows the window's.
  A chained entry (`QueueChain`, `enqueueChain`) keeps its document until an earlier entry's job has finished,
  then takes that job's structure (`chainedDocument`: final, path maximum, IRC ends) and only then saves its bundle.
- **NEB**: `task` "neb" writes `geometry` + `geometry endgeom`, a `neb` block with `print_shift 1` and
  `task … neb`; the end is the setting `neb_end` (positions by atom ID, kept by `Document::remove`/`renameIds`).
  The results reader takes the last printed path into `properties["neb"]` and frames; an unconverged NEB is a
  warning, not an error.
- **External programs**: `editing/xtb.*` + `ChemistryJob` mode "xtb" run the user's `xtb` in a temp folder
  (fixed atoms via `$fix`), proposing the result as a candidate; `ui/figure_export.*` writes figures of the 3D
  view (`MoleculeView::renderImage`, VTK tiles), GIF (own encoder), MP4 (`ffmpeg` from PATH) and chart figures.
- **Restarts**: `nwchem/nwchem_restart.*` prepares a document that goes on from a job's NWChem database
  (`document.nwchem.restart`: `continue`, `hessian`, `reuse`); the generator then writes `restart`, no geometry
  and only the unfinished tasks, and `saveExecutionBundle` copies the database files into the new job. MCP
  cannot set `restart` directly (it names a folder to copy from). `history_import.cpp` registers runs made
  outside MolForma, and `executionJobScript` writes `job.sh` for running a saved job elsewhere.
- **Rendering**: `ui/molecule_view.*` owns camera, input and Qt annotations and has a QPainter fallback;
  `ui/vtk_molecule_renderer.*` is compiled only with VTK. Both draw bonds and isosurfaces by `ui/bond_style.h`. Offscreen/minimal Qt platforms or missing OpenGL 3.2
  automatically fall back to QPainter.
- Persistence: UI prefs and user presets go to the user's preferences, a `QSettings` that only
  `mf::userPreferences()` (`model/settings.h`: `SettingsFormat`, user scope, "MolForma"/"MolForma") constructs and
  whose keys are the constants `mf::Preference::…` of that header (never a literal; names and value formats are
  those of users' files and never change). `MainWindow` holds the store; widgets use
  `UiServices::preference`/`setPreference`, `DocumentStorage` gets it injected, and the nwchem layer reads it only in
  `execution_environment.*` and for `RunSession`'s remembered job. Molecule, calculation and
  execution settings go into the project file. Interactive sessions autosave unsaved work to
  `AppLocalDataLocation/autosave` (with a `QLockFile` per session) and offer recovery at startup.
- The manual (`docs/features.md`, an index of one page per screen in `docs/features/*.md` with their screenshots in
  `docs/features/images/`, `docs/mcp.md`, the recipes `docs/recipes.md` + `docs/recipes/*.md` with their
  screenshots in `docs/recipes/images/`) and the window icon are embedded via `docs/manual.qrc` in `molforma_ui`;
  the recipes' practice files in `docs/recipes/samples/` are embedded too, so that their links open them
  (`MainWindow::useSample`; `test_core recipeSamples` checks their contents).
  `packaging/` holds the desktop entry, icon and MIME types installed by `cmake --install`, and `packaging/arch/PKGBUILD`
  (an Arch Linux `molforma-git` package of GitHub's main, with RDKit and VTK; see `docs/installation/arch_linux.md`).
  The CPack settings at the end of `CMakeLists.txt` make a Debian/Ubuntu package with `cpack -G DEB` in the build
  folder (`dpkg-shlibdeps` finds the libraries; see `docs/installation/ubuntu.md`). `packaging/appimage/` builds an
  AppImage in Docker (Ubuntu 24.04 with Qt 6.4, VTK built for Qt 6; `build.sh` runs linuxdeploy and appimagetool and
  sets Open Babel's `BABEL_LIBDIR`/`BABEL_DATADIR` in an AppRun hook, leaving OpenSSL to the computer; `release.sh` makes the
  release ZIP and the tar of the sources (Ubuntu packages, VTK, runtime, AppRun); see `docs/installation/appimage.md`). That
  build is also the check that the code still builds and passes with Qt 6.4. On Windows MSVC builds find Open Babel
  through its CMake package (no pkg-config), keep preferences in an INI file (`model/settings.h`), and `cmake --install`
  makes a self-contained folder with windeployqt plus `packaging/windows/` (batch files that run NWChem in WSL; its
  `copy_runtime.cmake` also copies the DLLs beside each executable of the build folder after linking; `release.sh` makes
  the release ZIP and the tar of the sources, without Qt's OpenSSL plugin and the Microsoft DLLs windeployqt adds); see
  `docs/installation/windows.md`. On macOS (Homebrew), `cmake --install` / `cpack -G DragNDrop` make `MolForma.app`
  and a disk image (`packaging/macos/`: `Info.plist.in`, the icon, and `bundle.cmake`, which runs macdeployqt for Qt,
  copies Open Babel's plugins, the other libraries and their license files into the bundle, points every reference
  at the copies through `@rpath` and signs it ad hoc); `main()` points `BABEL_LIBDIR`/`BABEL_DATADIR` into the bundle
  and adds Homebrew's `bin` to `PATH`, and opens files sent by Finder; see `docs/installation/macos.md`. The build
  folder keeps a plain `build/molforma`. `packaging/macos/release.sh` makes the release ZIP (the disk image, licenses and
  list of sources) and the tar of the sources in `build-release`, with a Qt it builds without OpenSSL from Homebrew's
  sources and options: Homebrew's Qt links OpenSSL, whose license does not go with the GPL version 2.
- `docs/` is also the source of the GitHub Pages site https://hnakada123.github.io/MolForma/: on pushes to main that
  touch `docs/`, `.github/workflows/pages.yml` generates the HTML with Jekyll (`docs/Gemfile` + `Gemfile.lock`, the
  `github-pages` gem) and publishes that HTML (Pages source: GitHub Actions). Preview locally with
  `bundle exec jekyll build` (HTML in `docs/_site`) or `jekyll serve` in `docs/`, after `bundle config set --local
  path vendor/bundle && bundle install` (`.bundle/`, `vendor/` and `_site/` are ignored). Pages link relative to
  themselves (`root` from `_includes/language.html`; the layouts also convert the content's root-absolute links), so
  the generated HTML opens as files too. The
  site's sources: `_config.yml`, `_layouts/` (`default.html` for the manual pages, `home.html`), `_includes/` (head,
  header with navigation, X share button and language switch, footer), `assets/site.css`, and the home pages `index.html` / `en/index.html`
  (HTML with front matter, not in the app; their feature and recipe cards come from `_data/features.yml` and
  `_data/recipes.yml`, kept in step with the tables of `features.md` / `recipes.md` and their English versions).
  `_includes/head.html` sends browsers without Japanese from the Japanese pages to their English versions (never
  the other way, and not search engines), unless the language links chose a language (`remember_language` from
  `language.html`, kept in `localStorage`).
  The manual pages stay plain Markdown without front matter, since the app shows them verbatim; Jekyll plugins give
  them the layout and turn `.md` links into `.html`. Keep `{{` and `{%` out of `docs/` (`test_translations` checks
  the embedded pages).

## Conventions

- Add deterministic regression tests, especially for file compatibility, connectivity, geometry, undo and
  candidates. Preserve project envelope fields in `.molforma.json`.
- Document feature limitations in the page of the screen in `docs/features/` (Japanese; `docs/features.md` lists the
  pages); MCP tool changes belong in `docs/mcp.md`. Keep `README.en.md` in step with `README.md`, and the English manuals `docs/en/features*` / `docs/en/mcp.md`
  and the recipes `docs/en/recipes*` (shown by the English UI) in step; `test_translations` compares their headings
  and checks that every embedded page's links and images exist. When a UI change alters a recipe's steps or a screen
  shown in the feature pages, update those pages (and their screenshots) too. `tools/screenshots` (see its README)
  takes the screenshots by driving `molforma --mcp-gui`; Python is only a development tool there.
- **Interface language**: Japanese is the source language; English comes from `assets/translations/en.json`
  (`src/foundation/i18n.h`). Write user-visible text as `tr("日本語")` (QObject::tr in QObject classes, `mf::tr`
  elsewhere — same catalog), `QT_TR_NOOP("…")` in tables shown via `translated()`, `%1` templates rather than
  concatenation, and `tr("…%n…", nullptr, n)` with a `["one", "other"]` entry for counts. Core functions return
  translated text; don't cache translated text in statics (or keep one copy per `english()`). Element and
  palette names: `Element::name()`, `templateName()`. `test_translations` fails on unmarked Japanese literals in
  `src/` (`// i18n-exempt` for deliberate ones), missing/unused catalog entries and placeholder mismatches. Never
  translate NWChem input, identifiers or text that is persisted and read back. The language is chosen at startup
  (`MOLFORMA_LANGUAGE`, else Preferences `ui/language`, else the system locale; `ui/translations.*`) and passed to
  workers; tests install no translator, so they keep comparing Japanese.
- MolForma is GPL-2.0-or-later (Open Babel, which it links, is GPL-2.0-only; keep new dependencies
  GPLv2-compatible). New source files start with `// SPDX-License-Identifier: GPL-2.0-or-later`. Keep `LICENSE` and
  `NOTICE` current. Do not add runtime dependence on Python.
- Commits use short imperative subjects. PRs describe behavior and checks, with screenshots for UI changes.
  Keep build dirs, logs and generated outputs out of version control.
