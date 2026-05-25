# Toolchain image for building jellyfin-desktop AppImages.
# Source build, mpv build, AppDir assembly, and packaging all happen at
# `podman run` time via container-build.sh (bind-mounted from the repo at
# /src/dev/linux/appimage/container-build.sh), with host bind mounts for
# incremental state. See container-build.sh and dev/linux/appimage/appimage.just.
FROM fedora:latest
# RPM Fusion provides the full FFmpeg with the complete decoder set, so mpv
# can decode the media and the device profile advertises it to the server.
# ffmpeg-devel below pulls it in; --allowerasing handles the swap for the
# libplacebo/libass deps.
RUN dnf -y upgrade && \
    dnf -y install \
        "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm" \
        "https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm" && \
    dnf -y install --allowerasing \
        gcc gcc-c++ make cmake git ninja-build meson python3 pkgconf-pkg-config \
        cargo rust \
        clang-devel \
        glibc-devel \
        alsa-lib-devel at-spi2-core-devel at-spi2-atk-devel atk-devel cairo-devel cups-devel \
        libXcomposite-devel libXdamage-devel libxkbcommon-devel \
        libxkbcommon-x11-devel libXrandr-devel nss-devel nspr-devel pango-devel \
        ffmpeg-devel libplacebo-devel libass-devel wayland-devel \
        libXpresent-devel libXScrnSaver-devel \
        vulkan-headers vulkan-loader-devel wayland-protocols-devel python3-docutils \
        libglvnd-devel mesa-libGL-devel mesa-libEGL-devel mesa-libgbm-devel \
        systemd-devel libdrm-devel libX11-devel xcb-util-cursor-devel \
        pipewire-devel luajit-devel rubberband-devel uchardet-devel \
        xcb-util-devel \
        plasma-wayland-protocols-devel \
        wget file patchelf desktop-file-utils && \
    dnf clean all

WORKDIR /opt/tools
RUN ARCH="$(uname -m)" && \
    wget -q "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${ARCH}.AppImage" && \
    chmod +x "appimagetool-${ARCH}.AppImage" && \
    "./appimagetool-${ARCH}.AppImage" --appimage-extract && \
    mv squashfs-root appimagetool && rm "appimagetool-${ARCH}.AppImage" && \
    wget -q -O "/opt/tools/runtime-${ARCH}" \
    "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-${ARCH}"

