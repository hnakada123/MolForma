# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

MolForma is a native Qt6 Widgets / C++20 molecular editor and NWChem input/run/result front end.
The UI text and user docs (`README.md`, `docs/`) are Japanese, with an English UI and manual (`docs/en/`).
`AGENTS.md` has the same repository guidelines in a per-file form.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^editor$' --output-on-failure   # one test executable
QT_QPA_PLATFORM=offscreen ./build/test_editor someTestFunction   # one Qt Test slot
```

- Dependencies: Qt6 (>= 6.4) Widgets/Test, Eigen3, Open Babel 3 (pkg-config `openbabel-3`, C++ API only),
  CMake >= 3.21. Optional: RDKit C++ (defines `MOLFORMA_HAS_RDKIT`, enables strict CIP/residue recognition)
  and Qt6-built VTK >= 9.1 (defines `MOLFORMA_HAS_VTK`). Code must build and pass tests with neither.
- Alternate trees exist locally: `build-no-vtk` (`-DMOLFORMA_ENABLE_VTK=OFF`) and `build-asan`
  (`-DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer'`). Check rendering changes against the
  QPainter path too.
- Each `tests/test_*.cpp` is its own executable/CTest test (`core`, `editor`, `coordinates`, `calculation`,
  `advanced`, `execution`, `input_preview`, `results`, `history`, `output`, `molden`, `rendering`, `translations`, `mcp`).
  All run with `QT_QPA_PLATFORM=offscreen` and need no network, credentials or NWChem install:
  `tests/fake_nwchem.cpp` builds a stand-in NWChem used by execution/history/output/mcp tests.
  `test_mcp` launches the built `molforma` binary. Fixtures live in `tests/fixtures/` (`FIXTURE_DIR` in `test_core`).
  Tests must not touch the user's files: MolForma's settings use `mf::SettingsFormat` (`model/settings.h`: NativeFormat,
  an INI file on Windows), so redirect `QSettings::setPath` for both NativeFormat and IniFormat and set
  `XDG_CONFIG_HOME` / `XDG_DATA_HOME` (honored on Windows too)
  (autosave copies) to temporary directories, also for launched `molforma` processes.
- New source files must be added to the explicit lists in `CMakeLists.txt` (no globbing).
- Format: `rg --files src tests -g '*.h' -g '*.cpp' -0 | xargs -0 clang-format -i` (uses `.clang-format`).
- Headless screenshot for UI checks:
  `QT_QPA_PLATFORM=offscreen ./build/molforma tests/fixtures/ala_gly_ser.molforma.json --screenshot /tmp/molforma.png`.
  `MOLFORMA_RENDERER=painter` forces the QPainter renderer.

## Architecture

Libraries (all in namespace `mf`; include headers relative to `src/`, e.g. `#include "model/document.h"`):

- `molforma_core` — `src/model`, `src/editing`, `src/nwchem`, `src/results`, plus `assets/resources.qrc`
  (fragment template/element data embedded as Qt resources; no runtime file dependency). No UI code here.
- `molforma_ui` — `src/ui`, links core and mcp (McpGui reuses the shared tool definitions and policy).
  `main_window.*` + `editor_panels.cpp` host the editor; `editor_chrome.*` holds the header (workflow steps that
  switch the hidden tab bar), tool rail, flow layout and 3D-view overlays. The numbered tabs are "1 分子" (the 3D
  view with the `coordinates_panel.*` table under it or in its place), "2 計算条件" (`calculation_panel.*`; each
  section opens its advanced overrides as "詳細設定"), "3 実行設定" (`execution_panel.*`, settings only; the run
  state and log are its run panel over the tabs, opened from the header run pill) and "4 入力プレビュー"
  (`input_preview_panel.*`); result viewer and history are panels too.
- `molforma_mcp` — `src/mcp`, links core only. A JSON-RPC MCP server over stdio (protocols 2026-07-28 and legacy
  2025-11-25); see `docs/mcp.md`. `headless.*` adds NWChem runs and result loading to `--mcp`, `tools.*` holds
  definitions/result views shared with `McpGui`, and `policy.*` holds the command-line access policy.
- `molforma` executable (`src/main.cpp`) links all of them and has several modes selected by argv:
  normal GUI, `--screenshot`, `--mcp` (headless, `QCoreApplication`), `--mcp-gui` (MCP driving a real
  `MainWindow` via `src/ui/mcp_gui.*`), and the internal `--worker` / `--result-worker <in> <out>` modes.

Key cross-cutting designs:

- **Document model**: `mf::Document` (`model/document.*`) holds atoms with stable IDs, bonds, residues,
  coordination centers, fragments, fixed atoms, isotopes, point charges and calculation/execution settings, and
  serializes to the `.molforma.json` project envelope (format `molforma-project`, version 1). Atom references are
  stored by stable ID and only
  translated to indices at export (NWChem input, XYZ). Coordinates are doubles in Å; camera/view never edits them.
- **Edit → candidate → commit**: UI edits operate on a copy of the document and commit only after validation.
  Operations such as ligand placement, sequence building, deletion and refinement produce a *candidate* shown
  separately until the user confirms ("変更を確定"); Esc discards it, Ctrl+Z undoes committed edits. The MCP server
  mirrors this: `McpEditor` hooks (`mcp/server.h`) expose `document`/`candidate`/`edit`/`history`, and without hooks
  the server owns its own headless document and undo stack. Fixed atoms must never move.
- **MCP access policy**: MCP must not choose the NWChem executable, launcher or environment overrides (only
  `--mcp-allow-execution-settings` permits it). `MainWindow::mcp` records the values last changed outside MCP;
  runs started over MCP must match them. `--mcp-allow-dir` confines MCP file paths, including paths reached through GUI actions
  (`MainWindow::checkMcpPath`). Widgets with `mcpExecutionSetting` or `mcpProtected` are refused by
  `McpGui`; `mcpPath` widgets only while `--mcp-allow-dir` is set. Edits made or confirmed over MCP
  never make execution settings trusted, even on undo/redo.
- **Out-of-process work**: long chemistry operations (Open Babel refinement, SMILES → 3D) and heavy result reading
  re-launch the same executable with `--worker` / `--result-worker` via `QProcess`, exchanging JSON through temp
  files, so they are cancellable. Results returning after the document changed must be rejected as stale.
- **NWChem pipeline**: `nwchem/nwchem.*` validates settings and generates full input; `nwchem_advanced.*` layers typed
  overrides (blank = default) on the base settings; `nwchem_output.*` adds CUBE/MOLDEN tasks; `nwchem_execution.*`
  saves immutable run bundles and controls NWChem/MPI processes; `history.*` scans those bundles. The Input preview
  tab and the MCP tools reuse the same generator and execution plan — don't fork generation logic in the UI.
- **Rendering**: `ui/molecule_view.*` owns camera, input and Qt annotations and has a QPainter fallback;
  `ui/vtk_molecule_renderer.*` is compiled only with VTK. Offscreen/minimal Qt platforms or missing OpenGL 3.2
  automatically fall back to QPainter.
- Persistence: UI prefs and user presets go to `QSettings("MolForma", "MolForma")`; molecule, calculation and
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
  sets Open Babel's `BABEL_LIBDIR`/`BABEL_DATADIR` in an AppRun hook; see `docs/installation/appimage.md`). That
  build is also the check that the code still builds and passes with Qt 6.4. On Windows MSVC builds find Open Babel
  through its CMake package (no pkg-config), keep preferences in an INI file (`model/settings.h`), and `cmake --install`
  makes a self-contained folder with windeployqt plus `packaging/windows/` (batch files that run NWChem in WSL; its
  `copy_runtime.cmake` also copies the DLLs beside each executable of the build folder after linking); see
  `docs/installation/windows.md`.
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
  The manual pages stay plain Markdown without front matter, since the app shows them verbatim; Jekyll plugins give
  them the layout and turn `.md` links into `.html`. Keep `{{` and `{%` out of `docs/` (`test_translations` checks
  the embedded pages).

## Conventions

- Add deterministic regression tests, especially for file compatibility, connectivity, geometry, undo and
  candidates. Preserve project envelope fields in `.molforma.json`.
- Document feature limitations in the page of the screen in `docs/features/` (Japanese; `docs/features.md` lists the
  pages); MCP tool changes belong in `docs/mcp.md`. Keep the English manuals `docs/en/features*` / `docs/en/mcp.md`
  and the recipes `docs/en/recipes*` (shown by the English UI) in step; `test_translations` compares their headings
  and checks that every embedded page's links and images exist. When a UI change alters a recipe's steps or a screen
  shown in the feature pages, update those pages (and their screenshots) too. `tools/screenshots` (see its README)
  takes the screenshots by driving `molforma --mcp-gui`; Python is only a development tool there.
- **Interface language**: Japanese is the source language; English comes from `assets/translations/en.json`
  (`src/model/i18n.h`). Write user-visible text as `tr("日本語")` (QObject::tr in QObject classes, `mf::tr`
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
