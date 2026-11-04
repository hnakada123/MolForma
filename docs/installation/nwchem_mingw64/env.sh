#!/usr/bin/env bash
# Source this file from an MSYS2 UCRT64 Bash session.
if [[ "${MSYSTEM:-}" != UCRT64 ]]; then
  echo 'Use the MSYS2 UCRT64 environment (MSYSTEM=UCRT64).' >&2
  return 1
fi
NWCHEM_WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export NWCHEM_TOP="$NWCHEM_WORK/nwchem-7.3.1"
export NWCHEM_TARGET=LINUX64 NWCHEM_MODULES=all
export USE_MPI=y USE_MPIF=y USE_MPIF4=y
export MPI_INCLUDE=/ucrt64/include MPI_LIB=/ucrt64/lib
export LIBMPI='-lmsmpi -lmsmpifec'
export ARMCI_NETWORK=MPI-TS
export BLASOPT=-lopenblas LAPACK_LIB=-lopenblas
export BLAS_SIZE=4 USE_64TO32=y
export FC=gfortran CC=gcc
export USE_SERIALEIGENSOLVERS=y FORCE_MPI_ENV=y
