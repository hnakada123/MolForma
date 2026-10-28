#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds MolForma from the source in /src and writes /out/MolForma-<version>-x86_64.AppImage. It runs in
# the container of the Dockerfile beside it (docs/installation/appimage.md). JOBS compiles at once
# ("-e JOBS=8" with docker run; 4 by default).
set -euo pipefail
src=/src
out=/out
work=$(mktemp -d)
appdir=$work/AppDir
version=$(sed -n 's/^project(MolForma VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")

cmake -S "$src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_PREFIX_PATH=/opt/vtk -DBUILD_TESTING=OFF
# Stop rather than make an AppImage that silently left out VTK or RDKit.
grep -q -- '-DMOLFORMA_HAS_VTK=1' "$work/build/build.ninja" || { echo 'CMake cannot use VTK' >&2; exit 1; }
grep -q -- '-DMOLFORMA_HAS_RDKIT=1' "$work/build/build.ninja" || { echo 'CMake cannot use RDKit' >&2; exit 1; }
cmake --build "$work/build" --parallel "${JOBS:-4}"
DESTDIR="$appdir" cmake --install "$work/build"

# Open Babel finds its format plugins and force-field data where BABEL_LIBDIR and BABEL_DATADIR say;
# AppRun sets them to the copies in the AppImage. Of Qt's platforms it holds xcb (and offscreen), so
# a Wayland session uses XWayland, also where QT_QPA_PLATFORM asks for wayland alone.
babel=$(pkg-config --modversion openbabel-3)
mkdir -p "$appdir/usr/lib/openbabel" "$appdir/usr/share/openbabel" "$appdir/apprun-hooks"
cp -a "$(pkg-config --variable=libdir openbabel-3)/openbabel/$babel" "$appdir/usr/lib/openbabel/"
cp -a "/usr/share/openbabel/$babel" "$appdir/usr/share/openbabel/"
cat >"$appdir/apprun-hooks/molforma.sh" <<EOF
export BABEL_LIBDIR="\$APPDIR/usr/lib/openbabel/$babel"
export BABEL_DATADIR="\$APPDIR/usr/share/openbabel/$babel"
case "\${QT_QPA_PLATFORM:-}" in
"" | wayland*) export QT_QPA_PLATFORM=xcb ;;
esac
EOF

# The libraries MolForma, the Open Babel plugins and the Qt plugins use, except those every desktop
# has (linuxdeploy's exclude list: glibc, OpenGL, X11, fontconfig, ...). The offscreen platform
# serves --screenshot.
export LD_LIBRARY_PATH=/opt/vtk/lib QMAKE=/usr/lib/qt6/bin/qmake EXTRA_PLATFORM_PLUGINS=libqoffscreen.so
linuxdeploy-x86_64.AppImage --appdir "$appdir" --executable "$appdir/usr/bin/molforma" \
    --desktop-file "$appdir/usr/share/applications/molforma.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/molforma.png" \
    --deploy-deps-only "$appdir/usr/lib/openbabel/$babel" --plugin qt

# The licenses of what it holds: VTK's, and the copyright file of each Ubuntu package a bundled
# library comes from.
mkdir -p "$appdir/usr/share/doc"
cp -a /opt/vtk/share/licenses/VTK "$appdir/usr/share/doc/vtk"
find "$appdir/usr" -name '*.so*' -type f -printf '/%f\n' | sort -u | while read -r name; do
    dpkg -S "$name" 2>/dev/null | sed 's/: .*//; s/, /\n/g; s/:[a-z0-9]*$//mg' || true
done | sort -u | while read -r package; do
    if [ -f "/usr/share/doc/$package/copyright" ]; then
        mkdir -p "$appdir/usr/share/doc/$package"
        cp "/usr/share/doc/$package/copyright" "$appdir/usr/share/doc/$package/"
    fi
done

mkdir -p "$out"
appimagetool-x86_64.AppImage --runtime-file /opt/appimage/runtime-x86_64 "$appdir" \
    "$out/MolForma-$version-x86_64.AppImage"
