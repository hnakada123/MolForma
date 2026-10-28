# Installing on Windows

How to build MolForma from source on Windows with Microsoft Visual C++ (MSVC) and install it into a folder. The libraries for the build (RDKit and VTK among them) come from vcpkg and from source; NWChem for calculations goes into Ubuntu on WSL (Windows Subsystem for Linux).

The steps are for Windows 11 (x64) and MolForma 1.0.0. They were checked on 2026-10-28 on Windows 11 Home with Visual Studio Community 2026 ([Verified configuration](#verified-configuration)).

There is no installer or binary download of MolForma for Windows. You build it from the source on GitHub, and `cmake --install` makes a MolForma folder that holds the Qt and Open Babel files it needs. NWChem has no official Windows build, so the Ubuntu package in WSL is used, started through two batch files that come with MolForma (`nwchem.cmd` and `mpirun.cmd`).

## Required software

| Kind | Software | Used for |
| --- | --- | --- |
| Required | Visual Studio 2022 or later (workload “Desktop development with C++”) | The MSVC compiler, Windows SDK, CMake and Ninja |
| Required | Git for Windows | Getting the sources; its Perl is used to build Open Babel |
| Required | “MSVC 2022 64-bit” of Qt 6 (6.4 or later) | The interface (Qt Widgets), the tests (Qt Test), `windeployqt` |
| Required | `eigen3`, `zlib` and `libxml2` from vcpkg | Coordinate and geometry calculations, building Open Babel |
| Required | Open Babel 3.2.1 (built from source) | Reading and writing structure files, force-field cleanup, 3D structures from SMILES |
| Optional | RDKit Release_2026_09_1 (built from source) with Boost from vcpkg | Strict stereo perception (R/S, E/Z), residue recognition |
| Optional | VTK 9.7.1 (built from source for Qt 6) | Drawing the 3D view with VTK (OpenGL); without it, QPainter (CPU) draws it |
| Calculations | Ubuntu on WSL 2 with its package `nwchem` | Running calculations. Building and saving input works without it |

- **Open Babel**: it is not in vcpkg, so it is built from source with MSVC ([3. Build Open Babel](#3-build-open-babel)).
- **RDKit and VTK**: both are optional and are enabled only when CMake finds them at build time ([4. Build RDKit and VTK (optional)](#4-build-rdkit-and-vtk-optional)). Without VTK, QPainter (CPU) draws the 3D view ([3D rendering](../features/rendering.md)).
- **Python**: used neither for building nor at run time.

Run the commands below in “x64 Native Tools Command Prompt for VS” from the Start menu (a command prompt set up for MSVC). The working folder is `C:\dev`.

## 1. Install Visual Studio, Git and Qt

1. Install [Visual Studio](https://visualstudio.microsoft.com/) (Community is fine) with the workload “Desktop development with C++”. It brings MSVC, the Windows SDK and “C++ CMake tools for Windows” (CMake and Ninja).
2. Install [Git for Windows](https://gitforwindows.org/). Its Perl (`C:\Program Files\Git\usr\bin\perl.exe`) is used to build Open Babel.
3. Install Qt with the [Qt Online Installer](https://www.qt.io/download-qt-installer), choosing “MSVC 2022 64-bit” under a Qt 6 version (a Qt account is needed). Below it is in `C:\Qt\6.11.2\msvc2022_64`. Qt for MSVC 2022 also works with the MSVC of Visual Studio 2026.

## 2. Install the libraries with vcpkg

Get [vcpkg](https://vcpkg.io/) and install Eigen, zlib and libxml2.

```bat
cd /d C:\dev
git clone https://github.com/microsoft/vcpkg.git
vcpkg\bootstrap-vcpkg.bat -disableMetrics
vcpkg\vcpkg install --triplet x64-windows eigen3 zlib libxml2[core]
```

The headers, libraries and DLLs go into `C:\dev\vcpkg\installed\x64-windows`.

## 3. Build Open Babel

Get the source of Open Babel 3.2.1, build it and install it into `C:\dev\openbabel-install`.

```bat
cd /d C:\dev
git clone -c core.autocrlf=false -c core.eol=lf --depth 1 --branch openbabel-3-2-1 https://github.com/openbabel/openbabel.git
cd openbabel
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=C:/dev/openbabel-install ^
  -DCMAKE_PREFIX_PATH=C:/dev/vcpkg/installed/x64-windows -DOB_USE_PREBUILT_BINARIES=OFF ^
  -DWITH_INCHI=OFF -DWITH_MAEPARSER=OFF -DWITH_COORDGEN=OFF -DBUILD_GUI=OFF -DENABLE_TESTS=OFF ^
  "-DPERL_EXECUTABLE=C:/Program Files/Git/usr/bin/perl.exe"
cmake --build build -j 4
cmake --install build
```

- `-c core.autocrlf=false -c core.eol=lf`: keeps the LF line endings of the data files. By default Git for Windows converts them to CRLF, and with that data 3D structures of ring molecules (cyclohexane, for example) cannot be generated.
- `-DWITH_INCHI=OFF`: the InChI library in Open Babel 3.2.1 does not link with MSVC. MolForma does not use InChI.
- `-DOB_USE_PREBUILT_BINARIES=OFF` and the others: leave out parts that are not in the Open Babel source or are downloaded while configuring (the GUI, the Maestro format, Coordgen).

`C:\dev\openbabel-install\bin` gets `openbabel-3.dll` and the format plugins (`*.obf`), and `bin\data` the force-field and other data. Open Babel's CMake package (`lib\cmake\openbabel3`) is installed there too, and MolForma finds Open Babel through it (the MSVC build makes no pkg-config file).

## 4. Build RDKit and VTK (optional)

RDKit (strict stereo perception, residue recognition) and VTK (OpenGL drawing of the 3D view) are optional. RDKit is not in vcpkg, and vcpkg's VTK uses vcpkg's own Qt, so both are built from source. To leave them out, skip this section and remove `C:/dev/rdkit-install` and `C:/dev/vtk-install` from `CMAKE_PREFIX_PATH` in [5. Build MolForma](#5-build-molforma). On a 4-core PC, Boost and RDKit took about 40 minutes and VTK about 45 minutes (partly run at the same time).

### RDKit

Install the Boost libraries that RDKit uses with vcpkg.

```bat
cd /d C:\dev
vcpkg\vcpkg install --triplet x64-windows boost-algorithm boost-any boost-assign boost-container boost-crc boost-dynamic-bitset ^
  boost-flyweight boost-format boost-functional boost-graph boost-integer boost-iostreams[core] boost-json ^
  boost-lexical-cast boost-multi-array boost-multiprecision boost-numeric-conversion boost-optional ^
  boost-parameter boost-predef boost-property-map boost-property-tree boost-random boost-range ^
  boost-rational boost-smart-ptr boost-tokenizer boost-unordered boost-variant2
```

Get the source of RDKit Release_2026_09_1, build only its C++ libraries and install them into `C:\dev\rdkit-install`. While configuring, RDKit's CMake downloads Catch2, better-enums and RingDecomposerLib from GitHub.

```bat
cd /d C:\dev
git clone -c core.autocrlf=false -c core.eol=lf --depth 1 --branch Release_2026_09_1 https://github.com/rdkit/rdkit.git
cd rdkit
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=C:/dev/rdkit-install ^
  -DCMAKE_PREFIX_PATH=C:/dev/vcpkg/installed/x64-windows -DRDK_INSTALL_INTREE=OFF ^
  -DRDK_BUILD_PYTHON_WRAPPERS=OFF -DRDK_BUILD_CPP_TESTS=OFF -DRDK_TEST_MMFF_COMPLIANCE=OFF ^
  -DRDK_USE_BOOST_SERIALIZATION=OFF -DRDK_USE_BOOST_IOSTREAMS=OFF -DRDK_USE_BOOST_STACKTRACE=OFF ^
  -DRDK_BUILD_COORDGEN_SUPPORT=OFF -DRDK_BUILD_MAEPARSER_SUPPORT=OFF -DRDK_BUILD_CHEMDRAW_SUPPORT=OFF ^
  -DRDK_BUILD_PUBCHEMSHAPE_SUPPORT=OFF -DRDK_BUILD_DESCRIPTORS3D=OFF -DRDK_BUILD_FREETYPE_SUPPORT=OFF
cmake --build build -j 4
cmake --install build
```

- With MSVC, RDKit makes static libraries (`*.lib`). They are built into the MolForma executable, so the installed folder gets no RDKit DLLs.
- The Python wrappers, fonts for drawing pictures (FreeType), Coordgen, the Maestro and ChemDraw formats, PubChem shape alignment and the 3D descriptors are not built, as MolForma does not use them.

### VTK

Get the source of VTK 9.7.1 and build only the modules MolForma uses, for your Qt (the same configuration as the AppImage). Check that the SHA-256 that `certutil` prints is `cae04fd355004cb916a409db79d53a208f1221e975aeacc7540ee67b148ee91a` before unpacking.

```bat
cd /d C:\dev
curl -L -o VTK-9.7.1.tar.gz https://vtk.org/files/release/9.7/VTK-9.7.1.tar.gz
certutil -hashfile VTK-9.7.1.tar.gz SHA256
tar -xzf VTK-9.7.1.tar.gz
cd VTK-9.7.1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=C:/dev/vtk-install ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/msvc2022_64 -DBUILD_SHARED_LIBS=ON -DVTK_QT_VERSION=6 ^
  -DVTK_BUILD_TESTING=OFF -DVTK_BUILD_EXAMPLES=OFF -DVTK_WRAP_PYTHON=OFF -DVTK_WRAP_JAVA=OFF -DVTK_USE_MPI=OFF ^
  -DVTK_GROUP_ENABLE_Imaging=DONT_WANT -DVTK_GROUP_ENABLE_MPI=DONT_WANT -DVTK_GROUP_ENABLE_Qt=DONT_WANT ^
  -DVTK_GROUP_ENABLE_Rendering=DONT_WANT -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT ^
  -DVTK_GROUP_ENABLE_Views=DONT_WANT -DVTK_GROUP_ENABLE_Web=DONT_WANT ^
  -DVTK_MODULE_ENABLE_VTK_CommonCore=YES -DVTK_MODULE_ENABLE_VTK_CommonDataModel=YES ^
  -DVTK_MODULE_ENABLE_VTK_FiltersSources=YES -DVTK_MODULE_ENABLE_VTK_RenderingCore=YES ^
  -DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2=YES -DVTK_MODULE_ENABLE_VTK_GUISupportQt=YES ^
  -DVTK_MODULE_ENABLE_VTK_InteractionStyle=YES
cmake --build build -j 4
cmake --install build
```

The VTK DLLs go into `C:\dev\vtk-install\bin`. Drawing with VTK also needs a GPU driver with OpenGL 3.2 or later.

## 5. Build MolForma

Get the source and build it in Release mode. Keep the source folder and `build` for updates.

```bat
cd /d C:\dev
git clone https://github.com/hnakada123/MolForma.git
cd MolForma
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  "-DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/msvc2022_64;C:/dev/openbabel-install;C:/dev/rdkit-install;C:/dev/vtk-install;C:/dev/vcpkg/installed/x64-windows"
cmake --build build -j 4
```

Near the end of the output of `cmake -S . -B build …` (configuring), the Open Babel found and the 3D rendering method appear. Without VTK it reads `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)`. RDKit was found if `rdkit_DIR` in the CMake cache (`build\CMakeCache.txt`) is `C:/dev/rdkit-install/lib/cmake/rdkit`.

```
-- Found OpenBabel3, version 3.2.1, in C:/dev/openbabel-install/bin
-- MolForma rendering: VTK 9.7.1 (Qt6), with QPainter fallback
```

To run the tests (optional): they use the DLLs of Qt, Open Babel, VTK and vcpkg, so add their folders to PATH first. They use neither NWChem nor the network.

```bat
set PATH=C:\Qt\6.11.2\msvc2022_64\bin;C:\dev\openbabel-install\bin;C:\dev\vtk-install\bin;C:\dev\vcpkg\installed\x64-windows\bin;%PATH%
ctest --test-dir build --output-on-failure
```

## 6. Install

Install from `build`. Here it goes to the per-user place for apps (`%LOCALAPPDATA%\Programs\MolForma`), which needs no administrator rights.

```bat
cmake --install build --prefix "%LOCALAPPDATA%\Programs\MolForma"
```

`windeployqt` gathers the DLLs, plugins and translations of Qt, and CMake the DLLs needed from Open Babel, VTK and vcpkg, so that MolForma starts from this folder alone (about 190 MB with RDKit and VTK, about 110 MB without them).

| Location | Contents |
| --- | --- |
| `bin\molforma.exe` | The executable (with RDKit built in), with the DLLs of Qt, Open Babel, VTK, zlib and libxml2 beside it |
| `bin\*.obf`, `bin\data\` | Open Babel's format plugins and its force-field and other data |
| `plugins\`, `translations\` | Qt plugins (platform, image formats and others) and the translations of Qt's standard dialogs |
| `nwchem-wsl\` | `nwchem.cmd` and `mpirun.cmd`, which start NWChem in WSL ([7. Install NWChem (WSL)](#7-install-nwchem-wsl)) |
| `share\molforma\` | LICENSE, NOTICE, README and the Markdown of the manual (the manual is also built into the executable) |

To start MolForma from the Start menu, make a shortcut (optional) by running this in PowerShell:

```powershell
$s = (New-Object -ComObject WScript.Shell).CreateShortcut("$env:APPDATA\Microsoft\Windows\Start Menu\Programs\MolForma.lnk")
$s.TargetPath = "$env:LOCALAPPDATA\Programs\MolForma\bin\molforma.exe"
$s.Save()
```

The MSVC runtime (`MSVCP140.dll` and others) is not in the folder; a PC with Visual Studio has it. To use the folder on another PC, install the “Microsoft Visual C++ Redistributable” (x64) there.

## 7. Install NWChem (WSL)

Install NWChem into Ubuntu on WSL. If you do not use WSL yet, run `wsl --install` in PowerShell as administrator, restart, and set up the Ubuntu user name and password. Then run in the command prompt:

```bat
wsl sudo apt update
wsl sudo apt install nwchem
```

On Ubuntu 24.04 this installs NWChem 7.2.2 with the Open MPI build (`nwchem-openmpi`) and the MPI runtime (`openmpi-bin`). The basis set library works as installed, so `~/.nwchemrc` is not needed.

MolForma uses the two batch files in `nwchem-wsl` of the installed folder. Both run NWChem in the default WSL distribution, working in the job folder (which WSL sees under `/mnt/c/…`).

| File | How MolForma calls it | What runs in WSL |
| --- | --- | --- |
| `nwchem.cmd` | `nwchem.cmd input.nw` (launch mode “direct”) | `nwchem input.nw` |
| `mpirun.cmd` | `mpirun.cmd -np 2 <NWChem executable> input.nw` (launch mode “MPI”) | `mpirun -np 2 nwchem input.nw` |

In MolForma, open “Edit → Preferences…” and save these defaults of the run settings. Keep the launch mode “MPI” and 2 MPI processes. `<user>` is your Windows user name (`%LOCALAPPDATA%` cannot be used there; enter the expanded path).

| Setting | Value |
| --- | --- |
| NWChem executable | `C:\Users\<user>\AppData\Local\Programs\MolForma\nwchem-wsl\nwchem.cmd` |
| MPI launcher | `C:\Users\<user>\AppData\Local\Programs\MolForma\nwchem-wsl\mpirun.cmd` |

The defaults are used for new projects and when opening structure files. Change projects you already made the same way in “3 Run settings”. If you add the `nwchem-wsl` folder to the Windows PATH, the names `nwchem` and `mpirun` (the defaults of the run settings) work as they are.

- The batch files pass the thread counts (`OMP_NUM_THREADS`, `OPENBLAS_NUM_THREADS`, `OMP_STACKSIZE`) on to WSL through `WSLENV`. “Additional environment variables” do not reach WSL; if you need them, add their names to the `WSLENV` line of the batch files.
- “Stop” ends `cmd.exe`, `wsl.exe` and `mpirun` and NWChem in WSL together after 2 seconds ([“3 Run settings”](../features/execution.md)).
- WSL sees the job folder (by default `C:\Users\<user>\MolForma\jobs`) much like a network drive, so calculations that write large scratch files are slower than when run on Linux directly.

## 8. Start and check

Start MolForma from “MolForma” in the Start menu or from `%LOCALAPPDATA%\Programs\MolForma\bin\molforma.exe`. The display language follows the Windows language (“Language (after restart)” in “Edit → Preferences…” switches it; it takes effect after a restart).

1. Open “Help → About MolForma” and check that it reads “MolForma 1.0.0” and, with RDKit and VTK installed, “Stereochemistry and residue recognition (RDKit): Enabled” and “3D rendering: VTK (OpenGL)”. Where OpenGL is unavailable (some remote desktops and virtual machines), it reads “3D rendering: QPainter (CPU)”.
2. Type `CC1CCCCC1` (methylcyclohexane) into the search field of the parts palette and check that “Build 3D from SMILES “CC1CCCCC1”” makes a 3D candidate (this uses Open Babel and its data).
3. Open “3 Run settings” and check that the pre-run check finds the NWChem executable and the MPI launcher.
4. Try a calculation on a small molecule; the steps are in [Recipe 1 Building from atoms and bonds](../recipes/atoms_and_bonds.md).

The settings are kept in `%APPDATA%\MolForma\MolForma.ini` and the autosave copies of unsaved work in `%LOCALAPPDATA%\MolForma\autosave` (the Windows registry is not used).

## Updating and removing

### Updating

Update the source, rebuild it and install it again in the same place. As long as Open Babel, RDKit, VTK, Qt and the vcpkg libraries stay the same, you need not configure again as in [5. Build MolForma](#5-build-molforma).

```bat
cd /d C:\dev\MolForma
git pull --ff-only
cmake --build build -j 4
cmake --install build --prefix "%LOCALAPPDATA%\Programs\MolForma"
```

After changing the Qt version, rebuild VTK with that Qt too, delete the `build` of MolForma and configure again with the new Qt folder in `CMAKE_PREFIX_PATH`. Delete the installed folder before installing again, so that no files of the old Qt remain.

### Removing

Delete the installed folder `%LOCALAPPDATA%\Programs\MolForma` and, if you made it, the Start menu shortcut (`%APPDATA%\Microsoft\Windows\Start Menu\Programs\MolForma.lnk`). The settings (`%APPDATA%\MolForma`), autosave copies (`%LOCALAPPDATA%\MolForma`) and calculation jobs (by default `%USERPROFILE%\MolForma\jobs`) remain; delete them by hand if you no longer need them. The build folders under `C:\dev` (MolForma, Open Babel, RDKit, VTK, vcpkg and their `*-install` folders) can also be deleted. Remove NWChem with `wsl sudo apt remove nwchem` followed by `wsl sudo apt autoremove`.

## Troubleshooting

| Symptom | Check / action |
| --- | --- |
| `cmake` or `cl` is not found | Check that you are in “x64 Native Tools Command Prompt for VS”; an ordinary command prompt has no MSVC or CMake set up |
| Configuring MolForma stops with `Could not find a package configuration file provided by "OpenBabel3"`, or with `Qt6` or `Eigen3` not found | Check that `CMAKE_PREFIX_PATH` names the MSVC folder of Qt, `C:/dev/openbabel-install` and `C:/dev/vcpkg/installed/x64-windows`, separated by `;` |
| Building RDKit stops because `boost/iostreams/tee.hpp` is not found | Install `boost-iostreams[core]` with vcpkg; RDKit uses this header even with `-DRDK_USE_BOOST_IOSTREAMS=OFF` |
| Configuring RDKit stops with `Could NOT find Freetype` | Configure again with `-DRDK_BUILD_FREETYPE_SUPPORT=OFF` |
| `MolForma rendering: QPainter (Qt6-enabled VTK development package not found)` appears | Check that `CMAKE_PREFIX_PATH` has `C:/dev/vtk-install` and that VTK was built with the same Qt as MolForma; the line `VTK development dependency unavailable:` before it names what is missing |
| The top left of the 3D view reads “Rendering: QPainter (CPU) · simplified, OpenGL unavailable” | OpenGL 3.2 or later is unavailable. Update the GPU driver; remote desktops and virtual machines may not provide it |
| Configuring Open Babel stops with `Could NOT find Perl` | Set `-DPERL_EXECUTABLE` to the `perl.exe` of Git for Windows |
| Building Open Babel stops with link errors of `inchi.dll` (`Get_inchi_Input_FromAuxInfo` and others) | Configure again with `-DWITH_INCHI=OFF` |
| 3D generation from SMILES or parts says that two atoms overlap at the same position (especially for molecules with rings) | Open Babel's data was checked out with CRLF. Get the source again with the `-c` options of `git clone` in [3. Build Open Babel](#3-build-open-babel), delete `build` and `C:\dev\openbabel-install`, and build again |
| Cleanup says that no force-field parameters fit this structure | Open Babel's data (`bin\data`) is not found. For the installed folder, check that `bin\data` exists; for `molforma.exe` in the build folder, check `C:\dev\openbabel-install\bin\data`. The environment variable `BABEL_DATADIR`, if set, takes precedence |
| `build\molforma.exe` or the tests do not start because `Qt6Core.dll` or another DLL is missing | Executables in the build folder look for the DLLs of Qt and the others in PATH. Run the `set PATH=…` of [5. Build MolForma](#5-build-molforma), or use the installed folder |
| The pre-run check does not find the NWChem executable or the MPI launcher | Check that the paths in “3 Run settings” point to the two batch files in `nwchem-wsl` |
| The calculation fails at once and the log (standard error) shows an error of `wsl` | Check in the command prompt that `wsl nwchem` and `wsl mpirun --version` work. For a WSL distribution that is not the default, add `-d <distribution name>` after `wsl.exe` in the batch files |
| Standard error shows `your 131072x1 screen size is bogus. expect trouble` | A warning of `ps` and similar programs in WSL; it does not affect the calculation |

## Verified configuration

On 2026-10-28 the following was checked with this configuration:

- Configuring and building Open Babel, RDKit, VTK and MolForma with the steps above, with all 14 CTest tests passing (the RDKit checks run instead of being skipped); all 14 also pass in a configuration without RDKit and VTK.
- On a real desktop (GPU OpenGL), the optional test of the VTK rendering path (`test_rendering` with `MOLFORMA_REQUIRE_VTK=1`; see [3D rendering](../features/rendering.md)) passes.
- The folder made by `cmake --install` starts without the folders of Qt, Open Babel or vcpkg in PATH, and generates 3D structures from the SMILES of cyclohexane and methylcyclohexane.
- NWChem installed with apt into Ubuntu 24.04 on WSL starts through `mpirun.cmd` and `nwchem.cmd`; in MolForma's run settings test (the optional test with real NWChem), an HF/STO-3G calculation of water with 2 MPI processes completes with an SCF energy of −74.945628203255 Hartree, and stopping a running calculation also ends `mpirun` and NWChem in WSL.

| Item | Version |
| --- | --- |
| Windows | Windows 11 Home (x64) |
| Visual Studio, MSVC | Community 2026 18.9.2, 14.51.36231 |
| CMake, Ninja (with Visual Studio) | 4.3.1, 1.13.2 |
| Git for Windows | 2.50.1 |
| Qt (MSVC 2022 64-bit) | 6.11.2 |
| Open Babel | 3.2.1 |
| RDKit, Boost (vcpkg) | 2026.09.1, 1.91.0 |
| VTK | 9.7.1 |
| Eigen, zlib, libxml2 (vcpkg) | 5.0.1, 1.3.2, 2.15.3 |
| Ubuntu on WSL | 24.04.4 LTS |
| NWChem, Open MPI (WSL) | 7.2.2, 4.1.6 |

vcpkg was the `vcpkg.exe` that comes with Visual Studio, with the ports of the matching version (tag `2026.06.24`). Windows 10 and Windows on ARM64 were not checked.

Related: [Installation and required packages by OS](../installation.md), [3D rendering](../features/rendering.md), [“3 Run settings”](../features/execution.md)
