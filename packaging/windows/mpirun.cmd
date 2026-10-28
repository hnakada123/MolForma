@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
rem MPI launcher of WSL for MolForma on Windows (docs/installation/windows.md). MolForma runs
rem "mpirun.cmd -np N <NWChem executable> input.nw"; this starts "mpirun -np N nwchem input.nw" in the
rem default WSL distribution instead, in the current folder (the job folder).
set WSLENV=OMP_NUM_THREADS/u:OPENBLAS_NUM_THREADS/u:OMP_STACKSIZE/u:%WSLENV%
wsl.exe -e mpirun %1 %2 nwchem %4
