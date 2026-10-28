@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem NWChem of WSL for MolForma on Windows (docs/installation/windows.md): runs "nwchem" of the default
rem WSL distribution in the current folder (the job folder, which WSL sees under /mnt), passing on the
rem thread counts that MolForma sets.
set WSLENV=OMP_NUM_THREADS/u:OPENBLAS_NUM_THREADS/u:OMP_STACKSIZE/u:%WSLENV%
wsl.exe -e nwchem %*
