# Making an AppImage (any Linux)

How to make MolForma a single executable file that runs on any Linux distribution (an AppImage). It is built on Ubuntu 24.04 inside Docker, and the libraries MolForma uses, such as Qt, Open Babel, RDKit and VTK, go into one file. The AppImage needs no installation: make it executable and start it.

The computer that makes the AppImage can run any distribution: it needs Docker on x86-64 Linux. The computer that runs the AppImage needs an x86-64 Linux desktop with glibc 2.38 or later and the `libstdc++` of GCC 13 or later (Ubuntu 24.04 or later, Debian 13 or later, Fedora 39 or later, Arch Linux and others). These steps are for MolForma 1.0.0. They were checked on 2026-10-28 (see [Verified configuration](#verified-configuration)).

No AppImage is distributed yet (as of 2026-10-28). Make one with these steps.

## What the AppImage holds

| Kind | Contents |
| --- | --- |
| Inside | MolForma, Qt 6.4.2 (the xcb and offscreen platforms and Qt's own translations), Open Babel 3.1.1 (format plugins and force-field data), RDKit 2023.09.3, VTK 9.7.1 (built for Qt 6), the libraries these use such as Boost, and the license of each library (`usr/share/doc`) |
| Taken from the computer running it | glibc and `libstdc++`, OpenGL (the GPU driver), the basic X11 libraries (`libX11`, `libxcb` and so on), fontconfig, FreeType and HarfBuzz with the fonts, NWChem and MPI |

Desktops usually have the libraries taken from the computer. For the Japanese interface, a Japanese font must be installed on the computer.

## 1. Prepare Docker

How to install Docker depends on the distribution (`sudo pacman -S docker` on Arch Linux, `sudo apt install docker.io` on Ubuntu). Once installed, start Docker and add yourself to the `docker` group to use it as a normal user. The group takes effect after you log in again.

```sh
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER"
```

Users in the `docker` group can do through Docker what an administrator can. If that concerns you, do not join the group and run the `docker` commands below as `sudo docker`.

## 2. Make the build environment

Get the source and make the AppImage's build environment (the Docker image `molforma-appimage`).

```sh
git clone https://github.com/hnakada123/MolForma.git
cd MolForma
docker build -t molforma-appimage packaging/appimage
```

`packaging/appimage/Dockerfile` does the following.

1. Installs the development packages for the build on Ubuntu 24.04. Ubuntu 24.04 is used for its older glibc, so that the AppImage runs on more distributions.
2. Builds VTK 9.7.1 from source for Qt 6, only the parts MolForma uses. Ubuntu 24.04's VTK is built for Qt 5 and cannot be used as it is.
3. Gets the tools that make the AppImage (linuxdeploy, linuxdeploy-plugin-qt, appimagetool and the AppImage runtime).

What it downloads is pinned by version and SHA-256. Building VTK takes a while: about 23 minutes on a 16-core computer (4 compile jobs at once). The image is about 2 GB. You make it only once and reuse it afterwards.

VTK's build runs 4 compile jobs at once by default. Using all cores at once can leave a desktop unresponsive, so it is kept modest. To change it, use `docker build --build-arg JOBS=8 -t molforma-appimage packaging/appimage`.

## 3. Make the AppImage

In the folder of the source, run the commands below. This makes `build-appimage/MolForma-1.0.0-x86_64.AppImage` (about 100 MB) in about 3 minutes.

```sh
mkdir -p build-appimage
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src:ro" -v "$PWD/build-appimage:/out" molforma-appimage
```

The source folder is passed read-only, and the build happens inside the container. The file it makes belongs to your user. To change the number of compile jobs run at once (4 by default), add `-e JOBS=8` or similar to `docker run`.

`packaging/appimage/build.sh` does the following.

1. Builds MolForma and places it in the AppImage's contents (the AppDir). If RDKit or VTK cannot be used, it stops rather than make an AppImage with features missing.
2. Copies Open Babel's format plugins and force-field data, and sets `BABEL_LIBDIR` and `BABEL_DATADIR` to them at startup.
3. Gathers, with linuxdeploy, the libraries MolForma and the plugins use, and Qt's platforms and translations.
4. Copies the license of each library to `usr/share/doc`.
5. Packs everything into one file with appimagetool.

## 4. Start it

Make it executable and start it. You can also give a file to open.

```sh
chmod +x MolForma-1.0.0-x86_64.AppImage
./MolForma-1.0.0-x86_64.AppImage
./MolForma-1.0.0-x86_64.AppImage molecule.xyz
```

- **FUSE**: At startup the AppImage mounts itself with FUSE. It needs `fusermount3` (the `fuse3` package) or `fusermount`, which most desktops have. It does not need `libfuse2`.
- **Wayland**: The AppImage holds the xcb platform. On a Wayland desktop it shows through XWayland.
- **Menu**: The AppImage does not add itself to the application menu. To start it from the menu, use an application that manages AppImages (Gear Lever, AppImageLauncher and others).
- **MCP server**: In an MCP client, register the AppImage's path followed by `--mcp` (or `--mcp-gui` to show the interface) as the server's command (see [MCP server](../mcp.md)).

Once started, open “Help → About MolForma” and check that it reads “MolForma 1.0.0”, “Stereochemistry and residue recognition (RDKit): Enabled” and “3D rendering: VTK (OpenGL)”. Where OpenGL is unavailable, it reads “3D rendering: QPainter (CPU)”.

The AppImage does not hold NWChem, which calculations use. Install your distribution's NWChem ([the AUR's `nwchem`](arch_linux.md#2-install-nwchem-aur) on Arch Linux, [`apt install nwchem`](ubuntu.md#2-install-nwchem) on Ubuntu). The “3 Run settings” defaults (executable `nwchem`, launch with MPI, MPI launcher `mpirun`, 2 processes) launch it as they are.

## Updating and removing

### Updating

Update the source and make the AppImage again as in [3. Make the AppImage](#3-make-the-appimage). The build environment's image can be used as it is. If `packaging/appimage/` changed, make the build environment again too as in [2. Make the build environment](#2-make-the-build-environment) (Docker reuses the steps that did not change).

```sh
cd MolForma
git pull --ff-only
docker build -t molforma-appimage packaging/appimage
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src:ro" -v "$PWD/build-appimage:/out" molforma-appimage
```

### Removing

Delete the AppImage file. The application settings (`~/.config/MolForma/`), autosaves (`~/.local/share/MolForma/`) and calculation jobs (by default `~/MolForma/jobs`) remain; delete them by hand if you no longer need them. `docker rmi molforma-appimage` removes the build environment.

## When distributing it

If you give the AppImage to others, follow the licenses of the software it holds. Their texts are in the AppImage's `usr/share/doc`, readable after `./MolForma-1.0.0-x86_64.AppImage --appimage-extract`.

- **MolForma (GPL-2.0-or-later), Open Babel (GPL-2.0-only) and Qt**: In binaries linked with Open Babel, Qt is used under the GPL-2.0 too (the repository's `NOTICE`). When distributing binaries, make the corresponding source available too. For MolForma, point to the source of the commit the AppImage was made from. The source of Open Babel, Qt and the other Ubuntu packages it holds is Ubuntu 24.04's source packages; VTK's is the 9.7.1 source from vtk.org.
- **The others (RDKit, VTK, Boost and so on)**: Under BSD and similar licenses, they can be distributed with their license texts, which it holds.

## Troubleshooting

| Symptom | Check and fix |
| --- | --- |
| It says `permission denied while trying to connect to the Docker daemon socket` | Check that you logged in again after joining the `docker` group, or run it as `sudo docker …` |
| `docker build` stops with a `sha256sum` mismatch | A downloaded file differs from the pinned version. It may have been altered on the way, so find the cause rather than go on |
| The desktop becomes unresponsive or memory runs short during the build | Lower the number of compile jobs at once with `--build-arg JOBS=2` (`docker build`) or `-e JOBS=2` (`docker run`) |
| It stops with `CMake cannot use VTK` or `CMake cannot use RDKit` | The build environment's image is old or was not made completely. Make it again as in [2. Make the build environment](#2-make-the-build-environment) |
| It does not start, with `version 'GLIBC_2.38' not found` or `GLIBCXX_3.4.32' not found` | The computer's glibc or `libstdc++` is too old. Use a newer distribution such as Ubuntu 24.04 or later |
| It cannot mount with FUSE and does not start | Install the `fuse3` package (`fusermount3`). If you cannot, `./MolForma-1.0.0-x86_64.AppImage --appimage-extract-and-run` unpacks it and starts it |
| The 3D view shows “Rendering: QPainter (CPU) · simplified, OpenGL unavailable” at its top left | OpenGL 3.2 or later is unavailable. Check the GPU driver. It may be unavailable over remote desktops and in virtual machines |
| Japanese text shows as boxes | Install a Japanese font on the computer (`noto-fonts-cjk` on Arch Linux, `fonts-noto-cjk` on Ubuntu and Debian, `google-noto-sans-cjk-fonts` on Fedora and so on) |
| The pre-run check cannot find NWChem or the MPI launcher | Check `command -v nwchem mpirun` in a terminal. If NWChem is installed elsewhere, set its path in “3 Run settings” |

## Verified configuration

On 2026-10-28, an AppImage was made with these steps and the following was checked.

- **Build environment**: Ubuntu 24.04, GCC 13.3.0, CMake 3.28.3, Qt 6.4.2, Open Babel 3.1.1, RDKit 2023.09.3 and VTK 9.7.1, with linuxdeploy 1-alpha-20251107-1, linuxdeploy-plugin-qt 1-alpha-20250213-1, appimagetool 1.9.1 and the AppImage runtime (type2-runtime) 20251108. In the same environment, all of MolForma's CTest tests (14) passed too.
- **Arch Linux (the computer it was made on)**: The AppImage started without unpacking (through FUSE). It built a 3D structure from SMILES (Open Babel and the worker), and NWChem 7.3.1 completed an HF calculation of water with 2 MPI processes.
- **Docker containers**: In Ubuntu 24.04, Debian 13, Fedora 39 and Fedora 40 containers, it was started unpacked (`--appimage-extract-and-run`). The containers had desktop-like libraries plus Xvfb with Mesa's software OpenGL. It reported version 1.0.0, built a 3D structure from SMILES, and showed its window with xcb and wrote a screenshot (on Ubuntu 24.04 and Fedora 40, the image also showed the 3D view drawn with VTK).
- **Wayland sessions**: In an environment that looks like a Wayland session (`QT_QPA_PLATFORM` unset, `wayland;xcb` or `wayland`), it started with xcb.
- **Ubuntu 26.04 container (with the `nwchem` package)**: A calculation with NWChem completed.

VTK rendering on a real GPU and the actual display on a Wayland desktop were not checked with the AppImage.

See also: [Installation and required packages by OS](../installation.md), [Installing on Arch Linux](arch_linux.md), [Installing on Ubuntu](ubuntu.md)
