#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds molforma_1.1.0_amd64.deb of the MolForma 1.1.0 release from its tag on GitHub, with RDKit,
# VTK and Qt SVG, against the libraries of this computer: the package in
# MolForma-1.1.0-ubuntu-26.04-amd64.zip of https://github.com/hnakada123/MolForma/releases/tag/v1.1.0,
# which also holds the copy of this file it was built with (docs/installation/ubuntu.md). A new
# release changes version and the archive's SHA-256.
# Run it in an empty folder of an Ubuntu 26.04 computer that has the packages of
# docs/installation/ubuntu.md; the deb lands in that folder. JOBS compiles at once (4 by default),
# and CHECK=0 skips the tests (they use neither NWChem nor the network).
set -euo pipefail
version=1.1.0
sha256=d2eea28b399a7480b88c41421a47c74b13327f115900b003f0adadc2182118ba
archive=MolForma-$version.tar.gz

[ -f "$archive" ] ||
    curl -fsSL -o "$archive" "https://github.com/hnakada123/MolForma/archive/refs/tags/v$version.tar.gz"
echo "$sha256  $archive" | sha256sum -c -
rm -rf "MolForma-$version" build
tar -xzf "$archive"

cmake -S "MolForma-$version" -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
# Stop rather than package a build that silently left out VTK, RDKit or Qt SVG.
for feature in VTK RDKIT QTSVG; do
    grep -q -- "-DMOLFORMA_HAS_$feature=1" build/build.ninja ||
        { echo "CMake cannot use $feature (see docs/installation/ubuntu.md)" >&2; exit 1; }
done
cmake --build build --parallel "${JOBS:-4}"
[ "${CHECK:-1}" = 0 ] || ctest --test-dir build --output-on-failure
(cd build && cpack -G DEB)
cp "build/molforma_${version}_amd64.deb" .
