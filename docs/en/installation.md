# Installation and required packages by OS

The software MolForma needs and how to install it on each OS. MolForma is built from the source code published on GitHub. Calculations use NWChem, installed separately. Choose the page for your OS.

## Steps by OS

| OS | Steps | Contents |
| --- | --- | --- |
| Arch Linux | [Installing on Arch Linux](installation/arch_linux.md) | Making a package from the PKGBUILD and installing it with pacman (with its dependencies), NWChem from the AUR; checking VTK rendering, updating and removing, troubleshooting |

Steps for other operating systems will be added to this page as they become available.

## Required software

On every OS, MolForma uses the following software. Python is not needed at run time.

| Kind | Software | Used for |
| --- | --- | --- |
| Required | A C++20 compiler, CMake 3.21 or later, pkg-config | Building |
| Required | Qt6 (6.4 or later; Widgets, and Test for the tests) | The interface |
| Required | Eigen3 | Coordinate and geometry calculations |
| Required | Open Babel 3 (the C++ library with its format plugins and force-field data) | Reading and writing structure files, force-field cleanup, 3D structures from SMILES |
| Optional | RDKit C++ | Strict stereo perception (R/S, E/Z), residue recognition |
| Optional | VTK 9.1 or later built for Qt6 (development package) | Drawing the 3D view with VTK (OpenGL); without it, QPainter (CPU) draws it |
| Calculations | NWChem and, for an MPI build, the MPI it was built with | Running calculations. Building and saving input works without it |

The optional ones are enabled only when CMake finds them at build time. “Help → About MolForma” shows which are enabled. See [3D rendering](features/rendering.md) for how the 3D view is drawn and [“3 Run settings”](features/execution.md) for how NWChem is launched.
