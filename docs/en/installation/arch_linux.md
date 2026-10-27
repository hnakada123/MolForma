# Installing on Arch Linux

How to build MolForma from source on Arch Linux, install it, and set up NWChem for calculations. The dependencies come from the official repositories (core, extra) and NWChem from the AUR.

These steps are for Arch Linux (x86-64) and MolForma 0.1.0. They were checked on 2026-10-27 (see [Verified configuration](#verified-configuration)).

MolForma has no package in the official repositories or the AUR (as of 2026-10-27). You build it from the source on GitHub and place it in `/usr/local` with `cmake --install`. As pacman does not manage it, update and remove it as described in [Updating and removing](#updating-and-removing).

## Required packages

| Kind | Packages | Source | Used for |
| --- | --- | --- | --- |
| Required | `base-devel`, `git`, `cmake`, `ninja` | core, extra | Building (`base-devel` includes GCC and pkgconf) |
| Required | `qt6-base` | extra | The interface (Qt Widgets), the tests (Qt Test) |
| Required | `eigen` | extra | Coordinate and geometry calculations |
| Required | `openbabel` | extra | Reading and writing structure files, force-field cleanup, 3D structures from SMILES (includes the format plugins and force-field data) |
| Optional | `rdkit` | extra | Strict stereo perception (R/S, E/Z), residue recognition |
| Optional | `vtk`, `fast_float`, `nlohmann-json`, `openmpi` | extra | Drawing the 3D view with VTK (OpenGL) |
| Optional | `qt6-translations` | extra | With the Japanese interface, Japanese for Qt's standard dialogs such as file selection too (standard buttons such as “Save” and “Cancel” are Japanese even without it) |
| Optional | `noto-fonts-cjk` | extra | Japanese fonts (if none are installed) |
| Calculations | `nwchem` | AUR | Running calculations. Also brings in `openmpi` and `scalapack` (from the AUR) as dependencies |

- **VTK**: With `vtk` alone, CMake cannot load VTK's development configuration, and MolForma is configured to draw with QPainter (CPU). Also install `fast_float` and `nlohmann-json` (both header-only) and `openmpi`, which VTK's configuration refers to. Without `openmpi`, a configuration that uses VTK stops with a CMake error. Drawing with VTK also needs a GPU driver that provides OpenGL 3.2 or later.
- **RDKit**: `rdkit` also depends on Python and more, but MolForma uses only its C++ libraries.
- **NWChem**: Building and saving input (“Save as job”) works without NWChem.

## 1. Install the packages

Update the system and install the required packages.

```sh
sudo pacman -Syu --needed base-devel git cmake ninja qt6-base eigen openbabel
```

For the optional features, install these as well. Leave out what you do not need.

```sh
sudo pacman -S --needed rdkit vtk fast_float nlohmann-json openmpi qt6-translations noto-fonts-cjk
```

## 2. Install NWChem (AUR)

The AUR's `nwchem` depends on `scalapack`, which is in the AUR too, so install it with a helper that resolves AUR dependencies (yay here). It is built from source and takes a while.

```sh
yay -S nwchem
```

If yay is not installed, build and install it first as a normal user. Review the PKGBUILD shown before you continue.

```sh
git clone https://aur.archlinux.org/yay.git
cd yay
less PKGBUILD
makepkg -si
```

NWChem goes to `/usr/bin/nwchem` and its basis set library to `/usr/share/nwchem/libraries/`. The basis set library works as installed, so you need neither `~/.nwchemrc` nor `NWCHEM_BASIS_LIBRARY`. MolForma's “3 Run settings” defaults (executable `nwchem`, launch with MPI, MPI launcher `mpirun`, 2 processes) launch this NWChem as they are.

## 3. Get the source and build

Get the source and build a Release build. Keep the folder you got and its `build`; updating and removing use them.

```sh
git clone https://github.com/hnakada123/MolForma.git
cd MolForma
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
```

Running the third line, `cmake -S . -B build …` (the configuration: CMake looks for the libraries it needs and prepares the build), prints a line beginning with `-- MolForma rendering:` in the terminal. This line shows how the 3D view will be drawn. Near the end, the output looks like this, for example:

```
--   Found openbabel-3, version 3.2.1
-- MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback
-- Configuring done (2.5s)
-- Generating done (0.2s)
-- Build files have been written to: …/MolForma/build
```

If it scrolled past, running the configuration command again prints the same line. The `MolForma rendering:` line means:

| Output | Meaning |
| --- | --- |
| `MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback` | Draws with VTK (with QPainter where OpenGL is unavailable) |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` | Does not use VTK. The `VTK development dependency unavailable:` lines before it name what is missing (`FastFloat::fast_float` is `fast_float`, `nlohmann_json::nlohmann_json` is `nlohmann-json`) |

After installing the missing packages, start again from the configuration command. `-j 4` is the number of compile jobs run at once; lower it if memory runs short.

To run the tests (optional), use the command below. They use neither NWChem nor the network. If you do not need the tests, adding `-DBUILD_TESTING=OFF` to the configuration command shortens the build.

```sh
ctest --test-dir build --output-on-failure
```

You can also start `./build/molforma` without installing.

## 4. Install

Place MolForma in `/usr/local` and update the file type (MIME type) information. pacman's hooks update only `/usr/share`, so update the `/usr/local` part here.

```sh
sudo cmake --install build
sudo update-mime-database /usr/local/share/mime
sudo update-desktop-database /usr/local/share/applications
```

`update-mime-database` comes from `shared-mime-info` and `update-desktop-database` from `desktop-file-utils`. These files are placed:

| Location | Contents |
| --- | --- |
| `/usr/local/bin/molforma` | The executable |
| `/usr/local/share/applications/molforma.desktop` | The application menu entry |
| `/usr/local/share/icons/hicolor/` | Icons (`scalable` and `256x256`) |
| `/usr/local/share/mime/packages/molforma.xml` | The file types of `.kgui.json` (projects) and `.nw` (NWChem input) |
| `/usr/local/share/molforma/` | LICENSE, NOTICE, README and the manual's Markdown (the manual is also built into the executable) |

Without administrator rights, `cmake --install build --prefix ~/.local` places it in your home directory. Then add `~/.local/bin` to PATH and run `update-mime-database ~/.local/share/mime`.

## 5. Start and check

Run `molforma` in a terminal or choose “MolForma” in the application menu. If it does not appear in the KDE Plasma menu right away, run `kbuildsycoca6`. The interface language follows the system language (“Language (after restart)” in “Edit → Preferences…” switches it; it takes effect after a restart).

1. Open “Help → About MolForma” and check “Stereochemistry and residue recognition (RDKit)” and “3D rendering”. With RDKit and VTK installed they read “Enabled” and “VTK (OpenGL)”.
2. Open “3 Run settings” and check that below the “NWChem executable” and “MPI launcher” fields appear `/usr/bin/nwchem` and `/usr/bin/mpirun`, where PATH found them.
3. Try a calculation on a small molecule. The steps are in [recipe 1 Building from atoms and bonds](../recipes/atoms_and_bonds.md).

## Updating and removing

### Updating

Update the source, rebuild and install again.

```sh
cd MolForma
git pull --ff-only
cmake --build build -j 4
sudo cmake --install build
```

As pacman does not manage MolForma, it is not rebuilt when `pacman -Syu` brings new versions of shared libraries such as Qt, Open Babel, RDKit or VTK. If it stops starting with `molforma: error while loading shared libraries`, delete `build`, start again from the configuration in [3. Get the source and build](#3-get-the-source-and-build) and install again.

### Removing

Delete the installed files from the list recorded in `build`, then the remaining folder `/usr/local/share/molforma`.

```sh
cd MolForma
sudo xargs -d '\n' rm -f -- < build/install_manifest.txt
sudo rm -r /usr/local/share/molforma
sudo update-mime-database /usr/local/share/mime
sudo update-desktop-database /usr/local/share/applications
```

The application settings (`~/.config/MolForma/`), autosaves (`~/.local/share/MolForma/`) and calculation jobs (by default `~/MolForma/jobs`) remain; delete them by hand if you no longer need them. `sudo pacman -Rs nwchem` removes NWChem.

## Troubleshooting

| Symptom | Check and fix |
| --- | --- |
| The configuration stops with an error that `Qt6`, `Eigen3` or `openbabel-3` is not found | Install the required packages from [1. Install the packages](#1-install-the-packages) and start again from the configuration |
| It reports `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` | Install the packages named in the `VTK development dependency unavailable:` lines (usually `fast_float` and `nlohmann-json`) and start again from the configuration |
| The configuration stops with an error in `VTK-targets.cmake` | Install `openmpi`. If you do not want VTK, add `-DMOLFORMA_ENABLE_VTK=OFF` to the configuration |
| The 3D view shows “Rendering: QPainter (CPU) · simplified, OpenGL unavailable” at its top left | OpenGL 3.2 or later is unavailable. Check the GPU driver (`glxinfo -B` from `mesa-utils` shows the OpenGL implementation and version in use). It may be unavailable over remote desktops and in virtual machines |
| Rotation is slow although VTK draws | If `glxinfo -B` reports `llvmpipe` or similar, OpenGL is rendered in software |
| Japanese text shows as boxes | Install a Japanese font such as `noto-fonts-cjk` |
| With the Japanese interface, Qt's standard dialogs such as file selection keep English text | Install `qt6-translations` |
| The pre-run check cannot find NWChem or the MPI launcher | Check `command -v nwchem mpirun` in a terminal. If NWChem is installed elsewhere, set its path in “3 Run settings” |
| It does not start, with `error while loading shared libraries` | A system update changed shared libraries. Rebuild as in [Updating](#updating) |
| Memory runs short during the build | Lower the parallelism, as in `cmake --build build -j 2` |

## Verified configuration

On 2026-10-27, on Arch Linux (x86-64) with the configuration below, the source was freshly fetched from GitHub, built, and CTest was run. The installation was checked by placing it in a temporary directory (`DESTDIR`). The VTK configuration was built against the contents of the `fast_float` and `nlohmann-json` packages.

| Item | Version |
| --- | --- |
| Qt (`qt6-base`) | 6.12.0 |
| Open Babel | 3.2.1 |
| Eigen | 5.0.1 |
| RDKit | 2026.09.1 |
| VTK | 9.7.1 |
| GCC, CMake, Ninja | 16.2.1, 4.4.4, 1.13.2 |
| NWChem (the AUR's `nwchem`) | 7.3.1 |
| Open MPI | 5.0.11 |

NWChem was started with `mpirun -np 2 nwchem` without a `~/.nwchemrc` and completed an HF/6-31G* calculation of the hydrogen molecule. This was checked on a system with the dependencies already installed; resolving them with pacman and yay on a new system is not covered.

See also: [Installation and required packages by OS](../installation.md), [3D rendering](../features/rendering.md), [“3 Run settings”](../features/execution.md)
