FROM archlinux:latest

# Install build + runtime dependencies
# - CEF runtime deps (from jellyfin-desktop-libcef-bin PKGBUILD)
# - mpv build deps (from jellyfin-desktop-libmpv-git PKGBUILD)
# - App build deps (from jellyfin-desktop-git PKGBUILD)
# - AppImage tooling
RUN pacman -Syu --noconfirm && \
    pacman -S --noconfirm --needed \
    base-devel cmake git ninja meson python \
    alsa-lib at-spi2-core cairo libcups libxcomposite libxdamage \
    libxkbcommon libxkbcommon-x11 libxrandr nss nspr pango \
    ffmpeg libplacebo libass wayland libxpresent libxss \
    vulkan-headers wayland-protocols python-docutils \
    systemd-libs libdrm libx11 xcb-util-cursor \
    libpipewire luajit rubberband uchardet \
    wget file patchelf desktop-file-utils && \
    pacman -Scc --noconfirm

# Download and extract appimagetool (no FUSE in Docker)
WORKDIR /opt/tools
RUN wget -q "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" && \
    chmod +x appimagetool-x86_64.AppImage && \
    ./appimagetool-x86_64.AppImage --appimage-extract && \
    mv squashfs-root appimagetool && rm appimagetool-x86_64.AppImage

# Copy app source (.dockerignore symlinks to .gitignore, so the mpv submodule
# worktree is copied as-is from the host — we build exactly what's checked out).
COPY . /src
WORKDIR /src

# Download CEF (uses dev/tools/download_cef.py to fetch latest stable and set up third_party/cef symlink)
RUN python3 dev/tools/download_cef.py

# KDE server-decoration-palette protocol (build-time only, for KWin titlebar colors)
RUN mkdir -p /usr/share/plasma-wayland-protocols && \
    wget -q -O /usr/share/plasma-wayland-protocols/server-decoration-palette.xml \
    "https://invent.kde.org/libraries/plasma-wayland-protocols/-/raw/master/src/protocols/server-decoration-palette.xml"

# Build mpv from submodule (cef-mpv branch: exposes wayland-display/wayland-surface properties)
RUN cd third_party/mpv && \
    rm -rf build && \
    meson setup build --default-library=shared -Dlibmpv=true \
        -Dcdda=disabled -Ddvdnav=disabled -Djavascript=disabled \
        -Dopenal=disabled -Dvapoursynth=disabled \
        -Dwayland=enabled && \
    meson compile -C build && \
    strip build/libmpv.so

# Build jellyfin-desktop
# - CMAKE_SKIP_RPATH=1: no hardcoded paths, AppRun handles library resolution
# - Uses CEF from third_party/cef, mpv from submodule (not external dirs)
WORKDIR /build
RUN cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SKIP_RPATH=1 \
    -Wno-dev \
    /src && \
    ninja && \
    strip /build/*.so /build/jellyfin-desktop

# -- Build the AppDir (junest-style: ship the full library stack) --

RUN mkdir -p /AppDir/usr/bin /AppDir/usr/share

# Binary + CEF resources (post-build copies put everything in the build dir).
# CEF finds resources relative to /proc/self/exe, so they must be next to the binary.
RUN cp /build/jellyfin-desktop /AppDir/usr/bin/ && \
    cp /build/*.pak /AppDir/usr/bin/ && \
    cp /build/icudtl.dat /AppDir/usr/bin/ && \
    cp /build/v8_context_snapshot.bin /AppDir/usr/bin/ && \
    cp -r /build/locales /AppDir/usr/bin/ && \
    cp /build/vk_swiftshader_icd.json /AppDir/usr/bin/ 2>/dev/null || true

# CEF's own libraries (ANGLE, SwiftShader) go in usr/bin/ alongside the binary.
# This keeps them separate from system GPU libs in usr/lib/ which get removed.
# Note: libvulkan.so.1 is NOT included — the host's Vulkan loader is used
# so it picks up the correct GPU ICD (nvidia, amd, intel, etc).
RUN cp /build/libcef.so /AppDir/usr/bin/ && \
    cp /build/libEGL.so /AppDir/usr/bin/ && \
    cp /build/libGLESv2.so /AppDir/usr/bin/ && \
    cp /build/libvk_swiftshader.so /AppDir/usr/bin/

# Ship all system shared libraries. This is the key to the junest approach:
# we include glibc and ld-linux so the AppImage is independent of the host's
# glibc version.
RUN cp -a /usr/lib /AppDir/usr/lib

# mpv from build (not installed to /usr, so copy explicitly)
RUN cp /build/libmpv.so.2 /AppDir/usr/lib/

# Desktop integration
RUN mkdir -p /AppDir/usr/share/applications \
             /AppDir/usr/share/icons/hicolor/scalable/apps \
             /AppDir/usr/share/metainfo && \
    cp /src/resources/linux/org.jellyfin.JellyfinDesktop.desktop \
       /AppDir/usr/share/applications/ && \
    cp /src/resources/linux/org.jellyfin.JellyfinDesktop.svg \
       /AppDir/usr/share/icons/hicolor/scalable/apps/ && \
    cp /src/resources/linux/org.jellyfin.JellyfinDesktop.metainfo.xml \
       /AppDir/usr/share/metainfo/

# Strip out files that aren't needed at runtime to reduce AppImage size
RUN find /AppDir/usr/lib -name '*.a' -delete && \
    rm -rf /AppDir/usr/lib/pkgconfig \
           /AppDir/usr/lib/cmake \
           /AppDir/usr/lib/python* \
           /AppDir/usr/lib/perl* \
           /AppDir/usr/lib/ruby* \
           /AppDir/usr/lib/node_modules \
           /AppDir/usr/lib/gcc \
           /AppDir/usr/lib/bfd-plugins \
           /AppDir/usr/lib/ldscripts \
           /AppDir/usr/lib/systemd \
           /AppDir/usr/lib/libLLVM* \
           /AppDir/usr/lib/LLVMgold* \
           /AppDir/usr/lib/libLTO* \
           /AppDir/usr/lib/libgallium* \
           /AppDir/usr/lib/dri_gbm.so* \
           /AppDir/usr/lib/gbm/dri_gbm.so* \
           /AppDir/usr/lib/udev* \
           /AppDir/usr/lib/guile* \
           /AppDir/usr/lib/git* \
           /AppDir/usr/share/doc \
           /AppDir/usr/share/man \
           /AppDir/usr/share/info \
           /AppDir/usr/share/gtk-doc \
           /AppDir/usr/share/locale \
           /AppDir/usr/share/i18n \
           /AppDir/usr/share/help \
           /AppDir/usr/share/bash-completion \
           /AppDir/usr/share/zsh \
           /AppDir/usr/share/fish \
           /AppDir/usr/share/vala

# Remove system GPU/graphics libraries — these MUST come from the host.
# The bundled Mesa/DRI drivers won't match the host's kernel GPU driver.
# CEF's own ANGLE libs (libEGL.so, libGLESv2.so) are in usr/bin/ and
# are not affected by this cleanup.
RUN rm -rf /AppDir/usr/lib/dri \
           /AppDir/usr/lib/vdpau && \
    rm -f /AppDir/usr/lib/libEGL.so* \
          /AppDir/usr/lib/libEGL_mesa.so* \
          /AppDir/usr/lib/libGL.so* \
          /AppDir/usr/lib/libGLX.so* \
          /AppDir/usr/lib/libGLX_mesa.so* \
          /AppDir/usr/lib/libGLESv1_CM.so* \
          /AppDir/usr/lib/libGLESv2.so* \
          /AppDir/usr/lib/libGLdispatch.so* \
          /AppDir/usr/lib/libOpenGL.so* \
          /AppDir/usr/lib/libgbm.so* \
          /AppDir/usr/lib/libvulkan.so* \
          /AppDir/usr/lib/libdrm.so* \
          /AppDir/usr/lib/libdrm_*.so* \
          /AppDir/usr/lib/libxshmfence.so* \
          /AppDir/usr/lib/libglapi.so*

# Flatten shared libraries from subdirectories (e.g. /usr/lib/pulseaudio/,
# /usr/lib/pipewire-*/) into /AppDir/usr/lib so the single --library-path
# in AppRun can find them all. Uses hard copies, not symlinks.
RUN find /AppDir/usr/lib -mindepth 2 \( -name '*.so' -o -name '*.so.*' \) | while read lib; do \
        base="$(basename "$lib")" ; \
        [ ! -e "/AppDir/usr/lib/$base" ] && cp -L "$lib" "/AppDir/usr/lib/$base" ; \
    done

# Fix absolute-path DT_NEEDED entries (e.g. /usr/lib/libmujs.so).
# Some Arch packages embed full paths instead of bare sonames. The dynamic
# linker follows these literally — even with --library-path — so they must
# be rewritten to bare filenames.
RUN find /AppDir/usr/lib /AppDir/usr/bin -type f \( -name '*.so*' -o -executable \) 2>/dev/null | while read f; do \
        patchelf --print-needed "$f" 2>/dev/null | grep '^/' | while read lib; do \
            base="$(basename "$lib")" ; \
            patchelf --replace-needed "$lib" "$base" "$f" 2>/dev/null || true ; \
        done ; \
    done

# Patch the binary's ELF interpreter to use our bundled ld-linux via a runtime
# symlink. AppRun creates /tmp/.jf-cef-interp/ld-linux-x86-64.so.2 -> bundled.
# Unlike "exec ld-linux ... binary", this preserves /proc/self/exe pointing to
# the actual binary — critical for CEF, which re-executes /proc/self/exe for
# renderer/GPU/utility subprocesses (--type=renderer, --type=gpu-process, etc).
RUN patchelf --set-interpreter /tmp/.jf-cef-interp/ld-linux-x86-64.so.2 \
    /AppDir/usr/bin/jellyfin-desktop

# Desktop integration files at AppDir root (required by AppImage spec)
RUN cp /AppDir/usr/share/applications/org.jellyfin.JellyfinDesktop.desktop /AppDir/ && \
    cp /AppDir/usr/share/icons/hicolor/scalable/apps/org.jellyfin.JellyfinDesktop.svg /AppDir/ && \
    ln -sf org.jellyfin.JellyfinDesktop.svg /AppDir/.DirIcon

COPY dev/appimage/AppRun /AppDir/AppRun
RUN chmod +x /AppDir/AppRun

# Download the type2 runtime separately (appimagetool's built-in download is
# flaky and frequently returns 502). Pin to a known-good release.
RUN wget -q -O /opt/tools/runtime-x86_64 \
    "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"

# Build the AppImage
RUN mkdir -p /output && \
    ARCH=x86_64 /opt/tools/appimagetool/AppRun --no-appstream \
    --runtime-file /opt/tools/runtime-x86_64 \
    /AppDir /output/JellyfinDesktop-x86_64.AppImage

CMD ["sh", "-c", "cp /output/JellyfinDesktop-x86_64.AppImage /host-output/JellyfinDesktop-${VERSION}-linux-x86_64.AppImage"]
