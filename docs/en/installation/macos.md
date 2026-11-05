# Installing on macOS

How to build MolForma from source on macOS, make a disk image (DMG) of the application (`MolForma.app`) and install it into the Applications folder. The libraries for the build and NWChem for calculations come from the package manager [Homebrew](https://brew.sh/).

These steps are for macOS 27 on Apple silicon Macs (arm64) and MolForma 1.0.0. They were checked on 2026-11-04 with macOS 27.0.1 and Homebrew 7.0.10 (see [Verified configuration](#verified-configuration)).

MolForma has no package in Homebrew (as of 2026-11-04). You build it from the source on GitHub and make a DMG (`MolForma-1.0.0-arm64.dmg`) with CPack, which comes with CMake. `MolForma.app` holds all the libraries it uses (Qt, Open Babel, RDKit, VTK and others) and loads no library from Homebrew's folders. Once installed, it keeps working when Homebrew's libraries are upgraded or removed. NWChem is not included; install it separately.

A ready-built DMG is in the ZIP on [Releases](https://github.com/hnakada123/MolForma/releases) (`MolForma-<version>-macos-arm64.zip`). On an Apple silicon Mac with macOS 27 or later, unpack the ZIP and go to [6. Install](#6-install). The check when opening it the first time is described in [Using it on other Macs and distributing it](#using-it-on-other-macs-and-distributing-it). For calculations, also do [3. Install NWChem](#3-install-nwchem). To make it yourself, follow the steps below.

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

The whole of Qt (`qt`) is not needed: MolForma uses only `qtbase`, and `qt` installs many modules such as Qt WebEngine. These libraries are used for building and for making the DMG, which copies them into `MolForma.app`.

For calculations, install Homebrew's `nwchem` (see [3. Install NWChem](#3-install-nwchem)).

- **RDKit and VTK**: Both are optional. They are enabled only when CMake finds them at build time, and then go into the DMG too. With `qttranslations` installed, Qt's Japanese translation goes into the DMG as well.
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

This also installs Open MPI (`open-mpi`), ScaLAPACK and OpenBLAS, GNU Fortran (`gcc`) and more as dependencies. NWChem takes about 1 GB. NWChem goes to `/opt/homebrew/bin/nwchem` and the MPI launcher `mpirun` to `/opt/homebrew/bin/mpirun`. Homebrew's NWChem reads `/opt/homebrew/etc/nwchemrc`, which gives the location of the basis set and other libraries, so `~/.nwchemrc` and `NWCHEM_BASIS_LIBRARY` need not be set. MolForma's "3 Run settings" defaults (executable `nwchem`, start with MPI, MPI launcher `mpirun`, 2 processes) start this NWChem as they are. Apps started from Finder or the Dock do not get the PATH set for Terminal, but `MolForma.app` adds `/opt/homebrew/bin` (`/usr/local/bin` on Intel Macs) to PATH to find NWChem and the MPI launcher.

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

## 5. Make the DMG

Run `cpack` in `build` to make the DMG.

```sh
cd build
cpack -G DragNDrop
```

In a minute or two, `build` holds `MolForma-1.0.0-arm64.dmg` (about 85 MB). CPack does the following, then puts `MolForma.app` and a link to the Applications folder into the DMG:

1. It assembles `MolForma.app`: the executable, the icon, `Info.plist` (with the file types and more), Open Babel's format plugins and force-field data, and Qt's Japanese translation.
2. Qt's `macdeployqt` copies Qt's frameworks and plugins.
3. `packaging/macos/bundle.cmake` copies the other libraries (Open Babel, RDKit, Boost, VTK and others) and points every reference at the copies in the app. Nothing that refers to Homebrew's folders remains.
4. It puts the license texts of the included libraries and a list of their packages, versions and licenses into `Contents/Resources/licenses` (see [Using it on other Macs and distributing it](#using-it-on-other-macs-and-distributing-it)).
5. It signs the app ad hoc (a signature without a Developer ID), as Apple silicon Macs do not run unsigned programs.

To make only `MolForma.app` without a DMG, run `cmake --install build --prefix <folder>`; that makes `<folder>/MolForma.app`.

## 6. Install

Open the DMG and drag `MolForma` to the Applications folder. The DMG also opens from Terminal:

```sh
open MolForma-1.0.0-arm64.dmg
```

When copying is done, eject the DMG with the eject button beside "MolForma" in Finder's sidebar. You can delete the DMG unless you want it for other Macs.

To start MolForma from Terminal or an MCP client, give `/Applications/MolForma.app/Contents/MacOS/molforma` (for example `/Applications/MolForma.app/Contents/MacOS/molforma water.molforma.json`; for MCP see [MCP](../mcp.md)). To start it by the name `molforma`, make a link in a folder in PATH:

```sh
mkdir -p ~/.local/bin
ln -sf /Applications/MolForma.app/Contents/MacOS/molforma ~/.local/bin/molforma
```

If `~/.local/bin` is not in PATH, add it with `echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zprofile` and open Terminal again.

## 7. Start and check

Start "MolForma" from Launchpad or the Applications folder. The interface language follows the system language ("Language (after restart)" in Preferences (⌘,) switches it; it takes effect after a restart). On macOS, the "Preferences…" and "About MolForma" items may move to the "MolForma" menu of the menu bar, depending on the interface language.

1. Open "About MolForma" (in the "Help" menu or the "MolForma" menu) and check that the version is "MolForma 1.0.0" and, if you installed RDKit and VTK, that it shows "Stereochemistry and residue recognition (RDKit): Enabled" and "3D rendering: VTK (OpenGL)".
2. Open "3 Run settings" and check that `/opt/homebrew/bin/nwchem` and `/opt/homebrew/bin/mpirun`, found through PATH, are shown under the "NWChem executable" and "MPI launcher" fields. If they are not, see [Troubleshooting](#troubleshooting).
3. Try a calculation on a small molecule. The steps are in [recipe 1 Building from atoms and bonds](../recipes/atoms_and_bonds.md).

From Finder, open a file by right-clicking it and choosing "Open With → MolForma", or by dragging it onto MolForma's icon in the Dock. NWChem input (`.nw`) opens with a double-click unless another app claims it. Structure files (XYZ, MOL, SDF, Mol2, CML, PDB, mmCIF, Gaussian input) and projects (`.molforma.json`) are offered under "Open With". As projects end in `.json`, a double-click opens them in the default app for JSON.

## Using it on other Macs and distributing it

The DMG works on Macs of the same kind (Apple silicon) with the same or a newer macOS than the Mac that made it. As Homebrew's libraries are built for the macOS of that Mac, the minimum macOS version in `Info.plist` is also the macOS version of the Mac that made the DMG (here macOS 27). Install NWChem on the Mac that uses it, for calculations (see [3. Install NWChem](#3-install-nwchem)).

- **Opening it the first time**: As the app is not signed with a Developer ID nor notarized, the app of a DMG received over the internet or AirDrop shows a message such as "“MolForma” can't be opened" the first time. Press "Open Anyway" under "System Settings → Privacy & Security", then open it again. Running `xattr -dr com.apple.quarantine /Applications/MolForma.app` in Terminal after installing it also lets it open. A DMG made on your own Mac opens without this check.
- **Licenses**: To give the DMG to others, follow the licenses of the software it holds. `MolForma.app/Contents/Resources/licenses` holds the license texts of each package (where the Homebrew package has them) and `packages.txt`, a list of the package names, versions and licenses. For MolForma (GPL-2.0-or-later), Open Babel (GPL-2.0-only) and Qt, which is used under GPL-2.0 because it is linked with Open Babel, make the corresponding source available when you distribute the binaries (see `NOTICE` in the repository). Point to the source of the commit the DMG was made from for MolForma, and to the source of the versions in `packages.txt` for the other packages.
- **OpenSSL**: Homebrew's Qt links OpenSSL (Apache-2.0), so the DMG of [5. Make the DMG](#5-make-the-dmg) holds OpenSSL too. As OpenSSL's license does not go with GPL-2.0, make a DMG for others as in [Making the release files](#making-the-release-files).

## Making the release files

The files for [Releases](https://github.com/hnakada123/MolForma/releases) are made in the source folder as follows. Of the packages above, RDKit, VTK and `qttranslations` are required for this.

```sh
packaging/macos/release.sh
```

The script does the following. The first run, with the build of Qt, takes about 20 minutes and downloads a few hundred MB of sources. Finished stages are not repeated; delete `build-release` to start again.

1. Builds the source and patches of the installed versions of `qtbase` and `qtsvg` with Homebrew's options, without OpenSSL. Qt's TLS is then macOS's SecureTransport (up to TLS 1.2). Vulkan is off too (`molten-vk`, which Homebrew's build uses, is not needed).
2. Builds MolForma with that Qt and runs the tests.
3. Downloads the source of the installed versions of the Homebrew packages in the app. Where they are comes from each package's `sbom.spdx.json` and its formula as installed (`.brew/*.rb`). For packages without license texts in Homebrew's folders, it takes the texts from the source and puts them into the app.
4. Makes the DMG and checks that it holds nothing of OpenSSL, that every Qt module is the one built here, and that no program or library links anything outside the app.
5. Makes these two files in `build-release/`.

| File | What it is |
| --- | --- |
| `MolForma-<version>-macos-arm64.zip` | The DMG, `README.txt`, `LICENSE`, `NOTICE`, `SOURCES.txt` (where the source is), `licenses/` (the licenses of the bundled libraries) |
| `MolForma-<version>-macos-third-party-sources.tar` | The source of the packages in the DMG, with their patches and Homebrew's formulae (about 570 MB) |

`SOURCES.txt` points to the source of the commit it was made from and to the release tag `v<version>`. As the source must be public, the script runs only on a pushed commit without changes. For a trial, use `DRAFT=1 packaging/macos/release.sh`.

## Updating and removing

### Updating

Update the source, build again and make the DMG again.

```sh
cd MolForma
git pull --ff-only
cmake --build build -j 4
cd build
cpack -G DragNDrop
```

Quit MolForma, drag `MolForma` of the new DMG to the Applications folder and choose "Replace".

The installed `MolForma.app` keeps working when `brew upgrade` brings new versions of Homebrew's libraries. The build folder `build` may fail to build or to make the DMG when the versions of Qt, Open Babel, RDKit or VTK change (showing `dyld[…]: Library not loaded` or similar). In that case, delete `build` and start again from configuring in [4. Get the source and build](#4-get-the-source-and-build).

### Removing

Move `MolForma` in the Applications folder to the Trash. If you made the link in [6. Install](#6-install), delete it with `rm ~/.local/bin/molforma`.

The application settings (`~/Library/Preferences/MolForma/MolForma.ini`), autosaves (`~/Library/Application Support/MolForma/`) and calculation jobs (by default `~/MolForma/jobs`) remain. Delete them by hand if you no longer need them. Packages installed with Homebrew are removed with `brew uninstall` followed by their names, and `brew autoremove` removes what came with them. Once the DMG is made, `MolForma.app` keeps working when Homebrew's packages other than NWChem are removed. NWChem is removed with `brew uninstall nwchem` followed by `brew autoremove`.

## Troubleshooting

| Symptom | Check and fix |
| --- | --- |
| Configuring stops with `No CMAKE_CXX_COMPILER could be found` or similar | Install the Command Line Tools (`xcode-select --install`). They are installed if `xcode-select -p` prints a location |
| Configuring stops with errors that `Qt6`, `Eigen3` or `openbabel-3` is not found | Install the required packages of [2. Install the packages](#2-install-the-packages) and start again from configuring |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` appears | Install it with `brew install vtk` and start again from configuring |
| `cpack` stops with `… not found in …` | A library to put into the app is not found in Homebrew's folders. After `brew upgrade`, delete `build` and start again from configuring |
| Opening it shows "“MolForma” can't be opened", "the developer cannot be verified" or similar | It is the app of a DMG you received. As in [Using it on other Macs and distributing it](#using-it-on-other-macs-and-distributing-it), press "Open Anyway" under "Privacy & Security", or run `xattr -dr com.apple.quarantine /Applications/MolForma.app` |
| `release.sh` stops with `wrong SHA-256` | A downloaded file differs from Homebrew's record. It may have been altered on the way, so find the cause before going on |
| "“MolForma” is damaged and can't be opened" appears | Its signature is broken. Make the DMG again and install it again. Changing files inside the app also leads to this message |
| `molforma: command not found` appears | Make the link and the PATH setting of [6. Install](#6-install) and open Terminal again |
| "Pre-run check" cannot find NWChem or the MPI launcher | Check `command -v nwchem mpirun` in Terminal. If NWChem is installed elsewhere than `/opt/homebrew/bin` or `/usr/local/bin`, set its path in "3 Run settings" |
| An NWChem calculation stops with an error that the basis set library is not found | Check that `/opt/homebrew/etc/nwchemrc` exists. If it does not, reinstall with `brew reinstall nwchem` |
| Qt's standard dialogs such as file selection keep English text although the interface is Japanese | Install `qttranslations` with `brew install qttranslations` and make the DMG again |
| The build runs out of memory | Lower the parallelism, for example `cmake --build build -j 2` |

## Verified configuration

On 2026-11-04 and 05, on an Apple silicon Mac (macOS 27.0.1, arm64, 10 cores, 32 GB memory), the following was checked:

- With the packages above installed, the source configures and builds (with RDKit and VTK both enabled). The build took about a minute with `-j 8`, without warnings. All 16 CTest tests pass, and MolForma's settings and autosaves in `~/Library/Preferences` and `~/Library/Application Support` stay unchanged while they run.
- `cpack -G DragNDrop` makes `MolForma-1.0.0-arm64.dmg` (84 MB) in about a minute and a half. The DMG holds `MolForma.app` (205 MB, with the libraries of 38 Homebrew packages) and a link to the Applications folder, and `codesign --verify --deep --strict` passes.
- The `MolForma.app` copied from the DMG loads no library from Homebrew's folders (`DYLD_PRINT_LIBRARIES`), takes a picture (`--screenshot`) of the window with a project file open, uses the Open Babel plugins in the app for a 3D structure from SMILES, and reads a file opened from Finder (`open -a`).
- Started with the minimal PATH that apps started from Finder or the Dock get (`/usr/bin:/bin:/usr/sbin:/sbin`), `MolForma.app` finds NWChem and `mpirun` in `/opt/homebrew/bin` with the default run settings and completes a calculation of the hydrogen molecule with 2 MPI processes.
- Without `~/.nwchemrc`, `mpirun -np 2 nwchem` completes an HF/6-31G* calculation of the hydrogen molecule with an SCF energy of −1.126755317965 Hartree. MolForma's run settings test (the optional test with a real NWChem, see ["3 Run settings"](../features/execution.md)) completes a water HF/STO-3G calculation with 2 MPI processes, with an SCF energy of −74.945628203255 Hartree.
- On 2026-11-05, the DMG made by `packaging/macos/release.sh` (84 MB) was checked as follows. Every Qt module is the one the script built, and there are no OpenSSL files. All 16 CTest tests pass with that Qt. In a sandbox (`sandbox-exec`) that cannot read `/opt/homebrew` or `/usr/local`, it starts and loads only libraries from the app and from macOS. In the same sandbox, a 3D structure from SMILES and a download from the PDB (1CRN) work (checked on a copy with an added offscreen display plugin, to operate it). With the minimal PATH, an SCF calculation of water with 2 MPI processes completes. `xattr -dr` removes the download mark (`com.apple.quarantine`) from the app of a DMG that carries it.

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

Intel Macs (where Homebrew goes into `/usr/local`), macOS 26 and earlier, starting on an actual Mac without Homebrew, the check shown when opening a received DMG, and long interactive use have not been checked.

Related: [Installation and required packages by OS](../installation.md), [3D rendering](../features/rendering.md), ["3 Run settings"](../features/execution.md), [MCP](../mcp.md)
