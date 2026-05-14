# Toolchain image for building jellyfin-desktop AppImages.
# Source build, mpv build, AppDir assembly, and packaging all happen at
# `podman run` time via container-build.sh (bind-mounted from the repo at
# /src/dev/linux/appimage/container-build.sh), with host bind mounts for
# incremental state. See container-build.sh and dev/linux/appimage/appimage.just.
FROM archlinux:latest
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

WORKDIR /opt/tools
RUN wget -q "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" && \
    chmod +x appimagetool-x86_64.AppImage && \
    ./appimagetool-x86_64.AppImage --appimage-extract && \
    mv squashfs-root appimagetool && rm appimagetool-x86_64.AppImage && \
    wget -q -O /opt/tools/runtime-x86_64 \
    "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"

# KDE server-decoration-palette protocol (build-time only, for KWin titlebar colors)
RUN mkdir -p /usr/share/plasma-wayland-protocols && \
    wget -q -O /usr/share/plasma-wayland-protocols/server-decoration-palette.xml \
    "https://invent.kde.org/libraries/plasma-wayland-protocols/-/raw/master/src/protocols/server-decoration-palette.xml"

