#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Makes the files of a macOS release in build-release (docs/installation/macos.md): a ZIP of the disk
# image with the licenses and the list of sources, and a tar of the sources of what the image holds.
#
# Homebrew's Qt links OpenSSL, whose license (Apache-2.0) does not go with the GPL version 2 of Open
# Babel, so qtbase and qtsvg are built here without it, from the sources, patches and options of the
# formulae of the installed versions; Qt's TLS is then macOS's SecureTransport. Everything else comes
# from Homebrew as installed, and its sources are those of the installed versions too, not of Homebrew's
# newest formulae. Steps that finished are not repeated: delete build-release to start again.
#
# The sources must be public, so the checkout must be clean and pushed; DRAFT=1 skips that for a trial.
set -euo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
work=$src/build-release
qt=$work/qt
downloads=$work/sources
version=$(sed -n 's/^project(MolForma VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")
architecture=$(uname -m)
tag=v$version
image=MolForma-$version-$architecture.dmg
release=MolForma-$version-macos-$architecture
sources=MolForma-$version-macos-third-party-sources
repository=https://github.com/hnakada123/MolForma
commit=$(git -C "$src" rev-parse HEAD)
jobs=$(sysctl -n hw.ncpu)
log=$work/logs
export COPYFILE_DISABLE=1 # No ._ files in the tar.

fail() {
    echo "release.sh: $*" >&2
    exit 1
}
keg() { readlink -f "$(brew --prefix)/opt/$1"; }
uuid() { otool -l "$1" | awk '/LC_UUID/ { getline; getline; print $2; exit }'; }
step() { echo "== $* ($(date +%H:%M:%S))"; }

if [ -z "${DRAFT:-}" ]; then
    [ -z "$(git -C "$src" status --porcelain)" ] || fail "the checkout has changes"
    [ -n "$(git -C "$src" branch -r --contains "$commit")" ] || fail "$commit is not pushed"
fi
[ -d "$(brew --prefix qttranslations)" ] || fail "brew install qttranslations"
mkdir -p "$downloads" "$log"

# Downloads url to folder unless the file with that SHA-256 is there; mirrors are the formula's.
fetch() {
    local url=$1 sha256=$2 folder=$3 formula=$4 file
    file=$folder/$(basename "${url%%\?*}")
    if [ ! -f "$file" ] || [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" != "$sha256" ]; then
        rm -f "$file"
        for location in "$url" $(tr -d '"' <"$formula" | awk '$1 == "mirror" { print $2 }'); do
            curl -fsSL --retry 3 -o "$file" "$location" && break
        done
        [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" = "$sha256" ] || fail "wrong SHA-256: $url"
    fi
    echo "$url" >>"$folder/.urls"
}

# The source archive and patches of an installed package (its SBOM has them), the resources that go into
# its build (the other resources of the formulae are test data) and its formula, the recipe of its build.
fetch_package() {
    local formula=$1 folder=$downloads/$1 from url sha256
    from=$(keg "$formula")
    mkdir -p "$folder"
    rm -f "$folder/.urls"
    cp "$from/.brew/$formula.rb" "$folder/"
    {
        jq -r '.packages[] | select(.SPDXID | test("^SPDXRef-(Archive|Patch)-"))
               | "\(.downloadLocation) \(.checksums[0].checksumValue)"' "$from/sbom.spdx.json"
        tr -d '"' <"$from/.brew/$formula.rb" | awk '
            /^ *resource (better_enums|gobject-introspection) / { on = 1 }
            on && $1 == "url" { url = $2 }
            on && $1 == "sha256" { print url, $2; on = 0 }'
    } | while read -r url sha256; do
        fetch "$url" "$sha256" "$folder" "$folder/$formula.rb"
    done
}
# The first file of a package's folder: its source archive.
archive() { echo "$downloads/$1/$(basename "$(sed -n 's/?.*//; 1p' "$downloads/$1/.urls")")"; }

# Qt as the formulae of qtbase and qtsvg build it, in Qt's own folder layout and without OpenSSL.
build_qt() {
    [ ! -e "$qt/.done" ] || return 0
    step "Building Qt without OpenSSL"
    local tree=$work/qt-build
    rm -rf "$tree" "$qt"
    mkdir -p "$tree"
    fetch_package qtbase
    fetch_package qtsvg
    tar -xf "$(archive qtbase)" -C "$tree" --strip-components=1
    for patch in "$downloads"/qtbase/*.patch; do
        patch -s -d "$tree" -p1 -i "$patch"
    done
    (cd "$tree/src/3rdparty" && rm -rf double-conversion freetype harfbuzz-ng libjpeg libpng md4c pcre2 sqlite xcb zlib)
    cmake -S "$tree" -B "$tree/build" -G Ninja -Wno-dev \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$qt" -DCMAKE_FIND_FRAMEWORK=FIRST \
        -DCMAKE_PREFIX_PATH="$(brew --prefix);$(brew --prefix icu4c@79)" \
        -DFEATURE_sql_mysql=OFF -DFEATURE_sql_odbc=OFF -DFEATURE_sql_psql=OFF \
        -DFEATURE_openssl=OFF \
        -DFEATURE_pkg_config=ON -DFEATURE_system_doubleconversion=ON -DFEATURE_system_freetype=ON \
        -DFEATURE_system_harfbuzz=ON -DFEATURE_system_jpeg=ON -DFEATURE_system_libb2=ON \
        -DFEATURE_system_pcre2=ON -DFEATURE_system_png=ON -DFEATURE_system_sqlite=ON \
        -DFEATURE_system_zlib=ON -DQT_ALLOW_SYMLINK_IN_PATHS=ON -DFEATURE_relocatable=ON \
        -DQT_NO_APPLE_SDK_AND_XCODE_CHECK=ON -DFEATURE_accessibility_atspi_bridge=OFF \
        >"$log/qtbase-configure.log"
    cmake --build "$tree/build" -j "$jobs" >"$log/qtbase-build.log"
    cmake --install "$tree/build" >"$log/qtbase-install.log"
    tree=$work/qtsvg-build
    rm -rf "$tree"
    mkdir -p "$tree"
    tar -xf "$(archive qtsvg)" -C "$tree" --strip-components=1
    cmake -S "$tree" -B "$tree/build" -G Ninja -Wno-dev -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$qt" -DCMAKE_INSTALL_PREFIX="$qt" -DCMAKE_FIND_FRAMEWORK=FIRST \
        -DQT_NO_APPLE_SDK_AND_XCODE_CHECK=ON >"$log/qtsvg-configure.log"
    cmake --build "$tree/build" -j "$jobs" >"$log/qtsvg-build.log"
    cmake --install "$tree/build" >"$log/qtsvg-install.log"
    mkdir -p "$qt/translations"
    cp "$(brew --prefix qttranslations)/share/qt/translations/qtbase_ja.qm" "$qt/translations/"
    # The same libraries as Homebrew's Qt but for OpenSSL.
    for module in QtCore QtGui QtNetwork; do
        ! otool -L "$qt/lib/$module.framework/$module" | grep -E 'libssl|libcrypto' || fail "$module links OpenSSL"
    done
    rm -rf "$work/qt-build" "$work/qtsvg-build"
    touch "$qt/.done"
}

build_molforma() {
    [ ! -e "$work/.tested-$commit" ] || [ -n "${DRAFT:-}" ] || return 0
    step "Building and testing MolForma"
    cmake -S "$src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$qt" \
        >"$log/configure.log"
    grep -q 'MolForma rendering: VTK' "$log/configure.log" || fail "VTK not found (brew install vtk)"
    grep -q 'RDKit' "$log/configure.log" || grep -q '^rdkit_DIR:.*=/' "$work/build/CMakeCache.txt" ||
        fail "RDKit not found (brew install rdkit)"
    cmake --build "$work/build" -j "$jobs" >"$log/build.log"
    # VTK's libraries name Homebrew's Qt; the build folder has no bundle to point them at this one.
    DYLD_FRAMEWORK_PATH=$qt/lib ctest --test-dir "$work/build" --output-on-failure
    touch "$work/.tested-$commit"
}

# The packages that the bundle names, then the sources of them all. What the kegs hold no license text of
# gets those of its source archive in the image's licenses too, and Qt, which is no Homebrew package
# here, its line in the list.
fetch_sources() {
    step "Fetching the sources of the bundled packages"
    local stage=$work/stage extra=$work/licenses package package_version license name files
    rm -rf "$stage" "$extra"
    cmake --install "$work/build" --prefix "$stage" >"$log/install.log"
    mkdir -p "$extra"
    sed 1d "$stage/MolForma.app/Contents/Resources/licenses/packages.txt" >"$work/packages"
    {
        cut -d' ' -f1 "$work/packages"
        printf '%s\n' qtbase qtsvg qttranslations
    } >"$work/package-names"
    while read -r package; do
        fetch_package "$package"
        if [ ! -d "$stage/MolForma.app/Contents/Resources/licenses/$package" ]; then
            name=$(archive "$package")
            files=$(tar -tf "$name" | grep -E '^[^/]+/(LICENSES/[^/]+|[^/]*(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE)[^/]*)$' || true)
            [ -n "$files" ] || fail "no license text in $name"
            mkdir -p "$extra/$package"
            echo "$files" >"$work/license-files"
            tar -xf "$name" -C "$extra/$package" --strip-components=1 -T "$work/license-files"
        fi
    done <"$work/package-names"
    {
        echo
        echo "Built for this release from the sources of the Homebrew packages, without OpenSSL (Qt's modules):"
        for package in qtbase qtsvg; do
            jq -r --arg name "$package" '.packages[] | select(.SPDXID | startswith("SPDXRef-Archive-"))
                | "\($name) \(.versionInfo) \(.licenseConcluded)"' "$(keg "$package")/sbom.spdx.json"
        done
        echo
        echo "Qt's Japanese texts (Contents/translations):"
        jq -r '.packages[] | select(.SPDXID | startswith("SPDXRef-Archive-"))
            | "qttranslations \(.versionInfo) \(.licenseConcluded)"' "$(keg qttranslations)/sbom.spdx.json"
    } >"$extra/packages.txt"
    rm -rf "$stage"
}

build_image() {
    step "Making the disk image"
    rm -f "$work/build/$image"
    (cd "$work/build" && MOLFORMA_RELEASE_LICENSES=$work/licenses cpack -G DragNDrop >"$log/cpack.log")
    [ -f "$work/build/$image" ] || fail "$image not made"
}

# The image holds nothing of OpenSSL, the Qt built here alone, and programs and libraries that link
# nothing outside the bundle and the system (a library's own name, the first line of otool, stays).
check_bundle() {
    local app=$1/MolForma.app/Contents file own links module found
    codesign --verify --deep --strict "$1/MolForma.app"
    found=$(find "$app" -iname '*libssl*' -o -iname '*libcrypto*' -o -iname '*openssl*')
    [ -z "$found" ] || fail "OpenSSL in the image: $found"
    ! grep -q '^openssl' "$app/Resources/licenses/packages.txt" || fail "OpenSSL in packages.txt"
    found=$(find "$app" -type f ! -perm -u+w)
    [ -z "$found" ] || fail "read-only, so xattr -dr fails on them: $found"
    for file in "$app"/Frameworks/Qt*.framework; do
        module=$(basename "$file" .framework)
        [ "$(uuid "$file/Versions/A/$module")" = "$(uuid "$qt/lib/$module.framework/Versions/A/$module")" ] ||
            fail "$module is not the Qt built here"
    done
    find "$app" -type f | while read -r file; do
        file -b "$file" | grep -q '^Mach-O' || continue
        own=$(otool -D "$file" | sed 1d)
        links=$(otool -L "$file" | sed 1d | awk '{ print $1 }' | grep -vxF "${own:-none}" || true)
        ! grep -E '^(/opt/homebrew|/usr/local)|/Cellar/' <<<"$links" || fail "$file links outside the bundle"
    done
}

make_files() {
    step "Making the release files"
    local mount=$work/mount here=$work/$release
    rm -rf "$here" "$work/$release.zip" "$work/$sources.tar"
    mkdir -p "$here" "$mount"
    hdiutil attach -readonly -nobrowse -mountpoint "$mount" "$work/build/$image" >/dev/null 2>&1
    trap 'hdiutil detach "$mount" >/dev/null 2>&1 || true' EXIT
    check_bundle "$mount"
    cp -R "$mount/MolForma.app/Contents/Resources/licenses" "$here/licenses"
    hdiutil detach "$mount" >/dev/null 2>&1
    trap - EXIT
    {
        echo "The source of MolForma $version and of what its disk image holds"
        echo
        echo "MolForma, with the scripts that build the disk image (packaging/macos):"
        echo "  $repository/tree/$commit"
        if git -C "$src" diff --quiet "$tag" "$commit" -- src assets; then
            echo "  Its source code (src, assets) is that of the tag $tag."
        fi
        echo
        echo "The other sources are in $sources.tar:"
        echo "  $repository/releases/tag/$tag"
        echo
        echo "There each package has a folder with the source archive and the patches of the"
        echo "version in the disk image and the Homebrew formula, which tells how it was built."
        echo "qtbase and qtsvg were built by packaging/macos/release.sh with the options of those"
        echo "formulae, without OpenSSL; the others are Homebrew's binaries for macOS 27 on arm64."
        echo
        while read -r package; do
            sed "s/^/  $package: /" "$downloads/$package/.urls"
        done <"$work/package-names"
    } >"$here/SOURCES.txt"
    cp "$here/SOURCES.txt" "$downloads/"
    cp "$work/build/$image" "$src/LICENSE" "$src/NOTICE" "$src/packaging/macos/README.txt" "$here/"
    (cd "$work" && zip -qrXy "$release.zip" "$release")
    tar -C "$work" -s ",^sources,$sources," --exclude .urls -cf "$work/$sources.tar" \
        sources/SOURCES.txt $(sed 's,^,sources/,' "$work/package-names")
    (cd "$work" && shasum -a 256 "$release.zip" "$sources.tar")
}

build_qt
build_molforma
fetch_sources
build_image
make_files
step "Done: $work/$release.zip, $work/$sources.tar"
