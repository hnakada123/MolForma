# SPDX-License-Identifier: GPL-2.0-or-later
# The environment that builds the AppImage (build.sh, docs/installation/appimage.md). Ubuntu 24.04
# for an old enough glibc (2.39) with Qt 6.4; VTK is built here for Qt 6, as Ubuntu's VTK uses Qt 5.
# The downloads are pinned by version and SHA-256.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkgconf file ca-certificates curl \
        qt6-base-dev qt6-base-dev-tools qt6-translations-l10n \
        libeigen3-dev zlib1g-dev libopenbabel-dev librdkit-dev libboost-iostreams-dev libboost-serialization-dev \
        libgl-dev libegl-dev libglx-dev libx11-dev libxcursor-dev \
    && rm -rf /var/lib/apt/lists/*

# Only the VTK modules MolForma uses (CMakeLists.txt) and what they need. JOBS compiles at once
# ("--build-arg JOBS=8"); all cores at once can leave a desktop unresponsive.
ARG JOBS=4
ARG VTK_VERSION=9.7.1
ARG VTK_SHA256=cae04fd355004cb916a409db79d53a208f1221e975aeacc7540ee67b148ee91a
RUN curl -fsSL -o /tmp/vtk.tar.gz "https://vtk.org/files/release/${VTK_VERSION%.*}/VTK-${VTK_VERSION}.tar.gz" \
    && echo "${VTK_SHA256}  /tmp/vtk.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/vtk.tar.gz -C /tmp \
    && cmake -S /tmp/VTK-${VTK_VERSION} -B /tmp/vtk-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/vtk -DBUILD_SHARED_LIBS=ON -DVTK_QT_VERSION=6 \
        -DVTK_BUILD_TESTING=OFF -DVTK_BUILD_EXAMPLES=OFF -DVTK_WRAP_PYTHON=OFF -DVTK_WRAP_JAVA=OFF -DVTK_USE_MPI=OFF \
        -DVTK_GROUP_ENABLE_Imaging=DONT_WANT -DVTK_GROUP_ENABLE_MPI=DONT_WANT -DVTK_GROUP_ENABLE_Qt=DONT_WANT \
        -DVTK_GROUP_ENABLE_Rendering=DONT_WANT -DVTK_GROUP_ENABLE_StandAlone=DONT_WANT \
        -DVTK_GROUP_ENABLE_Views=DONT_WANT -DVTK_GROUP_ENABLE_Web=DONT_WANT \
        -DVTK_MODULE_ENABLE_VTK_CommonCore=YES -DVTK_MODULE_ENABLE_VTK_CommonDataModel=YES \
        -DVTK_MODULE_ENABLE_VTK_FiltersSources=YES -DVTK_MODULE_ENABLE_VTK_RenderingCore=YES \
        -DVTK_MODULE_ENABLE_VTK_RenderingOpenGL2=YES -DVTK_MODULE_ENABLE_VTK_GUISupportQt=YES \
        -DVTK_MODULE_ENABLE_VTK_InteractionStyle=YES \
    && cmake --build /tmp/vtk-build --parallel ${JOBS} \
    && cmake --install /tmp/vtk-build \
    && rm -rf /tmp/vtk.tar.gz /tmp/VTK-${VTK_VERSION} /tmp/vtk-build

# linuxdeploy gathers the libraries and Qt plugins; appimagetool packs them with the static runtime,
# which needs no libfuse2 on the computer that runs the AppImage.
ARG LINUXDEPLOY=1-alpha-20251107-1
ARG LINUXDEPLOY_SHA256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
ARG LINUXDEPLOY_QT=1-alpha-20250213-1
ARG LINUXDEPLOY_QT_SHA256=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
ARG APPIMAGETOOL=1.9.1
ARG APPIMAGETOOL_SHA256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
ARG RUNTIME=20251108
ARG RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
RUN mkdir -p /opt/appimage \
    && cd /opt/appimage \
    && curl -fsSLO "https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY}/linuxdeploy-x86_64.AppImage" \
    && curl -fsSLO "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/${LINUXDEPLOY_QT}/linuxdeploy-plugin-qt-x86_64.AppImage" \
    && curl -fsSLO "https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL}/appimagetool-x86_64.AppImage" \
    && curl -fsSLO "https://github.com/AppImage/type2-runtime/releases/download/${RUNTIME}/runtime-x86_64" \
    && printf '%s  %s\n' "${LINUXDEPLOY_SHA256}" linuxdeploy-x86_64.AppImage \
        "${LINUXDEPLOY_QT_SHA256}" linuxdeploy-plugin-qt-x86_64.AppImage \
        "${APPIMAGETOOL_SHA256}" appimagetool-x86_64.AppImage "${RUNTIME_SHA256}" runtime-x86_64 | sha256sum -c - \
    && chmod +x *.AppImage
# What the CMake files of Ubuntu's RDKit name for its C++ targets: Boost.System and Boost.Regex by
# path, and Python's include folders, without which CMake stops (MolForma uses neither Python nor NumPy).
RUN apt-get update && apt-get install -y --no-install-recommends libboost-system-dev libboost-regex-dev \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /usr/include/python3.12 /usr/lib/python3/dist-packages/numpy/core/include
# The tools are AppImages themselves; a container has no FUSE to mount them.
ENV APPIMAGE_EXTRACT_AND_RUN=1 PATH=/opt/appimage:$PATH

COPY build.sh /usr/local/bin/build-appimage
ENTRYPOINT ["/usr/local/bin/build-appimage"]
