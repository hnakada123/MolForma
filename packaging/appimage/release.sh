#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Makes the files of a release from /out/MolForma-<version>-x86_64.AppImage (build.sh): a ZIP of the
# AppImage with the licenses and the list of sources, and a tar of the sources of the Ubuntu packages
# the AppImage holds. It runs as root in the container of the Dockerfile beside it, with /src and /out
# as for build.sh (docs/installation/appimage.md). OWNER ("uid:gid") gets the files made. The versions
# of the sources are those installed in the image, so make the AppImage in the same image.
set -euo pipefail
src=/src
out=/out
work=$(mktemp -d)
version=$(sed -n 's/^project(MolForma VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")
tag=v$version
name=MolForma-$version-x86_64
sources=MolForma-$version-third-party-sources
repository=https://github.com/hnakada123/MolForma
pinned() { sed -n "s/^ARG $1=//p" "$src/packaging/appimage/Dockerfile"; }

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
(
    cd "$sources"
    while read -r package package_version; do
        apt-get source --download-only "$package=$package_version"
    done <../list
)

mkdir "$name"
{
    echo "The source of MolForma $version and of what its AppImage holds"
    echo
    echo "MolForma, with the scripts that build the AppImage (packaging/appimage):"
    echo "  $repository/tree/$tag"
    echo
    echo "VTK $(pinned VTK_VERSION), built from (SHA-256 $(pinned VTK_SHA256)):"
    vtk=$(pinned VTK_VERSION)
    echo "  https://vtk.org/files/release/${vtk%.*}/VTK-$vtk.tar.gz"
    echo
    echo "The AppImage runtime and its AppRun:"
    echo "  https://github.com/AppImage/type2-runtime/releases/tag/$(pinned RUNTIME)"
    echo "  https://github.com/linuxdeploy/linuxdeploy/releases/tag/$(pinned LINUXDEPLOY)"
    echo
    echo "The Ubuntu 24.04 packages, as source packages in $sources.tar:"
    echo "  $repository/releases/tag/$tag"
    echo
    sed 's/^/  /' list
} >"$name/SOURCES.txt"
cp "$name/SOURCES.txt" "$sources/"
cp -a "$out/$name.AppImage" "$src/LICENSE" "$src/NOTICE" "$src/packaging/appimage/README.txt" "$name/"
cp -a "$doc" "$name/licenses"

zip -qrX "$out/$name.zip" "$name"
tar -cf "$out/$sources.tar" "$sources"
[ -z "${OWNER:-}" ] || chown "$OWNER" "$out/$name.zip" "$out/$sources.tar"
