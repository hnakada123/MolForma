# Installation and required packages by OS

The software MolForma needs and how to install it on each OS. MolForma is built from the source code published on GitHub (on Windows, installed into a folder). Calculations use NWChem, installed separately. Choose the page for your OS.

## Steps by OS

| OS | Steps | Contents |
| --- | --- | --- |
| Arch Linux | [Installing on Arch Linux](installation/arch_linux.md) | Making a package from the PKGBUILD and installing it with pacman (with its dependencies), NWChem from the AUR; checking VTK rendering, updating and removing, troubleshooting |
| Ubuntu 26.04 LTS | [Installing on Ubuntu](installation/ubuntu.md) | Development packages and NWChem with apt, building from source, making a deb package and installing it with apt; checking VTK rendering, updating and removing, troubleshooting |
| Windows 11 | [Installing on Windows](installation/windows.md) | Building with Visual Studio (MSVC) and vcpkg, Open Babel, RDKit and VTK also from source, and installing into a folder that holds Qt and the rest; NWChem in Ubuntu on WSL, started through the included batch files; updating and removing, troubleshooting |
| Linux (any distribution) | [Making an AppImage](installation/appimage.md) | Building inside Docker and making an AppImage that holds Qt, Open Babel, RDKit and VTK in one file; starts without installation on Linux with glibc 2.38 or later (Ubuntu 24.04 or later, Debian 13 or later, Fedora 39 or later and others); licenses when distributing it, troubleshooting |

Steps for other operating systems will be added to this page as they become available.

## Required software

On every OS, MolForma uses the following software. Python is not needed at run time.

| Kind | Software | Used for |
| --- | --- | --- |
| Required | A C++20 compiler, CMake 3.21 or later, pkg-config (not with MSVC) | Building |
| Required | Qt6 (6.4 or later; Widgets, and Test for the tests) | The interface |
| Required | Eigen3 | Coordinate and geometry calculations |
| Required | Open Babel 3 (the C++ library with its format plugins and force-field data) | Reading and writing structure files, force-field cleanup, 3D structures from SMILES |
| Optional | RDKit C++ | Strict stereo perception (R/S, E/Z), residue recognition |
| Optional | VTK 9.1 or later built for Qt6 (development package) | Drawing the 3D view with VTK (OpenGL); without it, QPainter (CPU) draws it |
| Calculations | NWChem and, for an MPI build, the MPI it was built with (on Windows, those in WSL) | Running calculations. Building and saving input works without it |

The optional ones are enabled only when CMake finds them at build time. “Help → About MolForma” shows which are enabled. See [3D rendering](features/rendering.md) for how the 3D view is drawn and [“3 Run settings”](features/execution.md) for how NWChem is launched.
