# Repository Guidelines

## Structure

MolForma is a native Qt6 Widgets / C++20 molecular editor. `src/document.*`
owns the stable-ID document and KAGAMI v2 serialization; `geometry.*` owns
double-precision geometry. `library.*`, `operations.*`, `peptide.*` and
`coordination.*` implement editing. `chemistry.*` uses the Open Babel C++ API.
`stereochemistry.*` optionally uses native RDKit for CIP and residue recognition.
`molecule_view.*` renders with QPainter. `main_window.*` and `editor_panels.cpp`
provide the Qt UI, candidate edits and undo history. `template_delegate.*`
renders and caches the palette's molecular thumbnails.
`coordinates.*` implements coordinate edits and NWChem geometry fragments;
`coordinates_panel.*` provides the native "2 Coordinates" tab.
`nwchem.*` validates computational settings and generates complete NWChem input;
`calculation_panel.*` provides the native "3 Calculation conditions" tab.
`nwchem_advanced.*` defines typed NWChem overrides and their applicability;
`advanced_panel.*` provides the native "4 Advanced settings" tab.
`nwchem_execution.*` validates launch settings, saves immutable run bundles and
controls cancellable NWChem processes; `execution_panel.*` provides "5 Execution settings".
`input_preview_panel.*` provides the read-only "6 Input preview" tab using the
shared NWChem input generator and execution plan.
`results.*` reads NWChem logs, XYZ, CUBE and MOLDEN and generates bounded isosurfaces;
`results_panel.*` provides the independent result viewer with cancellable native readers.

Tests are in `tests/`, documentation in `docs/`, and resource data in `assets/`.
Preserve KAGAMI data and fixture attributions.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Dependencies: Qt6 Widgets/Test, Eigen3, Open Babel 3, CMake >= 3.21.
CTest uses `QT_QPA_PLATFORM=offscreen`. Tests require no credentials or network.
Format with `clang-format -i src/*.h src/*.cpp tests/*.cpp`.

## Conventions

Use `.clang-format`. Store physical coordinates as doubles in angstrom;
camera movement must not edit them. Store atom-dependent references using
stable IDs and translate them to indices at export. UI edits operate on a
copy and commit only after validation. Candidate edits stay separate until
confirmed. Fixed atoms must not move. Long chemistry operations run in a
cancellable process, and stale results must be rejected.

Add deterministic regression tests, especially for file compatibility,
connectivity, geometry, undo and candidates. Preserve project envelope fields.
Document limitations in `docs/features.md`.

## Changes and security

Use short imperative commit subjects. PRs describe behavior, checks and
screenshots for UI changes. Git metadata was unavailable at initial setup;
verify it before Git operations. Keep builds, logs, credentials and generated
outputs out of version control. Do not add runtime dependence on the original
KAGAMI checkout or Python GUI.
