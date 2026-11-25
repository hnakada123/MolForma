# Installing on Ubuntu

How to build MolForma from source on Ubuntu, make a deb package and install it with apt. The development packages for the build and NWChem for calculations come from Ubuntu's official repositories.

These steps are for Ubuntu 26.04 LTS (amd64) and MolForma 1.1.0. The procedure was checked with MolForma 1.0.0 on 2026-10-28 in Docker's `ubuntu:26.04` (26.04.1 LTS) (see [Verified configuration](#verified-configuration)).

MolForma has no package in Ubuntu's official repositories or in a PPA (as of 2026-10-28). You build it from the source on GitHub and make a deb package (`molforma_1.1.0_amd64.deb`) with CPack, which comes with CMake. As apt manages the installed package, `apt remove` removes it. Since 1.1.0, [Releases](https://github.com/hnakada123/MolForma/releases) also has a deb package built on Ubuntu 26.04 LTS (see [Using the release package](#using-the-release-package)).

## Using the release package

`MolForma-<version>-ubuntu-26.04-amd64.zip` on [Releases](https://github.com/hnakada123/MolForma/releases) holds a deb package built on Ubuntu 26.04 LTS from the source of the release tag, the script that made it (the repository's `packaging/ubuntu/build-deb.sh`), an install guide (`INSTALL.en.md`, in Japanese `INSTALL.md`) and the licenses. Extract the ZIP and install it with apt, without building anything. apt installs the packages used at run time (the paragraph below the table of [Required packages](#required-packages)) from the official repositories automatically.

```sh
unzip MolForma-1.1.0-ubuntu-26.04-amd64.zip
cd MolForma-1.1.0-ubuntu-26.04-amd64
sudo apt update
sudo apt install ./molforma_1.1.0_amd64.deb
```

- It is for Ubuntu 26.04 LTS only. Other versions of Ubuntu cannot satisfy its dependencies and do not install it (checked with 26.10). On them, build from source with the steps below.
- Its name and version (`molforma` 1.1.0) are those of the package the steps below make. To replace one with the other, add `--reinstall`, as in `sudo apt install --reinstall ./molforma_1.1.0_amd64.deb`.
- On Ubuntu 26.04 with the packages of [1. Install the packages](#1-install-the-packages) (all of them, with RDKit, VTK and Qt SVG) and `curl`, `build-deb.sh` run in an empty folder makes a deb of the same configuration. It checks the tag's source by SHA-256 before building, stops if it cannot use RDKit, VTK or Qt SVG, and makes the deb after the tests.
- The package is not signed. The SHA-256 of the ZIP is in the release notes.
- Install NWChem as in [2. Install NWChem](#2-install-nwchem). Starting and checking are as in [5. Start and check](#5-start-and-check) (`dpkg -L molforma` lists the installed files). `sudo apt remove molforma` removes it.

## Required packages

The build needs these packages (see [1. Install the packages](#1-install-the-packages)).

| Kind | Packages | Used for |
| --- | --- | --- |
| Required | `build-essential`, `cmake`, `ninja-build`, `git`, `pkgconf`, `file` | Building and making the deb package (CPack uses `file` to look up the shared libraries used) |
| Required | `qt6-base-dev` | The interface (Qt Widgets), the tests (Qt Test) |
| Required | `libeigen3-dev` | Coordinate and geometry calculations |
| Required | `zlib1g-dev` | Reading gzip-compressed structure files |
| Required | `libopenbabel-dev` | Reading and writing structure files, force-field cleanup, 3D structures from SMILES |
| Optional | `librdkit-dev`, `libboost-iostreams-dev`, `libboost-serialization-dev` | Strict stereo perception (R/S, E/Z), residue recognition. RDKit's CMake configuration refers to the two Boost packages |
| Optional | `libvtk9-dev`, `libvtk9-qt-dev` | Drawing the 3D view with VTK (OpenGL). `nlohmann-json3-dev` and the MPI development packages that VTK's CMake configuration refers to come in as dependencies |
| Optional | `qt6-svg-dev` | Saving charts, spectra and reaction profiles as SVG too (without it, PNG and PDF) |

apt installs the packages used at run time automatically when it installs the deb package you make: the Qt 6 libraries and `qt6-qpa-plugins`, `libopenbabel7` (with the format plugins and force-field data) and, when built with RDKit and VTK, `librdkit1t64`, `libvtk9.5` and `libvtk9.5-qt`. The recommended packages `qt6-translations-l10n` (with the Japanese interface, Japanese for Qt's standard dialogs such as file selection too) and `fonts-noto-cjk` (Japanese fonts) come in as well with apt's default settings.

For calculations, install NWChem from the official repositories (universe) as `nwchem` (see [2. Install NWChem](#2-install-nwchem)).

- **RDKit and VTK**: Both are optional. They are enabled only when CMake finds them at build time, and the deb package's dependencies follow. Drawing with VTK also needs a GPU driver that provides OpenGL 3.2 or later.
- **NWChem**: Building and saving input (“Save as job”) works without NWChem.
- **External programs (optional)**: MP4 video export uses `ffmpeg` (`sudo apt install ffmpeg`), and “Tools → Optimize with xTB…” uses [xtb](https://github.com/grimme-lab/xtb) (obtain it separately and put it in PATH, or set it in “Edit → Preferences”). Both are only started at run time and are not needed for building.

## 1. Install the packages

Update the package lists and install the required packages.

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build git pkgconf file qt6-base-dev libeigen3-dev zlib1g-dev libopenbabel-dev
```

For RDKit and VTK, install these as well. VTK's development package has many dependencies (the Java version of VTK and more): from a bare system, about 780 packages with the ones above, and about 1 GB to download.

```sh
sudo apt install librdkit-dev libboost-iostreams-dev libboost-serialization-dev libvtk9-dev libvtk9-qt-dev
```

To save charts as SVG too, install `qt6-svg-dev` as well.

```sh
sudo apt install qt6-svg-dev
```

## 2. Install NWChem

```sh
sudo apt install nwchem
```

This also brings in the Open MPI build of NWChem (`nwchem-openmpi`) and the MPI run time (`openmpi-bin`). NWChem goes to `/usr/bin/nwchem` and its basis set library to `/usr/share/nwchem/`. The basis set library works as installed, so you need neither `~/.nwchemrc` nor `NWCHEM_BASIS_LIBRARY`. MolForma's “3 Run settings” defaults (executable `nwchem`, launch with MPI, MPI launcher `mpirun`, 2 processes) launch this NWChem as they are.

## 3. Get the source and build

Get the source and build a Release build. Keep the folder you got and its `build`; updating uses them.

```sh
git clone https://github.com/hnakada123/MolForma.git
cd MolForma
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
```

Running the third line, `cmake -S . -B build …` (the configuration: CMake looks for the libraries it needs and prepares the build), prints a line beginning with `-- MolForma rendering:` in the terminal. This line shows how the 3D view will be drawn. Near the end, the output looks like this, for example:

```
--   Found openbabel-3, version 3.1.1
-- MolForma rendering: VTK 9.5.2 (Qt6), with QPainter fallback
-- Configuring done (2.2s)
-- Generating done (0.2s)
-- Build files have been written to: …/MolForma/build
```

If it scrolled past, running the configuration command again prints the same line. The `MolForma rendering:` line means:

| Output | Meaning |
| --- | --- |
| `MolForma rendering: VTK 9.5.2 (Qt6), with QPainter fallback` | Draws with VTK (with QPainter where OpenGL is unavailable) |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` | Does not use VTK. The `VTK development dependency unavailable:` lines before it name what is missing |

After installing the missing packages, start again from the configuration command. `-j 4` is the number of compile jobs run at once; lower it if memory runs short.

To run the tests (optional), use the command below. They use neither NWChem nor the network. If you do not need the tests, adding `-DBUILD_TESTING=OFF` to the configuration command shortens the build.

```sh
ctest --test-dir build --output-on-failure
```

## 4. Make and install the package

Make the deb package in `build` and install it with apt. With `./`, apt takes it as a file rather than a package name and installs the missing dependencies with it.

```sh
cd build
cpack -G DEB
sudo apt install ./molforma_1.1.0_amd64.deb
```

`cpack` works out the dependencies from the shared libraries the executable uses (`dpkg-shlibdeps`) and writes them into the package. The package places these files (`dpkg -L molforma` lists them). The file type and menu information is updated automatically when it is installed.

| Location | Contents |
| --- | --- |
| `/usr/bin/molforma` | The executable |
| `/usr/share/applications/molforma.desktop` | The application menu entry |
| `/usr/share/icons/hicolor/` | Icons (`scalable` and `256x256`) |
| `/usr/share/mime/packages/molforma.xml` | The file types of `.molforma.json` (projects) and `.nw` (NWChem input) |
| `/usr/share/molforma/` | LICENSE, NOTICE, README and the manual's Markdown (the manual is also built into the executable) |

## 5. Start and check

Run `molforma` in a terminal or choose “MolForma” in the list of applications. The interface language follows the system language (“Language (after restart)” in “Edit → Preferences…” switches it; it takes effect after a restart).

1. Open “Help → About MolForma” and check that it reads “MolForma 1.1.0” and, with RDKit and VTK installed, “Stereochemistry and residue recognition (RDKit): Enabled” and “3D rendering: VTK (OpenGL)”. Where OpenGL is unavailable, it reads “3D rendering: QPainter (CPU)”.
2. Open “3 Run settings” and check that below the “NWChem executable” and “MPI launcher” fields appear `/usr/bin/nwchem` and `/usr/bin/mpirun`, where PATH found them.
3. Try a calculation on a small molecule. The steps are in [recipe 1 Building from atoms and bonds](../recipes/atoms_and_bonds.md).

## Updating and removing

### Updating

Update the source, rebuild, make the package again and install it. While the version number stays the same, apt does not install it again unless you add `--reinstall`.

```sh
cd MolForma
git pull --ff-only
cmake --build build -j 4
cd build
cpack -G DEB
sudo apt install --reinstall ./molforma_1.1.0_amd64.deb
```

Upgrading Ubuntu to a new release changes the versions of libraries such as Qt, Open Babel, RDKit and VTK; MolForma may then stop starting, or be removed because its dependencies cannot be met. In that case, delete `build`, start again from the configuration in [3. Get the source and build](#3-get-the-source-and-build), make the package again and install it.

### Removing

```sh
sudo apt remove molforma
```

The application settings (`~/.config/MolForma/`), autosaves (`~/.local/share/MolForma/`) and calculation jobs (by default `~/MolForma/jobs`) remain; delete them by hand if you no longer need them. The development packages installed in [1. Install the packages](#1-install-the-packages) remain too; if you no longer need them, remove them by name with `sudo apt remove`, and what came in with them with `sudo apt autoremove`. `sudo apt remove nwchem` followed by `sudo apt autoremove` removes NWChem.

## Troubleshooting

| Symptom | Check and fix |
| --- | --- |
| The configuration stops with an error that `Qt6`, `Eigen3` or `openbabel-3` is not found | Install the required packages from [1. Install the packages](#1-install-the-packages) and start again from the configuration |
| The configuration stops with `The link interface of target "RDKit::rdkit_base" contains: Boost::serialization … but the target was not found` | `librdkit-dev` does not install Boost's development packages. Install `libboost-iostreams-dev` and `libboost-serialization-dev` and start again from the configuration |
| It reports `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` | Check that `libvtk9-dev` and `libvtk9-qt-dev` are installed. Install what the `VTK development dependency unavailable:` lines name and start again from the configuration |
| `cpack` stops with `CPackDeb: file utility is not available` | Install `file` with `sudo apt install file` and run `cpack -G DEB` again |
| Installing the release deb says `unmet dependencies` | The release deb is for Ubuntu 26.04 LTS only. On other versions, build from source as in [3. Get the source and build](#3-get-the-source-and-build) |
| After updating, apt says `molforma is already the newest version` and does not install it again | The version number is the same; use `sudo apt install --reinstall ./molforma_1.1.0_amd64.deb` |
| The 3D view shows “Rendering: QPainter (CPU) · simplified, OpenGL unavailable” at its top left | OpenGL 3.2 or later is unavailable. Check the GPU driver (`glxinfo -B` from `mesa-utils` shows the OpenGL implementation and version in use). It may be unavailable over remote desktops and in virtual machines |
| Rotation is slow although VTK draws | If `glxinfo -B` reports `llvmpipe` or similar, OpenGL is rendered in software |
| Japanese text shows as boxes | Install a Japanese font such as `fonts-noto-cjk` |
| With the Japanese interface, Qt's standard dialogs such as file selection keep English text | Install `qt6-translations-l10n` |
| The pre-run check cannot find NWChem or the MPI launcher | Check `command -v nwchem mpirun` in a terminal. If NWChem is installed elsewhere, set its path in “3 Run settings” |
| The calculation's log (standard error) shows `osc_ucx_component.c … Error: OSC UCX component priority set inside component query failed` | A warning of an Open MPI communication component (UCX); it does not affect the results. To hide it, add the name `OMPI_MCA_osc` with the value `^ucx` to the additional environment variables in “3 Run settings” |
| It does not start, with `error while loading shared libraries` | The library versions changed. As in [Updating](#updating), start again from the configuration and make the package again |
| Memory runs short during the build | Lower the parallelism, as in `cmake --build build -j 2` |

## Verified configuration

On 2026-10-28, in Docker's `ubuntu:26.04` (Ubuntu 26.04.1 LTS, amd64), the following was checked.

- In a container with the packages above, the source configured and built (with both RDKit and VTK), and all 14 CTest tests passed (both serially and in parallel). Under Xvfb with Mesa's software OpenGL, the optional test of the VTK rendering path ([3D rendering](../features/rendering.md)) passed too.
- `cpack -G DEB` made the deb package, and in another, bare container `apt install ./molforma_1.0.0_amd64.deb` and `apt install nwchem` installed with their dependencies. As a normal user, `molforma` started and reported version 1.0.0, and the file types were registered.
- Without a `~/.nwchemrc`, `mpirun -np 2 nwchem` completed an HF/6-31G* calculation of the hydrogen molecule. MolForma's run settings test that uses a real NWChem (optional) completed an HF/STO-3G calculation of water with 2 MPI processes, with an SCF energy of −74.945628203255 Hartree.

| Item | Version |
| --- | --- |
| Qt (`qt6-base-dev`) | 6.10.2 |
| Open Babel | 3.1.1 |
| Eigen | 3.4.0 |
| RDKit, Boost | 2025.03.6, 1.90.0 |
| VTK | 9.5.2 |
| GCC, CMake, Ninja | 15.2.0, 4.2.3, 1.13.2 |
| NWChem | 7.3.1 |
| Open MPI | 5.0.10 |

A real desktop (VTK rendering on a GPU, GNOME's list of applications) and other versions such as Ubuntu 24.04 LTS were not checked.

On 2026-11-25, the deb package of the 1.1.0 release was made in a new Docker `ubuntu:26.04` (26.04.1 LTS, with that day's updates). With the packages of [1. Install the packages](#1-install-the-packages) (with RDKit, VTK and Qt SVG) installed, `packaging/ubuntu/build-deb.sh` run as a regular user passed all 20 tests. It used Qt 6.10.2, Open Babel 3.1.1, RDKit 2025.03.6, VTK 9.5.2, GCC 15.2.0 and CMake 4.2.3. Installed with `apt install ./molforma_1.1.0_amd64.deb` in another new `ubuntu:26.04` container, it brought in its dependencies and recommended packages (`fonts-noto-cjk`, `qt6-translations-l10n`) automatically with no library missing, reported version 1.1.0, wrote an offscreen screenshot and, over `--mcp-gui`, made a 3D structure (a candidate) from SMILES. In a `ubuntu:26.10` container its dependencies could not be satisfied and it was not installed. Display on a real desktop and calculations with NWChem were not checked with this package.

See also: [Installation and required packages by OS](../installation.md), [3D rendering](../features/rendering.md), [“3 Run settings”](../features/execution.md)
