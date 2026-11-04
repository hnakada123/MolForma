#!/usr/bin/env bash
set -euo pipefail
support=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$support/env.sh"
test -f "$NWCHEM_TOP/.mingw64-prepared" || { echo 'Run prepare.sh first.' >&2; exit 1; }
cd "$NWCHEM_TOP/src"
make nwchem_config
make -C include
make 64_to_32
make -j4
test -f "$NWCHEM_TOP/bin/LINUX64/nwchem.exe"
