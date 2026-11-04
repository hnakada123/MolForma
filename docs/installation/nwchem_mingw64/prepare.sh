#!/usr/bin/env bash
set -euo pipefail
support=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$support/env.sh"
cd "$NWCHEM_WORK"
if [[ -e "$NWCHEM_TOP" ]]; then
  echo "Refusing to overwrite existing source: $NWCHEM_TOP" >&2
  exit 1
fi
curl -fL --retry 3 https://api.github.com/repos/nwchemgit/nwchem/tarball/v7.3.1-release -o nwchem-7.3.1.tar.gz
mkdir "$NWCHEM_TOP"
tar -xzf nwchem-7.3.1.tar.gz --strip-components=1 -C "$NWCHEM_TOP"
cd "$NWCHEM_TOP/src/tools"
curl -fL --retry 3 https://github.com/GlobalArrays/ga/releases/download/v5.9.2/ga-5.9.2.tar.gz -o ga-5.9.2.tar.gz
# This also runs the upstream nxtval.sh transformations.
bash ./get-tools-github
cd "$NWCHEM_TOP"
patch --batch --dry-run -p1 < "$support/mingw64-compatibility.patch"
patch --batch -p1 < "$support/mingw64-compatibility.patch"
touch .mingw64-prepared
sha256sum "$NWCHEM_WORK/nwchem-7.3.1.tar.gz" src/tools/ga-5.9.2.tar.gz > "$NWCHEM_WORK/source-checksums.txt"
echo "Source prepared: $NWCHEM_TOP"
