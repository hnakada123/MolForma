# Installing on macOS

How to build MolForma from source on macOS and install it under your home folder. The libraries for the build and NWChem for calculations come from the package manager [Homebrew](https://brew.sh/).

These steps are for macOS 27 on Apple silicon Macs (arm64) and MolForma 1.0.0. They were checked on 2026-11-04 with macOS 27.0.1 and Homebrew 7.0.10 (see [Verified configuration](#verified-configuration)).

MolForma has no package in Homebrew (as of 2026-11-04). You build it from the source on GitHub and install it into `~/.local` with `cmake --install`. No macOS application bundle (`.app`) is made, so you start MolForma from Terminal (see [Limitations on macOS](#limitations-on-macos)).

## Required packages

The build needs the following (see [1. Install the Command Line Tools and Homebrew](#1-install-the-command-line-tools-and-homebrew) and [2. Install the packages](#2-install-the-packages)).

| Kind | Package | Used for |
| --- | --- | --- |
| Required | Xcode Command Line Tools | C++ compiler (Apple clang), `git`, the macOS SDK (with zlib) |
| Required | Homebrew's `cmake`, `ninja`, `pkgconf` | Building |
| Required | `qtbase` | User interface (Qt Widgets), tests (Qt Test) |
| Required | `eigen` | Coordinate and geometry calculations |
| Required | `open-babel` | Reading and writing structure files, force-field cleanup, 3D structures from SMILES (with its format plugins and force-field data) |
| Optional | `rdkit` | Strict stereo perception (R/S, E/Z), residue recognition |
| Optional | `vtk` | Drawing the 3D view with VTK (OpenGL). Homebrew's VTK is built with Qt 6 support |
| Optional | `qttranslations` | Qt's standard dialogs (such as file selection) in Japanese too, when the interface is Japanese |

The whole of Qt (`qt`) is not needed: MolForma uses only `qtbase`, and `qt` installs many modules such as Qt WebEngine. The built MolForma uses Homebrew's libraries (under `/opt/homebrew`) where they are.

For calculations, install Homebrew's `nwchem` (see [3. Install NWChem](#3-install-nwchem)).

- **RDKit and VTK**: Both are optional. They are enabled only when CMake finds them at build time.
- **NWChem**: Writing and saving input ("Save as job") works without NWChem.

## 1. Install the Command Line Tools and Homebrew

Install the Xcode Command Line Tools, which hold the C++ compiler and more. They are not needed if Xcode is installed.

```sh
xcode-select --install
```

If Homebrew is not installed, install it as described on [Homebrew's page](https://brew.sh/). Also make the setting shown at the end of its installation that adds `brew` to PATH (lines added to `~/.zprofile`). On Apple silicon Macs, Homebrew goes into `/opt/homebrew`.

## 2. Install the packages

Install the required packages.

```sh
brew install cmake ninja pkgconf qtbase eigen open-babel
```

To use RDKit, VTK and Qt's Japanese translations, then install the following. VTK depends on many packages (Qt Quick's `qtdeclarative`, HDF5, NetCDF and others), and VTK alone takes about 300 MB.

```sh
brew install rdkit vtk qttranslations
```

## 3. Install NWChem

```sh
brew install nwchem
```

This also installs Open MPI (`open-mpi`), ScaLAPACK and OpenBLAS, GNU Fortran (`gcc`) and more as dependencies. NWChem takes about 1 GB. NWChem goes to `/opt/homebrew/bin/nwchem` and the MPI launcher `mpirun` to `/opt/homebrew/bin/mpirun`. Homebrew's NWChem reads `/opt/homebrew/etc/nwchemrc`, which gives the location of the basis set and other libraries, so `~/.nwchemrc` and `NWCHEM_BASIS_LIBRARY` need not be set. MolForma's "3 Run settings" defaults (executable `nwchem`, start with MPI, MPI launcher `mpirun`, 2 processes) start this NWChem as they are.

To check that NWChem works on its own, save the following input as `h2.nw` in an empty folder and run it (optional).

```
start h2
geometry
  H 0 0 0
  H 0 0 0.74
end
basis
  * library 6-31g*
end
task scf energy
```

```sh
mpirun -np 2 nwchem h2.nw > h2.out
grep "Total SCF energy" h2.out
```

`Total SCF energy =     -1.126755317965` means it works.

## 4. Get the source and build

Get the source and build it in Release mode. Keep the source folder and `build`, which updating uses.

```sh
git clone https://github.com/hnakada123/MolForma.git
cd MolForma
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
```

When you run the third line, `cmake -S . -B build …` (configuring: CMake looks for the libraries it needs and prepares the build), the terminal shows a line starting with `-- MolForma rendering:`. This line tells how the 3D view is drawn. Near the end, the output looks like this, for example:

```
--   Found openbabel-3, version 3.2.1
-- MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback
-- Configuring done (2.1s)
-- Generating done (1.1s)
-- Build files have been written to: …/MolForma/build
```

| Output | Meaning |
| --- | --- |
| `MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback` | Draws with VTK (QPainter where OpenGL is not available) |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` | Does not use VTK. Check that `vtk` is installed |

After installing missing packages, start again from the configure command. `-j 4` is the number of compilations run at the same time. Lower it if memory runs short.

To run the tests (optional): they use neither NWChem nor the network, and keep the application settings and autosaves in temporary folders, so your own settings do not change. If you do not need the tests, adding `-DBUILD_TESTING=OFF` to the configure command shortens the build.

```sh
ctest --test-dir build --output-on-failure
```

## 5. Install

Install into `~/.local` and add `~/.local/bin` to PATH. The PATH setting is needed only once and takes effect in newly opened terminals.

```sh
cmake --install build --prefix ~/.local
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zprofile
```

These files are installed:

| Location | Contents |
| --- | --- |
| `~/.local/bin/molforma` | The executable |
| `~/.local/share/molforma/` | LICENSE, NOTICE, README and the manual's Markdown (the manual is also built into the executable) |
| `~/.local/share/applications/`, `~/.local/share/icons/`, `~/.local/share/mime/` | Menu entry, icons and file types for Linux desktops. Not used on macOS |

You can also start `build/molforma` in the build folder without installing it.

## 6. Start and check

Run `molforma` in Terminal. You can also give a file to open (for example `molforma water.molforma.json`). The interface language follows the system language ("Language (after restart)" in Preferences (⌘,) switches it; it takes effect after a restart). On macOS, the "Preferences…" and "About MolForma" items may move to the "MolForma" menu of the menu bar, depending on the interface language.

1. Open "About MolForma" (in the "Help" menu or the "MolForma" menu) and check that the version is "MolForma 1.0.0" and, if you installed RDKit and VTK, that it shows "Stereochemistry and residue recognition (RDKit): Enabled" and "3D rendering: VTK (OpenGL)".
2. Open "3 Run settings" and check that `/opt/homebrew/bin/nwchem` and `/opt/homebrew/bin/mpirun`, found through PATH, are shown under the "NWChem executable" and "MPI launcher" fields. If they are not, see [Troubleshooting](#troubleshooting).
3. Try a calculation on a small molecule. The steps are in [recipe 1 Building from atoms and bonds](../recipes/atoms_and_bonds.md).

## Limitations on macOS

- **No application bundle**: As no `.app` is made, MolForma cannot be started from Finder, Launchpad or the Dock, and double-clicking a `.molforma.json` does not open it. Start it with `molforma` from Terminal.
- **Depends on Homebrew's libraries**: The built MolForma uses Qt, Open Babel, RDKit and VTK in `/opt/homebrew`. A copy on another Mac does not run.

## Updating and removing

### Updating

Update the source, build again and install again.

```sh
cd MolForma
git pull --ff-only
cmake --build build -j 4
cmake --install build --prefix ~/.local
```

When `brew upgrade` brings new versions of Qt, Open Babel, RDKit or VTK, their library file names change and MolForma may no longer start (it shows `dyld[…]: Library not loaded`). In that case, delete `build`, start again from configuring in [4. Get the source and build](#4-get-the-source-and-build) and install again.

### Removing

Delete the installed files.

```sh
rm ~/.local/bin/molforma
rm -r ~/.local/share/molforma
rm ~/.local/share/applications/molforma.desktop ~/.local/share/icons/hicolor/scalable/apps/molforma.svg
rm ~/.local/share/icons/hicolor/256x256/apps/molforma.png ~/.local/share/mime/packages/molforma.xml
```

The application settings (`~/Library/Preferences/MolForma/MolForma.ini`), autosaves (`~/Library/Application Support/MolForma/`) and calculation jobs (by default `~/MolForma/jobs`) remain. Delete them by hand if you no longer need them. Packages installed with Homebrew are removed with `brew uninstall` followed by their names, and `brew autoremove` removes what came with them. NWChem is removed with `brew uninstall nwchem` followed by `brew autoremove`.

## Troubleshooting

| Symptom | Check and fix |
| --- | --- |
| Configuring stops with `No CMAKE_CXX_COMPILER could be found` or similar | Install the Command Line Tools (`xcode-select --install`). They are installed if `xcode-select -p` prints a location |
| Configuring stops with errors that `Qt6`, `Eigen3` or `openbabel-3` is not found | Install the required packages of [2. Install the packages](#2-install-the-packages) and start again from configuring |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` appears | Install it with `brew install vtk` and start again from configuring |
| `molforma: command not found` appears | `~/.local/bin` is not in PATH. Make the PATH setting of [5. Install](#5-install) and open Terminal again |
| Starting shows `dyld[…]: Library not loaded` | Homebrew's library versions changed. As in [Updating](#updating), start again from configuring and install again |
| "Pre-run check" cannot find NWChem or the MPI launcher | Check `command -v nwchem mpirun` in Terminal. Make the setting that adds `brew` to PATH (`~/.zprofile`), then open the terminal from which you start MolForma again. If they are still not found, set `/opt/homebrew/bin/nwchem` and `/opt/homebrew/bin/mpirun` in "3 Run settings" |
| An NWChem calculation stops with an error that the basis set library is not found | Check that `/opt/homebrew/etc/nwchemrc` exists. If it does not, reinstall with `brew reinstall nwchem` |
| Qt's standard dialogs such as file selection keep English text although the interface is Japanese | Install `qttranslations` with `brew install qttranslations` |
| The build runs out of memory | Lower the parallelism, for example `cmake --build build -j 2` |

## Verified configuration

On 2026-11-04, on an Apple silicon Mac (macOS 27.0.1, arm64, 10 cores, 32 GB memory), the following was checked:

- With the packages above installed, the source configures and builds (with RDKit and VTK both enabled). The build took about a minute with `-j 8`, without warnings.
- The `molforma` installed with `cmake --install` starts and takes a picture (`--screenshot`) of the window with a project file open, and keeps its settings in `~/Library/Preferences/MolForma/MolForma.ini`.
- Without `~/.nwchemrc`, `mpirun -np 2 nwchem` completes an HF/6-31G* calculation of the hydrogen molecule with an SCF energy of −1.126755317965 Hartree. MolForma's run settings test (the optional test with a real NWChem, see ["3 Run settings"](../features/execution.md)) completes a water HF/STO-3G calculation with 2 MPI processes, with an SCF energy of −74.945628203255 Hartree.
- All 16 CTest tests pass (in parallel), and MolForma's settings and autosaves in `~/Library/Preferences` and `~/Library/Application Support` stay unchanged while they run.

| Item | Version |
| --- | --- |
| Qt (`qtbase`) | 6.12.0 |
| Open Babel | 3.2.1 |
| Eigen | 5.0.1 |
| RDKit, Boost | 2026.09.1, 1.92.0 |
| VTK | 9.7.1 |
| Apple clang, CMake, Ninja | 21.0.0, 4.4.4, 1.13.2 |
| NWChem | 7.3.1 |
| Open MPI | 5.0.11 |

Intel Macs (where Homebrew goes into `/usr/local`), macOS 26 and earlier, and long interactive use have not been checked.

Related: [Installation and required packages by OS](../installation.md), [3D rendering](../features/rendering.md), ["3 Run settings"](../features/execution.md)
