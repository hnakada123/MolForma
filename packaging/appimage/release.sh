#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Makes the files of a release from /out/MolForma-<version>-x86_64.AppImage (build.sh): a ZIP of the
# AppImage with the licenses and the list of sources, and a tar of the sources of the Ubuntu packages,
# VTK, the runtime and AppRun the AppImage holds. It runs as root in the container of the Dockerfile
# beside it, with /src and /out as for build.sh (docs/installation/appimage.md). OWNER ("uid:gid") gets
# the files made. The versions of the sources are those installed in the image, so make the AppImage in
# the same image.
set -euo pipefail
src=/src
out=/out
work=$(mktemp -d)
version=$(sed -n 's/^project(MolForma VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")
tag=v$version
name=MolForma-$version-x86_64
release=MolForma-$version-linux-x86_64
sources=MolForma-$version-linux-third-party-sources
repository=https://github.com/hnakada123/MolForma
pinned() { sed -n "s/^ARG $1=//p" "$src/packaging/appimage/Dockerfile"; }
# Alpine 3.21 when the runtime was built (RUNTIME of the Dockerfile): its tag names no versions of the
# packages it links, which came from this commit of aports, like the patches of those packages.
aports=590fd1489813473277cf110fd88b1c3888d19367

# The folder appimage of the tar: the runtime, a static program, with what it links, and what makes
# AppRun (linuxdeploy, and its Qt plugin for the hook). Trees of Git go with their submodules, which
# GitHub's archives leave out.
appimage_sources() {
    tree() { # folder, repository, tag, commit
        git -c advice.detachedHead=false clone -q --depth 1 --recurse-submodules --shallow-submodules \
            --branch "$3" "$2" "$1"
        [ "$(git -C "$1" rev-parse HEAD)" = "$4" ] || { echo "$3 of $2 is not $4" >&2; exit 1; }
        find "$1" -name .git -prune -exec rm -rf {} +
    }
    mkdir "$sources/appimage"
    cd "$sources/appimage"
    tree type2-runtime https://github.com/AppImage/type2-runtime.git "$(pinned RUNTIME)" "$(pinned RUNTIME_COMMIT)"
    tree linuxdeploy https://github.com/linuxdeploy/linuxdeploy.git "$(pinned LINUXDEPLOY)" "$(pinned LINUXDEPLOY_COMMIT)"
    tree linuxdeploy-plugin-qt https://github.com/linuxdeploy/linuxdeploy-plugin-qt.git \
        "$(pinned LINUXDEPLOY_QT)" "$(pinned LINUXDEPLOY_QT_COMMIT)"
    git init -q aports
    git -C aports remote add origin https://github.com/alpinelinux/aports.git
    git -C aports fetch -q --depth 1 --filter=blob:none origin "$aports"
    git -C aports sparse-checkout set main/musl main/zstd main/zlib community/mimalloc2
    git -C aports checkout -q FETCH_HEAD
    rm -rf aports/.git
    while read -r sha256 file url; do
        curl -fsSL -o "$file" "$url"
        echo "$sha256  $file" | sha256sum -c -
        case $file in
        fuse-* | squashfuse-*)
            grep -q "$sha256" type2-runtime/scripts/common/install-dependencies.sh ||
                { echo "The runtime no longer builds $file" >&2; exit 1; }
            ;;
        esac
    done <<'END'
70589cfd5e1cff7ccd6ac91c86c01be340b227285c5e200baa284e401eea2ca0 fuse-3.15.0.tar.xz https://github.com/libfuse/libfuse/releases/download/fuse-3.15.0/fuse-3.15.0.tar.xz
db0238c5981dabbd80ee09ae15387f390091668ca060a7bc38047912491443d3 squashfuse-0.5.2.tar.gz https://github.com/vasi/squashfuse/archive/0.5.2.tar.gz
a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4 musl-1.2.5.tar.gz https://musl.libc.org/releases/musl-1.2.5.tar.gz
30f35f71c1203369dc979ecde0400ffea93c27391bfd2ac5a9715d2173d92ff7 zstd-1.5.6.tar.gz https://github.com/facebook/zstd/archive/v1.5.6.tar.gz
9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 zlib-1.3.1.tar.gz https://zlib.net/fossils/zlib-1.3.1.tar.gz
0eed39319f139afde8515010ff59baf24de9e47ea316a315398e8027d198202d mimalloc-2.1.7.tar.gz https://github.com/microsoft/mimalloc/archive/v2.1.7/mimalloc-2.1.7.tar.gz
END
}

# What the AppImage holds: a folder of licenses per Ubuntu package (build.sh), and VTK from vtk.org.
unset APPIMAGE_EXTRACT_AND_RUN
cd "$work"
"$out/$name.AppImage" --appimage-extract >/dev/null
doc=$work/squashfs-root/usr/share/doc
ls "$doc" | grep -vx vtk | xargs dpkg-query -W -f='${source:Package} ${source:Version}\n' | sort -u >list

sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
apt-get update
apt-get install -y --no-install-recommends zip
mkdir "$sources"
# The image keeps only what it built of VTK; the archive comes again, checked as there.
vtk=$(pinned VTK_VERSION)
vtk_url=https://vtk.org/files/release/${vtk%.*}/VTK-$vtk.tar.gz
curl -fsSL -o "$sources/VTK-$vtk.tar.gz" "$vtk_url"
echo "$(pinned VTK_SHA256)  $sources/VTK-$vtk.tar.gz" | sha256sum -c -
(appimage_sources)
(
    cd "$sources"
    while read -r package package_version; do
        apt-get source --download-only "$package=$package_version"
    done <../list
)

mkdir "$release"
{
    echo "The source of MolForma $version and of what its AppImage holds"
    echo
    echo "MolForma, with the scripts that build the AppImage (packaging/appimage):"
    echo "  $repository/tree/$tag"
    echo
    echo "VTK $vtk, built from VTK-$vtk.tar.gz in $sources.tar (SHA-256 $(pinned VTK_SHA256)), of:"
    echo "  $vtk_url"
    echo
    echo "The AppImage runtime, a static program, and what made AppRun, in the folder appimage of $sources.tar:"
    echo "  type2-runtime $(pinned RUNTIME), with the patch of libfuse:"
    echo "    https://github.com/AppImage/type2-runtime/tree/$(pinned RUNTIME_COMMIT)"
    echo "  libfuse 3.15.0 and squashfuse 0.5.2, which the runtime links"
    echo "  musl 1.2.5-r9, zstd 1.5.6-r2, zlib 1.3.1-r2 and mimalloc 2.1.7-r0 of Alpine 3.21, which it links,"
    echo "  with their recipes and patches in aports:"
    echo "    https://github.com/alpinelinux/aports/tree/$aports"
    echo "  linuxdeploy $(pinned LINUXDEPLOY):"
    echo "    https://github.com/linuxdeploy/linuxdeploy/tree/$(pinned LINUXDEPLOY_COMMIT)"
    echo "  linuxdeploy-plugin-qt $(pinned LINUXDEPLOY_QT):"
    echo "    https://github.com/linuxdeploy/linuxdeploy-plugin-qt/tree/$(pinned LINUXDEPLOY_QT_COMMIT)"
    echo
    echo "The Ubuntu 24.04 packages, as source packages in $sources.tar:"
    echo "  $repository/releases/tag/$tag"
    echo
    sed 's/^/  /' list
} >"$release/SOURCES.txt"
cp "$release/SOURCES.txt" "$sources/"
cp -a "$out/$name.AppImage" "$src/LICENSE" "$src/NOTICE" "$src/packaging/appimage/README.txt" "$release/"
cp -a "$doc" "$release/licenses"

zip -qrX "$out/$release.zip" "$release"
tar -cf "$out/$sources.tar" "$sources"
[ -z "${OWNER:-}" ] || chown "$OWNER" "$out/$release.zip" "$out/$sources.tar"
