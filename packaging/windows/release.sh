#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Makes the files of a Windows release in build-release (docs/installation/windows.md): a ZIP of the folder
# that "cmake --install" makes, with the licenses and the list of sources, and a tar of the sources of what
# that folder holds. It runs in the Git Bash of Git for Windows, started from the x64 Native Tools Command
# Prompt, on the libraries that page builds: DEV is their folder (C:/dev), QT_PREFIX the MSVC Qt.
#
# The folder holds no OpenSSL backend of Qt, whose license (Apache-2.0) does not go with the GPL version 2 of
# Open Babel, so Qt's TLS is Windows' own (Schannel). It holds none of Microsoft's runtime or compiler DLLs
# either: the GPL's exception for system libraries does not cover those that accompany the program, so they
# come from the computer (README.txt).
#
# The sources must be public, so the checkout must be clean and pushed; DRAFT=1 skips that for a trial.
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
work=$src/build-release
dev=$(cygpath -u "${DEV:-C:/dev}")
qt=$(cygpath -u "${QT_PREFIX:-C:/Qt/6.12.0/msvc2022_64}")
qt_source=$(dirname "$qt")/Src
qt_version=$(basename "$(dirname "$qt")")
version=$(sed -n 's/^project(MolForma VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")
tag=v$version
release=MolForma-$version-windows-x86_64
sources=MolForma-$version-windows-third-party-sources
here=$work/$release
downloads=$work/$sources
repository=https://github.com/hnakada123/MolForma
commit=$(git -C "$src" rev-parse HEAD)
log=$work/logs
vcpkg=$dev/vcpkg
installed=$vcpkg/installed/x64-windows
# The Qt modules that the folder holds something of: qtbase, the SVG support and the translations.
qt_modules="qtbase qtsvg qtimageformats qttranslations"
vtk=9.7.1
vtk_sha256=cae04fd355004cb916a409db79d53a208f1221e975aeacc7540ee67b148ee91a
openbabel_tag=openbabel-3-2-1
rdkit_tag=Release_2026_09_1

fail() {
    echo "release.sh: $*" >&2
    exit 1
}
step() { echo "== $* ($(date +%H:%M:%S))"; }
# The first of the folders that exists: the steps of the page put the sources in DEV, others in DEV/src.
tree() { for folder in "$dev/$1" "$dev/src/$1"; do [ ! -e "$folder" ] || { echo "$folder"; return; }; done; fail "no $1 in $dev"; }
# The version of a package of vcpkg.
vcpkg_version() {
    awk -v name="$1" '$1 == "Package:" { on = $2 == name } on && $1 == "Version:" { print $2; exit }' \
        "$vcpkg/installed/vcpkg/status"
}

if [ -z "${DRAFT:-}" ]; then
    [ -z "$(git -C "$src" status --porcelain)" ] || fail "the checkout has changes"
    [ -n "$(git -C "$src" branch -r --contains "$commit")" ] || fail "$commit is not pushed"
fi
command -v cl >/dev/null || fail "start this from the x64 Native Tools Command Prompt"
[ "$(git -C "$(tree openbabel)" describe --tags)" = "$openbabel_tag" ] || fail "Open Babel is not $openbabel_tag"
[ "$(git -C "$(tree rdkit)" tag --points-at HEAD)" = "$rdkit_tag" ] || fail "RDKit is not $rdkit_tag"
mkdir -p "$log"

build_molforma() {
    [ ! -e "$work/.tested-$commit" ] || [ -n "${DRAFT:-}" ] || return 0
    step "Building and testing MolForma"
    local prefix
    prefix="$(cygpath -m "$qt");$(cygpath -m "$dev")/openbabel-install;$(cygpath -m "$dev")/rdkit-install"
    prefix="$prefix;$(cygpath -m "$dev")/vtk-install;$(cygpath -m "$installed")"
    cmake -S "$src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix" \
        >"$log/configure.log"
    grep -q 'MolForma rendering: VTK' "$log/configure.log" || fail "VTK not found"
    grep -q '^rdkit_DIR:.*=[A-Z]:/' "$work/build/CMakeCache.txt" || fail "RDKit not found"
    cmake --build "$work/build" -j "$NUMBER_OF_PROCESSORS" >"$log/build.log"
    ctest --test-dir "$work/build" --output-on-failure -j 2
    touch "$work/.tested-$commit"
}

install_folder() {
    step "Making the folder"
    rm -rf "$here" "$work/$release.zip"
    cmake --install "$work/build" --prefix "$here" >"$log/install.log" 2>&1
    rm -f "$here/plugins/tls/qopensslbackend.dll"
    # What windeployqt adds of Microsoft's and for software OpenGL: the runtime's installer and Direct3D's
    # shader compilers, and Mesa, which MolForma does not need as it then draws with QPainter.
    rm -f "$here/bin/vc_redist.x64.exe" "$here/bin/d3dcompiler_47.dll" "$here/bin/dxcompiler.dll" \
        "$here/bin/dxil.dll" "$here/bin/opengl32sw.dll"
}

# Copies a license file or folder to licenses/<name>.
license() {
    mkdir -p "$here/licenses/$1"
    cp -R "$2" "$here/licenses/$1/"
}

collect_licenses() {
    step "Collecting the licenses"
    local module package to=$here/licenses
    rm -rf "$to"
    for module in $qt_modules; do
        license "qt/$module" "$qt_source/$module/LICENSES"
    done
    license openbabel "$(tree openbabel)/COPYING"
    license rdkit "$(tree rdkit)/license.txt"
    license vtk "$dev/vtk-install/share/licenses/VTK"
    license eigen3 "$installed/share/eigen3/copyright"
    license zlib "$installed/share/zlib/copyright"
    license libxml2 "$installed/share/libxml2/copyright"
    license boost "$installed/share/boost-iostreams/copyright"
    {
        echo "The libraries in this folder, compiled into molforma.exe or in bin"
        echo
        echo "Qt $qt_version (Core, Gui, Widgets, Network, OpenGL, OpenGL Widgets, Svg, plugins and image formats): GPL-2.0-only, also offered under LGPL-3.0-only and GPL-3.0-only"
        echo "Qt's translations: GPL-3.0-only WITH Qt-GPL-exception-1.0, also offered under the Qt license"
        echo "Open Babel 3.2.1: GPL-2.0-only"
        echo "RDKit ${rdkit_tag#Release_}: BSD-3-Clause"
        echo "VTK $vtk: BSD-3-Clause"
        echo "Boost $(vcpkg_version boost-iostreams): BSL-1.0"
        echo "Eigen $(vcpkg_version eigen3): MPL-2.0"
        echo "zlib $(vcpkg_version zlib): Zlib"
        echo "libxml2 $(vcpkg_version libxml2): MIT"
    } >"$to/packages.txt"
}

# Programs and DLLs link only what the folder or Windows has, and no name of those left out is there.
check_folder() {
    step "Checking the folder"
    local file name found needed=$work/needed
    found=$(find "$here" -type f | grep -iE '/(lib(ssl|crypto)[^/]*|qopenssl[^/]*|vcruntime[^/]*|msvcp[^/]*|concrt[^/]*|vcomp[^/]*|vc_redist[^/]*|d3dcompiler[^/]*|dx(compiler|il)[^/]*|opengl32sw[^/]*)$' || true)
    [ -z "$found" ] || fail "not to be in the folder: $found"
    [ -f "$here/plugins/tls/qschannelbackend.dll" ] || fail "no Schannel backend"
    [ -f "$here/bin/qt.conf" ] || fail "no qt.conf"
    find "$here" -type f \( -name '*.exe' -o -name '*.dll' -o -name '*.obf' \) -print0 |
        xargs -0 -n 20 dumpbin -nologo -dependents |
        tr -d '\r' | awk '/^ +[^ ]+\.(dll|DLL)$/ { print tolower($1) }' | sort -u >"$needed"
    find "$here" -type f -iname '*.dll' -printf '%f\n' | tr 'A-Z' 'a-z' | sort -u >"$work/held"
    comm -23 "$needed" "$work/held" | while read -r name; do
        case $name in
        api-ms-* | ext-ms-* | concrt140.dll | msvcp140*.dll | vcruntime140*.dll | vcomp140.dll) ;;
        *) [ -f "$SYSTEMROOT/System32/$name" ] || fail "$name is neither in the folder nor in Windows" ;;
        esac
    done
    # It starts with no folder of the build tools or libraries on PATH, and with settings of its own. The
    # window appears for a moment: the folder has Qt's Windows platform plugin alone, not offscreen.
    rm -rf "$work/check.png" "$work/check"
    mkdir -p "$work/check"
    env PATH="$(cygpath -u "$SYSTEMROOT/System32")" XDG_CONFIG_HOME="$(cygpath -m "$work/check")" \
        XDG_DATA_HOME="$(cygpath -m "$work/check")" \
        /usr/bin/timeout 120 "$here/bin/molforma.exe" "$(cygpath -m "$src/tests/fixtures/ala_gly_ser.molforma.json")" \
        --screenshot "$(cygpath -m "$work/check.png")" || fail "the folder's molforma.exe failed"
    [ -s "$work/check.png" ] || fail "the folder's molforma.exe made no screenshot"
}

collect_sources() {
    step "Collecting the sources"
    local module package to=$downloads tarball
    rm -rf "$to"
    mkdir -p "$to/qt" "$to/openbabel" "$to/rdkit" "$to/vtk" "$to/vcpkg/ports" "$to/vcpkg/downloads"
    for module in $qt_modules; do
        tar -C "$qt_source" -czf "$to/qt/$module-everywhere-src-$qt_version.tar.gz" "$module"
    done
    git -C "$(tree openbabel)" archive --format=tar.gz --prefix="openbabel-$openbabel_tag/" \
        -o "$(cygpath -m "$to/openbabel/$openbabel_tag.tar.gz")" HEAD
    git -C "$(tree rdkit)" archive --format=tar.gz --prefix="rdkit-$rdkit_tag/" \
        -o "$(cygpath -m "$to/rdkit/$rdkit_tag.tar.gz")" HEAD
    cp "$(tree rdkit)"/External/RingFamilies/RingDecomposerLib-*.tar.gz "$to/rdkit/"
    tar -C "$(tree rdkit)/build/_deps" --exclude=.git -czf "$to/rdkit/better_enums.tar.gz" better_enums-src
    tarball=$(tree "VTK-$vtk.tar.gz")
    echo "$vtk_sha256 *$tarball" | sha256sum -c - >/dev/null || fail "wrong SHA-256: $tarball"
    cp "$tarball" "$to/vtk/"
    # What vcpkg downloaded and its recipes (ports), with their patches.
    for package in eigen3 zlib libxml2 $(awk '$1 == "Package:" && $2 ~ /^boost-/ { print $2 }' "$vcpkg/installed/vcpkg/status" | sort -u); do
        cp -R "$vcpkg/ports/$package" "$to/vcpkg/ports/"
    done
    cp "$vcpkg"/downloads/{boostorg-,libeigen-,madler-zlib-,GNOME-libxml2-}*.tar.gz "$to/vcpkg/downloads/"
}

make_files() {
    step "Making the release files"
    {
        echo "The source of MolForma $version and of what its Windows folder holds"
        echo
        echo "MolForma, with the scripts that make the folder (packaging/windows):"
        echo "  $repository/tree/$commit"
        if git -C "$src" diff --quiet "$tag" "$commit" -- src assets; then
            echo "  Its source code (src, assets) is that of the tag $tag."
        fi
        echo
        echo "The other sources are in $sources.tar:"
        echo "  $repository/releases/tag/$tag"
        echo
        echo "qt/        Qt $qt_version. The DLLs are those of the Qt Online Installer (MSVC 2022 64-bit);"
        echo "           these are the modules' sources that it installs (C:\\Qt\\$qt_version\\Src)."
        echo "openbabel/ Open Babel 3.2.1 (tag $openbabel_tag)"
        echo "rdkit/     RDKit (tag $rdkit_tag), with the two parts that its build downloads"
        echo "vtk/       VTK $vtk from vtk.org (SHA-256 $vtk_sha256)"
        echo "vcpkg/     Boost, Eigen, zlib and libxml2: what vcpkg ($(git -C "$vcpkg" describe --tags)) downloaded,"
        echo "           and its recipes with the patches (ports)"
        echo
        echo "Open Babel, RDKit and VTK were built as docs/installation/windows.md of MolForma tells."
        echo "That page lists the options and the versions of the compiler."
    } >"$here/SOURCES.txt"
    cp "$here/SOURCES.txt" "$downloads/"
    cp "$src/LICENSE" "$src/NOTICE" "$src/packaging/windows/README.txt" "$here/"
    rm -f "$work/$release.zip" "$work/$sources.tar"
    # Windows' tar writes ZIP files; Git for Windows has no zip.
    (cd "$work" && "$(cygpath -u "$SYSTEMROOT/System32/tar.exe")" -a -cf "$release.zip" "$release")
    (cd "$work" && tar -cf "$sources.tar" "$sources" && sha256sum "$release.zip" "$sources.tar")
}

build_molforma
install_folder
collect_licenses
check_folder
collect_sources
make_files
step "Done: $work/$release.zip, $work/$sources.tar"
