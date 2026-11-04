# Building NWChem on Windows (64-bit MinGW / GNU Fortran)

How to build NWChem from source so that it runs on Windows, with the UCRT64 environment of MSYS2 and GNU Fortran. The result is a native 64-bit Windows executable; WSL is not used at run time.

On 2026-10-29, NWChem 7.3.1 was built with all standard modules, and HF, DFT and TCE calculations and 2-process runs with Microsoft MPI were checked. This page collects the settings and changes of that build. For publication, the working folder is `C:\NWChem` throughout. On 2026-11-04, runs and stopping from MolForma were checked too ([9. Using it from MolForma](#9-using-it-from-molforma)).

These steps and the attached patch are unofficial. The developers of NWChem and Global Arrays have not reviewed or endorsed them ([License](#license)).

For building MolForma itself, see [Installing on Windows](windows.md). This page installs NWChem, the calculation engine. MolForma built with MSVC and NWChem built with GNU Fortran work together.

## Verified configuration

| Item | Configuration / version |
| --- | --- |
| Target | Windows x64 |
| NWChem | 7.3.1, tag `v7.3.1-release` |
| MSYS2 environment | UCRT64 |
| GCC / GNU Fortran | 16.2.0 (package `16.2.0-6`) |
| GNU Make | 4.4.1 |
| OpenBLAS | 0.3.34 (LP64, 32-bit integer arguments) |
| Global Arrays (GA) | 5.9.2 |
| Microsoft MPI | Runtime 10.1.1, MSYS2 development package `10.1.1-26` |
| NWChem modules | `NWCHEM_MODULES=all` |
| ARMCI network | `MPI-TS` |
| Eigensolvers | `USE_SERIALEIGENSOLVERS=y`, no ScaLAPACK |

`all` is the set of standard modules that NWChem selects. It includes HF, DFT, TCE, NWPW, MD and others, but does not turn on every external interface such as Python or NBO. Building every module is also not the same as checking every feature; what was checked is listed below.

Intel Fortran and Visual Studio are not needed for this NWChem build, and it can coexist with an existing Intel Fortran setup.

## What you need and the working folder

You need an internet connection, a Windows account that may install Microsoft MPI, and free disk space for the sources, the toolchain and intermediate files; allow several GB or more. The build was done on a PC with about 32 GB of memory and 4 parallel build jobs, which is not a minimum requirement.

Use a short working path without spaces or non-ASCII characters. This page uses this layout:

```text
C:\NWChem\
├─ toolchain\msys64\       MSYS2, GNU Fortran, OpenBLAS
├─ nwchem-7.3.1\           NWChem source and build results
├─ support\                The attached files of this page
└─ calculations\           Working folders of calculations
```

Below, **commands for PowerShell** and **commands for Bash in MSYS2 UCRT64** are kept apart. The path `C:\NWChem` is written `/c/NWChem` in Bash.

## 1. Save the attached files

Download [the build support files (ZIP)](../../installation/nwchem_mingw64.zip) and unpack them into `C:\NWChem\support`, so that `env.sh`, `prepare.sh` and `build.sh` are directly in `support`.

| File | Purpose |
| --- | --- |
| [env.sh](../../installation/nwchem_mingw64/env.sh) | The build settings that worked |
| [prepare.sh](../../installation/nwchem_mingw64/prepare.sh) | Getting the sources, GA's standard preparation, applying the compatibility patch |
| [build.sh](../../installation/nwchem_mingw64/build.sh) | Configuring, converting the integer interface, compiling and linking |
| [mingw64-compatibility.patch](../../installation/nwchem_mingw64/mingw64-compatibility.patch) | The changes for MinGW added in this build |
| [nwchem.cmd](../../installation/nwchem_mingw64/nwchem.cmd) | Starting one process from Windows |
| [nwchem-mpi.cmd](../../installation/nwchem_mingw64/nwchem-mpi.cmd) | Starting with MPI from Windows |
| [water_opt.nw](../../installation/nwchem_mingw64/water_opt.nw) | Input for a geometry optimization of water |
| [toolchain-packages.txt](../../installation/nwchem_mingw64/toolchain-packages.txt) | The MSYS2 packages at build time |

The ZIP holds no compiler, no NWChem and no Microsoft MPI installer; the steps below get each from its official source. When you edit a `.sh` file, save it as UTF-8 with LF line endings.

## 2. Install MSYS2 and GNU Fortran

This build unpacked the MSYS2 base archive inside the working folder. For the same layout, run this in **PowerShell**:

```powershell
New-Item -ItemType Directory -Force C:\NWChem\toolchain | Out-Null
Set-Location C:\NWChem
curl.exe -fL https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-base-x86_64-latest.tar.xz -o msys2-base.tar.xz
tar.exe -xf .\msys2-base.tar.xz -C .\toolchain

$env:MSYSTEM = 'UCRT64'
& .\toolchain\msys64\usr\bin\bash.exe -l
```

Then update and install packages in the **Bash** that started.

```bash
pacman -Syu
```

If the update asks you to close MSYS2, close it, open it again with the last two PowerShell lines above, and run `pacman -Syu` again. Then install these packages:

```bash
pacman -S --needed make patch diffutils git autoconf automake libtool \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-gcc-fortran \
  mingw-w64-ucrt-x86_64-msmpi \
  mingw-w64-ucrt-x86_64-openblas

echo "$MSYSTEM"
which gcc gfortran make
gfortran --version
gfortran -dumpmachine
```

Check that `MSYSTEM` is `UCRT64`, `gfortran` is `/ucrt64/bin/gfortran` and the target is `x86_64-w64-mingw32`. Make may be `/usr/bin/make`. Do not mix in other Fortran compilers from `/mingw64` or `/usr/bin`.

MSYS2 is a rolling release: the `latest` archive and the current package repositories may bring versions other than those in the table. To record your environment:

```bash
pacman -Q > /c/NWChem/installed-packages.txt
```

If you install MSYS2 elsewhere, change the toolchain path in the start scripts below and in MolForma's run environment to match.

## 3. Install the Microsoft MPI runtime

MSYS2's `mingw-w64-ucrt-x86_64-msmpi` provides the headers and libraries for building. Running NWChem also needs the Microsoft MPI runtime on the Windows side.

In another **PowerShell**, run:

```powershell
Set-Location C:\NWChem
curl.exe -fL https://github.com/microsoft/Microsoft-MPI/releases/download/v10.1.1/msmpisetup.exe -o .\toolchain\msmpisetup.exe
Get-AuthenticodeSignature .\toolchain\msmpisetup.exe
Start-Process -FilePath .\toolchain\msmpisetup.exe -Wait
```

Go on with the installation once the signature status is `Valid`; allow the administrator prompt if Windows shows one. This build used the runtime `msmpisetup.exe`, without Microsoft's separate SDK.

After installing, check:

```powershell
Test-Path 'C:\Windows\System32\msmpi.dll'
& 'C:\Program Files\Microsoft MPI\Bin\mpiexec.exe' -help
```

An executable built with MPI needs the MPI runtime even when started directly as one process.

## 4. Get NWChem and Global Arrays and apply the changes

In **Bash**, run:

```bash
cd /c/NWChem
bash support/prepare.sh
```

The script prepares the sources in this order:

1. Gets the source of NWChem's tag `v7.3.1-release` through the GitHub API and unpacks it into `nwchem-7.3.1`.
2. Gets the release archive of Global Arrays 5.9.2 into `src/tools`.
3. Runs NWChem's `get-tools-github`, which unpacks GA and applies NWChem's standard preprocessing with `nxtval.sh`.
4. Checks the compatibility patch of this page with a dry run, then applies it.

If `nwchem-7.3.1` already exists, the script stops without overwriting it; it is not meant to update an existing setup.

**Apply the patch only after GA's standard preprocessing.** Do not apply it to raw GA sources or twice to the same sources. If you leave getting GA to Make and patch first, the files to change are missing, or the preprocessing and the changes overlap.

The main changes to the official sources are:

| Target | Change |
| --- | --- |
| `bzero` / `bcopy` calls in GA | Handled with `memset` / `memmove` |
| Some random number functions in GA and NWChem | A `srand` / `rand` path for MinGW, or NWChem's existing portable random function |
| Getting the host name | `MPI_Get_processor_name` where it is needed |
| Free disk space in GA | The output arguments of `GetDiskFreeSpace` match the Windows API's `DWORD` |
| GA's `nice()` wrapper | Returns `ENOSYS` as unsupported on MinGW |
| NWChem's source for MD sockets | Includes the declaration that `GA_Error` needs |

These are local changes for this build, not official patches adopted by NWChem or GA. Where the random number path changes, the sequence differs from the original implementation; numerical checks of stochastic methods were not part of this work.

## 5. Build all standard modules

The attached `env.sh` sets these main environment variables:

```bash
export NWCHEM_TOP=/c/NWChem/nwchem-7.3.1
export NWCHEM_TARGET=LINUX64
export NWCHEM_MODULES=all
export FC=gfortran CC=gcc

export USE_MPI=y USE_MPIF=y USE_MPIF4=y
export MPI_INCLUDE=/ucrt64/include
export MPI_LIB=/ucrt64/lib
export LIBMPI='-lmsmpi -lmsmpifec'
export FORCE_MPI_ENV=y
export ARMCI_NETWORK=MPI-TS

export BLASOPT=-lopenblas LAPACK_LIB=-lopenblas
export BLAS_SIZE=4 USE_64TO32=y
export USE_SERIALEIGENSOLVERS=y
```

`NWCHEM_TARGET=LINUX64` is the name of NWChem's internal target used for this MinGW build; the result is a Windows PE x86-64 executable, not a Linux binary.

This OpenBLAS is LP64, so `BLAS_SIZE=4`, `USE_64TO32=y` and `make 64_to_32` match NWChem's integer interface to it. `USE_SERIALEIGENSOLVERS=y` selects a configuration without ScaLAPACK; it does not turn off MPI parallel runs of NWChem as a whole.

Build in **Bash**:

```bash
cd /c/NWChem
set -o pipefail
bash support/build.sh 2>&1 | tee build.log
```

`build.sh` runs:

```bash
source /c/NWChem/support/env.sh
cd "$NWCHEM_TOP/src"
make nwchem_config
make -C include
make 64_to_32
make -j4
```

`make -C include` prepares the headers needed, and the main build follows the integer interface conversion. NWChem's Make also configures and builds GA. With `tee`, `pipefail` keeps a failed build from being hidden by the success of the logging command.

With all modules there are many sources: on the PC used, the integer interface conversion alone took tens of minutes. When the screen shows little output for a while, check the log and the processes. `-j4` is the number of parallel compile jobs, unrelated to the number of MPI processes in calculations.

If the build stops, fix the cause and, **as long as the settings are unchanged**, continue with:

```bash
source /c/NWChem/support/env.sh
cd "$NWCHEM_TOP/src"
set -o pipefail
make -j4 2>&1 | tee /c/NWChem/build-resume.log
```

To change the compiler, MPI, ARMCI or integer size, prepare the sources again in another working folder. Do not mix intermediate files of different configurations.

## 6. Check the result and the run environment

A successful build makes:

```text
C:\NWChem\nwchem-7.3.1\bin\LINUX64\nwchem.exe
```

In **Bash** you can check the format and the DLLs it depends on:

```bash
objdump -f /c/NWChem/nwchem-7.3.1/bin/LINUX64/nwchem.exe
objdump -p /c/NWChem/nwchem-7.3.1/bin/LINUX64/nwchem.exe | grep 'DLL Name'
```

This build was `pei-x86-64`. The main DLLs it depends on are `libgfortran-5.dll`, `libgcc_s_seh-1.dll`, `libopenblas.dll`, `libwinpthread-1.dll`, `libmsmpifec.dll` and `msmpi.dll`. Copying only `nwchem.exe` to another PC does not work.

The attached start scripts set the DLL search path and these data libraries inside their own process; you need not change the Windows-wide PATH.

| Variable | Set to |
| --- | --- |
| `PATH` | `toolchain\msys64\ucrt64\bin`, Microsoft MPI's `Bin`, the original PATH |
| `NWCHEM_BASIS_LIBRARY` | `C:/NWChem/nwchem-7.3.1/src/basis/libraries/` |
| `NWCHEM_NWPW_LIBRARY` | `C:/NWChem/nwchem-7.3.1/src/nwpw/libraryps/` |
| `OPENBLAS_NUM_THREADS` | `1`, only if not set |

Give the library paths with `/` separators and a trailing `/`. With backslashes, reading the basis set library failed in this build; the form above worked. For MD force-field data, set its location separately, for example in an `nwchemrc` in the working directory of the calculation.

## 7. Check it with a geometry optimization of water

Save this input as `water_opt.nw` (the same file is attached). It optimizes neutral singlet water at RHF/6-31G*.

```text
echo
start water_opt
title "Water geometry optimization: RHF/6-31G*"
memory total 512 mb
charge 0

geometry units angstrom
  O  0.000000  0.000000  0.000000
  H  0.800000  0.000000  0.600000
  H -0.800000  0.000000  0.600000
end

basis
  * library 6-31g*
end

scf
  rhf
  singlet
  thresh 1.0e-8
  maxiter 100
end

driver
  tight
  maxiter 50
  xyz water_opt
end

task scf optimize
```

In **PowerShell**, make a new calculation folder and run it:

```powershell
New-Item -ItemType Directory -Force C:\NWChem\calculations\water | Out-Null
Copy-Item C:\NWChem\support\water_opt.nw C:\NWChem\calculations\water\
Set-Location C:\NWChem\calculations\water
& C:\NWChem\support\nwchem.cmd water_opt.nw > water_opt.out
$LASTEXITCODE
Select-String -Path water_opt.out -Pattern 'Optimization converged','Total SCF energy','Total times'
```

Check that the exit code is `0` and the log has `Optimization converged`. The one-process run converged in 5 steps after the starting structure (step 0), with these results:

| Item | Result |
| --- | ---: |
| Final SCF energy | −76.010746508443 hartree |
| O–H bond lengths (both) | 0.94732 Å |
| H–O–H angle | 105.49982° |
| Wall time shown by NWChem | about 1.6 s |
| Final structure | `water_opt-005.xyz` |

The time and the last digits depend on the environment. This example is a geometry optimization without frequencies.

To try 2 processes, use another calculation folder:

```powershell
New-Item -ItemType Directory -Force C:\NWChem\calculations\water-mpi2 | Out-Null
Copy-Item C:\NWChem\support\water_opt.nw C:\NWChem\calculations\water-mpi2\
Set-Location C:\NWChem\calculations\water-mpi2
& C:\NWChem\support\nwchem-mpi.cmd 2 water_opt.nw > water_opt.out
$LASTEXITCODE
```

This is a parallel optimization for you to run. The 2-process numerical test recorded for this build is the HF energy and gradient of the official QA in the next section; the optimization results above are from one process.

## 8. What was checked and what was not

| Test | Result |
| --- | --- |
| MPI calls from GNU Fortran | Finished normally |
| Creating, writing and reading Global Arrays distributed arrays | Succeeded with 1 and 2 processes |
| Official QA `h2o`: HF/6-31G* energy and gradient | Finished normally with 1 and 2 processes; the SCF energy −76.010538615916 hartree matches the reference output |
| Official QA `dft_he_mvs15` | The DFT energy −2.867079060595 hartree matches the reference output |
| Official QA `tce_h2o` | 11 printed correlation energies compared; the largest difference from the reference is about 5.31×10⁻¹³ hartree |
| The water RHF/6-31G* optimization of this page | Converged with 1 process |

The TCE methods run were LCCD, LCCSD, CCD, CCSD, CCSDT, QCISD, CISD, CISDT, MBPT(2) and MBPT(3); MBPT(2) is printed twice, hence 11 values.

The full QA suite, numerical results of NWPW, MD and stochastic methods, multi-node MPI and ScaLAPACK were not checked. MD coupling over Unix sockets is not available in this MinGW configuration. Compiler warnings from old sources remain.

The published scripts arrange the settings that worked so that they are easy to move. Their shell syntax, applying the patch, and that the 15 patched files match the sources actually built were checked; a full new build from fresh downloads with the published layout was not repeated.

## 9. Using it from MolForma

These are example settings that connect MolForma's run settings to this NWChem. First check that the command-line calculation above succeeds.

On 2026-11-04, MolForma's run settings test (the optional test with real NWChem) was run with this native NWChem and MolForma for Windows (built with MSVC): an HF/STO-3G calculation of water with 2 MPI processes completed with an SCF energy of −74.945628203255 hartree, and stopping a running calculation ended all the processes of `mpiexec`, `smpd` and NWChem. That test uses the default MPI launcher name `mpirun`, so `mpiexec.exe` was copied as `mpirun.exe` for it; it is the same program as `mpiexec.exe` named directly in the settings below.

So that NWChem finds its DLLs and data, set the environment in **PowerShell** and start MolForma from there. Change the last executable path to where MolForma is installed.

```powershell
$env:PATH = "C:\NWChem\toolchain\msys64\ucrt64\bin;C:\Program Files\Microsoft MPI\Bin;$env:PATH"
$env:NWCHEM_BASIS_LIBRARY = 'C:/NWChem/nwchem-7.3.1/src/basis/libraries/'
$env:NWCHEM_NWPW_LIBRARY = 'C:/NWChem/nwchem-7.3.1/src/nwpw/libraryps/'
& "$env:LOCALAPPDATA\Programs\MolForma\bin\molforma.exe"
```

These settings apply to that PowerShell and the processes started from it, not to a MolForma that is already running.

In MolForma's “3 Run settings”, set:

| Setting | Example |
| --- | --- |
| NWChem executable | `C:\NWChem\nwchem-7.3.1\bin\LINUX64\nwchem.exe` |
| Launch mode | Direct at first, MPI for parallel calculations |
| MPI launcher | `C:\Program Files\Microsoft MPI\Bin\mpiexec.exe` |
| MPI processes | `2` |
| OpenMP / BLAS threads | `1` each at first |

For MPI runs MolForma passes `-np <processes> <executable> input.nw`; Microsoft MPI 10.1.1 accepts `-np`. The table names NWChem's actual `.exe`. The `nwchem-mpi.cmd` of this page takes a different argument form (`processes input-file`), so do not give it as MolForma's MPI launcher, and do not combine it with the `mpirun.cmd` for WSL either.

“Stop” ends `mpiexec.exe` together with the `smpd.exe` and `nwchem.exe` it started, after 2 seconds.

Let the pre-run check test the paths, and start with a small calculation such as water. The log and the optimized XYZ can be loaded into MolForma's result viewer. For the settings screen, see [“3 Run settings”](../features/execution.md).

## Troubleshooting

| Symptom | Check / action |
| --- | --- |
| `gfortran` is missing, or another version is used | Check `MSYSTEM=UCRT64`, `which gfortran` and `gfortran -dumpmachine` |
| Getting packages fails again and again | Check MSYS2's mirror and the network; in this build, changing the mirror helped once |
| `pacman` is locked | First check that no other update or installation is running; do not remove the lock of a running one |
| GA stops on Unix dependencies such as `sys/mman.h` | Check `ARMCI_NETWORK=MPI-TS`; the `MPI-PR` configuration tried had this kind of problem |
| The patch reports `Hunk FAILED` | Check that the sources are NWChem 7.3.1 and GA 5.9.2, after the standard preprocessing and before the patch; do not go on past the failure |
| Errors on `random`, `srandom`, `bzero`, `gethostname` and the like | Check that the compatibility patch was applied to sources of the versions given |
| A header is not found | Run `make -C include` after `make nwchem_config` |
| `msmpi.dll` is missing | Install the Microsoft MPI runtime for Windows, not only the MSYS2 package |
| `libgfortran-5.dll`, `libopenblas.dll` or similar is missing | Check that `ucrt64\bin` is in the PATH of the running process; use the attached start scripts first |
| `failed to open basis library` | Check that `NWCHEM_BASIS_LIBRARY` names an existing location, with `/` separators and a trailing `/` |
| The source archive shows `not a git repository` | The archive has no `.git`; this build showed it when getting version information, and the final link and numerical tests still succeeded. Do not overlook other compile errors |
| It works on the command line but not from MolForma | Check the PATH and data library settings MolForma inherited at startup, and that the native `.exe` is combined with Microsoft MPI |

## License

NWChem is published by Pacific Northwest National Laboratory (Battelle Memorial Institute) under the [Educational Community License 2.0](https://opensource.org/licenses/ECL-2.0). Global Arrays is published by Battelle Memorial Institute under [BSD-3-Clause](https://opensource.org/licenses/BSD-3-Clause). Both allow getting the sources and changing and building them yourself.

- These steps and scripts describe how you get NWChem, Global Arrays, MSYS2 and Microsoft MPI from their official sources and build them; they contain none of their sources or executables.
- The attached `mingw64-compatibility.patch` contains a few lines around each changed place. Its header names the copyright holders and licenses of the changed files and states that it is unofficial. Each change is offered under the license of the file it changes.
- If you give the built `nwchem.exe` or DLLs to others, follow the license of everything you include: NWChem (keep the ECL 2.0 notices and mark the changed files), Global Arrays, OpenBLAS, the GCC runtime libraries and the redistribution terms of Microsoft MPI, among others.
- When publishing results obtained with NWChem, cite NWChem ([NWChem citation](https://nwchemgit.github.io/Citation.html)).

## References

- [NWChem: Compiling NWChem from source](https://nwchemgit.github.io/Compiling-NWChem.html)
- [Source of NWChem 7.3.1](https://github.com/nwchemgit/nwchem/tree/v7.3.1-release)
- [MSYS2](https://www.msys2.org/)
- [MSYS2 environments](https://www.msys2.org/docs/environments/)
- [Global Arrays 5.9.2](https://github.com/GlobalArrays/ga/releases/tag/v5.9.2)
- [Microsoft MPI 10.1.1](https://github.com/microsoft/Microsoft-MPI/releases/tag/v10.1.1)

The version numbers and numerical results on this page come from the records of this local build. The general description of the official procedure and the local changes this build needed are kept apart.
