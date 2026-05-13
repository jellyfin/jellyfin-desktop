#!/usr/bin/env sh
# Install build dependencies for jellyfin-desktop on Fedora/RHEL.
# Requires rpmfusion-free for ffmpeg-devel (or substitute libavcodec-free-devel).
set -eu

# Enable RPM Fusion free if not already enabled (needed for ffmpeg-devel)
if ! rpm -q rpmfusion-free-release >/dev/null 2>&1; then
    echo "RPM Fusion free not enabled. Install it first:"
    echo "  sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-\$(rpm -E %fedora).noarch.rpm"
    echo "Or substitute ffmpeg-devel with libavcodec-free-devel from the main Fedora repo."
    exit 1
fi

sudo dnf install -y \
    cmake \
    ninja-build \
    gcc \
    gcc-c++ \
    git \
    meson \
    just \
    python3 \
    python3-pip \
    \
    ffmpeg-devel \
    \
    wayland-devel \
    wayland-protocols-devel \
    plasma-wayland-protocols \
    libxkbcommon-devel \
    libxkbcommon-x11-devel \
    xcb-util-devel \
    xcb-util-cursor-devel \
    xcb-util-image-devel \
    xcb-util-keysyms-devel \
    xcb-util-wm-devel \
    libX11-devel \
    libXpresent-devel \
    libXScrnSaver-devel \
    libXv-devel \
    systemd-devel \
    libdrm-devel \
    mesa-libEGL-devel \
    mesa-libgbm-devel \
    \
    alsa-lib-devel \
    libass-devel \
    libplacebo-devel \
    rubberband-devel \
    uchardet-devel \
    zimg-devel \
    libva-devel \
    libvdpau-devel \
    libshaderc-devel \
    vulkan-headers \
    vulkan-loader-devel \
    compat-lua-devel \
    mujs-devel \
    libjpeg-turbo-devel \
    libdvdnav-devel \
    libcdio-paranoia-devel \
    libbluray-devel \
    libdisplay-info-devel \
    libunwind-devel \
    openal-soft-devel \
    pulseaudio-libs-devel \
    libsamplerate-devel \
    openssl-devel \
    libarchive-devel
