FROM archlinux:latest

# Install build + runtime dependencies (aligned with the AUR PKGBUILD)
RUN pacman -Syu --noconfirm && \
    pacman -S --noconfirm --needed \
    base-devel cmake git ninja \
    qt6-webengine qt6-declarative \
    mpv mpvqt libcec sdl2 p8-platform protobuf qt6-serialport \
    wget file patchelf desktop-file-utils && \
    pacman -Scc --noconfirm

# Download and extract appimagetool (no FUSE in Docker)
WORKDIR /opt/tools
RUN wget -q "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" && \
    chmod +x appimagetool-x86_64.AppImage && \
    ./appimagetool-x86_64.AppImage --appimage-extract && \
    mv squashfs-root appimagetool && rm appimagetool-x86_64.AppImage

# Build and install
COPY . /src
WORKDIR /build
RUN cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_SKIP_RPATH=1 \
    -Wno-dev \
    /src && \
    ninja && \
    ninja install

# -- Build the AppDir (junest-style: ship the full library stack) --

RUN mkdir -p /AppDir/usr/bin /AppDir/usr/share

# Binary
RUN cp /usr/bin/jellyfin-desktop /AppDir/usr/bin/

# Ship all shared libraries. This is the key to the junest approach: we include
# glibc and ld-linux so the AppImage is independent of the host's glibc version.
RUN cp -a /usr/lib /AppDir/usr/lib

# App data, Qt resources, desktop integration
RUN for d in jellyfin-desktop qt6; do \
        [ -d "/usr/share/$d" ] && cp -a "/usr/share/$d" "/AppDir/usr/share/$d"; \
    done && \
    mkdir -p /AppDir/usr/share/applications /AppDir/usr/share/icons /AppDir/usr/share/metainfo && \
    cp /usr/share/applications/org.jellyfin.JellyfinDesktop.desktop /AppDir/usr/share/applications/ && \
    cp -a /usr/share/icons/hicolor /AppDir/usr/share/icons/ && \
    cp /usr/share/metainfo/org.jellyfin.JellyfinDesktop.appdata.xml /AppDir/usr/share/metainfo/ 2>/dev/null || true

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

# Remove GPU/graphics libraries — these MUST come from the host system.
# The bundled Mesa/DRI drivers won't match the host's kernel GPU driver,
# which breaks EGL initialization and prevents mpv from rendering video.
# With these removed, --library-path falls through to host paths in AppRun.
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

# Desktop integration files at AppDir root (required by AppImage spec)
RUN cp /AppDir/usr/share/applications/org.jellyfin.JellyfinDesktop.desktop /AppDir/ && \
    cp /AppDir/usr/share/icons/hicolor/scalable/apps/org.jellyfin.JellyfinDesktop.svg /AppDir/ && \
    ln -sf org.jellyfin.JellyfinDesktop.svg /AppDir/.DirIcon

# Build Wayland positioner fix (LD_PRELOAD library).
# Prevents xdg_positioner.set_size(0,0) during popup reposition, which is
# an xdg-shell protocol violation that crashes on Mutter/GNOME.
COPY dev/appimage/positioner-fix.c /tmp/positioner-fix.c
RUN gcc -shared -fPIC -O2 -o /AppDir/usr/lib/libpositioner-fix.so \
    /tmp/positioner-fix.c -ldl

# Wrap QtWebEngineProcess so it also uses the bundled ld-linux.
# This MUST be a statically-linked binary, not a shell script.  The parent
# exports LD_LIBRARY_PATH pointing to the bundled (Arch) libs; a script
# wrapper would be interpreted by /bin/bash loaded via the host's ld-linux,
# which then pulls in the bundled glibc — an ABI mismatch that segfaults
# on distros whose glibc differs from the build host.
COPY dev/appimage/webengine-wrapper.c /tmp/webengine-wrapper.c
RUN gcc -static -O2 -o /tmp/webengine-wrapper /tmp/webengine-wrapper.c && \
    strip /tmp/webengine-wrapper
RUN set -e && \
    webengine_bin="$(find /AppDir/usr/lib -name QtWebEngineProcess -type f)" && \
    if [ -n "$webengine_bin" ]; then \
        mv "$webengine_bin" "${webengine_bin}.real" && \
        cp /tmp/webengine-wrapper "$webengine_bin" && \
        chmod +x "$webengine_bin"; \
    fi

# Create AppRun — uses the bundled ld-linux to bypass the host glibc entirely
COPY dev/appimage/AppRun /AppDir/AppRun
RUN chmod +x /AppDir/AppRun

# Build the AppImage
RUN mkdir -p /output && \
    ARCH=x86_64 /opt/tools/appimagetool/AppRun --no-appstream \
    /AppDir /output/JellyfinDesktop-x86_64.AppImage

CMD ["cp", "/output/JellyfinDesktop-x86_64.AppImage", "/host-output/"]
