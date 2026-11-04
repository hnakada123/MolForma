@echo off
setlocal
set "NWCHEM_ROOT=%~dp0..\nwchem-7.3.1"
set "PATH=%~dp0..\toolchain\msys64\ucrt64\bin;C:\Program Files\Microsoft MPI\Bin;%PATH%"
set "NWCHEM_DATA=%NWCHEM_ROOT:\=/%"
set "NWCHEM_BASIS_LIBRARY=%NWCHEM_DATA%/src/basis/libraries/"
set "NWCHEM_NWPW_LIBRARY=%NWCHEM_DATA%/src/nwpw/libraryps/"
if not defined OPENBLAS_NUM_THREADS set "OPENBLAS_NUM_THREADS=1"
"%NWCHEM_ROOT%\bin\LINUX64\nwchem.exe" %*
exit /b %errorlevel%
