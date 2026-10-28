# Installing on Arch Linux

How to make MolForma an Arch Linux package and install it with pacman. `makepkg` builds the package `molforma-git` from the repository's `packaging/arch/PKGBUILD`. The dependencies come from the official repositories (core, extra) automatically, and NWChem for calculations from the AUR.

These steps are for Arch Linux (x86-64) and MolForma 1.0.0. They were checked on 2026-10-28 (see [Verified configuration](#verified-configuration)).

MolForma has no package in the official repositories or the AUR (as of 2026-10-28). The PKGBUILD fetches the source of the main branch on GitHub and builds it. As pacman manages the installed package, `pacman -R` removes it.

## Required packages

`makepkg -s` installs these packages from the official repositories automatically.

| Kind | Packages | Used for |
| --- | --- | --- |
| Run time | `qt6-base` | The interface (Qt Widgets) |
| Run time | `openbabel` | Reading and writing structure files, force-field cleanup, 3D structures from SMILES (includes the format plugins and force-field data) |
| Run time | `rdkit` | Strict stereo perception (R/S, E/Z), residue recognition |
| Run time | `vtk` | Drawing the 3D view with VTK (OpenGL) |
| Build time | `git`, `cmake`, `ninja`, `eigen` | Fetching the source and building |
| Build time | `boost`, `fast_float`, `nlohmann-json`, `openmpi` | What the CMake configurations of RDKit and VTK refer to |

Install these yourself as needed.

| Kind | Package | Source | Used for |
| --- | --- | --- | --- |
| Required | `base-devel` | core | Making packages with `makepkg` (includes GCC) |
| Calculations | `nwchem` | AUR | Running calculations. Also brings in `openmpi` and `scalapack` (from the AUR) as dependencies |
| Optional | `qt6-translations` | extra | With the Japanese interface, Japanese for Qt's standard dialogs such as file selection too (standard buttons such as “Save” and “Cancel” are Japanese even without it) |
| Optional | `noto-fonts-cjk` | extra | Japanese fonts (if none are installed) |

- **RDKit and VTK**: The PKGBUILD builds with RDKit and VTK, and stops the build when CMake cannot use either of them. Drawing with VTK also needs a GPU driver that provides OpenGL 3.2 or later. `rdkit` also depends on Python and more, but MolForma uses only its C++ libraries.
- **NWChem**: Building and saving input (“Save as job”) works without NWChem.

## 1. Prepare

Update the system and install `base-devel` and `git`, which making the package uses. For the optional packages, install them as well.

```sh
sudo pacman -Syu --needed base-devel git
sudo pacman -S --needed qt6-translations noto-fonts-cjk
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

## 3. Make and install the package

Fetch the PKGBUILD into a working folder, review it, then run `makepkg`. Updating uses the working folder too.

```sh
mkdir -p ~/.cache/molforma-arch
cd ~/.cache/molforma-arch
curl -LO https://raw.githubusercontent.com/hnakada123/MolForma/main/packaging/arch/PKGBUILD
less PKGBUILD
makepkg -si
```

`makepkg -si` does the following in order.

1. Installs the missing dependencies with pacman (`-s`). It asks for the administrator password.
2. Fetches the source of the main branch on GitHub and builds it. A `-- MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback` line along the way means it draws with VTK. If CMake cannot use VTK or RDKit, it stops here with `==> ERROR: CMake cannot use VTK` or similar (see [Troubleshooting](#troubleshooting)).
3. Runs the tests. They use neither NWChem nor the network. To skip them, use `makepkg -si --nocheck`.
4. Makes the package (`molforma-git-1.0.0.r<commit count>.g<commit id>-1-x86_64.pkg.tar.zst`) and installs it with pacman (`-i`).

The build uses all CPU cores. If memory runs short, lower the number of compile jobs run at once, as in `CMAKE_BUILD_PARALLEL_LEVEL=2 makepkg -si`.

Building for development without making a package is described in the [README](https://github.com/hnakada123/MolForma#readme).

## 4. Start and check

The package places these files (`pacman -Ql molforma-git` lists them). pacman's hooks update the file type and menu information automatically.

| Location | Contents |
| --- | --- |
| `/usr/bin/molforma` | The executable |
| `/usr/share/applications/molforma.desktop` | The application menu entry |
| `/usr/share/icons/hicolor/` | Icons (`scalable` and `256x256`) |
| `/usr/share/mime/packages/molforma.xml` | The file types of `.molforma.json` (projects) and `.nw` (NWChem input) |
| `/usr/share/molforma/` | LICENSE, NOTICE, README and the manual's Markdown (the manual is also built into the executable) |

Run `molforma` in a terminal or choose “MolForma” in the application menu. If it does not appear in the KDE Plasma menu right away, run `kbuildsycoca6`. The interface language follows the system language (“Language (after restart)” in “Edit → Preferences…” switches it; it takes effect after a restart).

1. Open “Help → About MolForma” and check that it reads “MolForma 1.0.0”, “Stereochemistry and residue recognition (RDKit): Enabled” and “3D rendering: VTK (OpenGL)”. Where OpenGL is unavailable, it reads “3D rendering: QPainter (CPU)”.
2. Open “3 Run settings” and check that below the “NWChem executable” and “MPI launcher” fields appear `/usr/bin/nwchem` and `/usr/bin/mpirun`, where PATH found them.
3. Try a calculation on a small molecule. The steps are in [recipe 1 Building from atoms and bonds](../recipes/atoms_and_bonds.md).

## Updating and removing

### Updating

Fetch the PKGBUILD again, then make the package again. `makepkg` fetches the latest source of the main branch, makes a package of the new version and installs it.

```sh
cd ~/.cache/molforma-arch
curl -LO https://raw.githubusercontent.com/hnakada123/MolForma/main/packaging/arch/PKGBUILD
makepkg -si
```

As it is not in the AUR, `pacman -Syu` and `yay -Syu` do not update MolForma. If `pacman -Syu` brings new versions of shared libraries such as Qt, Open Babel, RDKit or VTK and MolForma stops starting with `molforma: error while loading shared libraries`, make it again with `makepkg -sif`, even if the source has not changed (`-f` makes the package again even if one was already made).

### Removing

```sh
sudo pacman -R molforma-git
```

The application settings (`~/.config/MolForma/`), autosaves (`~/.local/share/MolForma/`), calculation jobs (by default `~/MolForma/jobs`) and the working folder (`~/.cache/molforma-arch`) remain; delete them by hand if you no longer need them. `pacman -Qdtq` lists the dependencies installed for the build that nothing else uses. `sudo pacman -Rs nwchem` removes NWChem.

## Troubleshooting

| Symptom | Check and fix |
| --- | --- |
| It stops with `==> ERROR: CMake cannot use VTK` | The `VTK development dependency unavailable:` lines before it name what is missing (`FastFloat::fast_float` is `fast_float`, `nlohmann_json::nlohmann_json` is `nlohmann-json`). Install that package and run `makepkg -si` again |
| It stops with `==> ERROR: CMake cannot use RDKit` | Check that `boost` is installed and run `makepkg -si` again |
| A test fails and no package is made | Report it with the output of the failed test in the [GitHub issues](https://github.com/hnakada123/MolForma/issues). To make the package without the tests, use `makepkg -si --nocheck` |
| Memory runs short during the build | Lower the parallelism, as in `CMAKE_BUILD_PARALLEL_LEVEL=2 makepkg -si` |
| The 3D view shows “Rendering: QPainter (CPU) · simplified, OpenGL unavailable” at its top left | OpenGL 3.2 or later is unavailable. Check the GPU driver (`glxinfo -B` from `mesa-utils` shows the OpenGL implementation and version in use). It may be unavailable over remote desktops and in virtual machines |
| Rotation is slow although VTK draws | If `glxinfo -B` reports `llvmpipe` or similar, OpenGL is rendered in software |
| Japanese text shows as boxes | Install a Japanese font such as `noto-fonts-cjk` |
| With the Japanese interface, Qt's standard dialogs such as file selection keep English text | Install `qt6-translations` |
| The pre-run check cannot find NWChem or the MPI launcher | Check `command -v nwchem mpirun` in a terminal. If NWChem is installed elsewhere, set its path in “3 Run settings” |
| It does not start, with `error while loading shared libraries` | A system update changed shared libraries. Make it again with `makepkg -sif` as in [Updating](#updating) |

## Verified configuration

On 2026-10-28, on Arch Linux (x86-64) with the configuration below, `makepkg` made the package from this PKGBUILD, its tests (the PKGBUILD's `check()`) passed, and the package's contents and dependencies were checked. The dependencies were installed with pacman, and `makepkg`'s check of the run-time and build-time dependencies passed.

| Item | Version |
| --- | --- |
| Qt (`qt6-base`) | 6.12.0 |
| Open Babel | 3.2.1 |
| Eigen | 5.0.1 |
| RDKit, Boost | 2026.09.1, 1.92.0 |
| VTK | 9.7.1 |
| fast_float, nlohmann-json | 8.3.2, 3.12.0 |
| GCC, CMake, Ninja | 16.2.1, 4.4.4, 1.13.2 |
| NWChem (the AUR's `nwchem`) | 7.3.1 |
| Open MPI | 5.0.11 |

NWChem was started with `mpirun -np 2 nwchem` without a `~/.nwchemrc` and completed an HF/6-31G* calculation of the hydrogen molecule. This was checked on a system with the dependencies already installed; installing the package with pacman (`-i`) and installing the dependencies automatically with `makepkg -s` and yay on a new system are not covered. The package's executable, unpacked, was checked to start and to report version 1.0.0.

See also: [Installation and required packages by OS](../installation.md), [3D rendering](../features/rendering.md), [“3 Run settings”](../features/execution.md)
