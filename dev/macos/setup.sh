#!/usr/bin/env sh
# Jellyfin Desktop - macOS dependency installer
# Run once to install all build dependencies
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

echo "[1/4] Checking Xcode Command Line Tools..."
if ! xcode-select -p > /dev/null 2>&1; then
    echo "Installing Xcode Command Line Tools..."
    xcode-select --install
    echo "Please re-run this script after installation completes"
    exit 0
fi

echo "[2/4] Checking Homebrew..."
if ! command -v brew > /dev/null; then
    echo "error: Homebrew not found. Install from https://brew.sh" >&2
    exit 1
fi

echo "[3/4] Installing build tools..."
brew install cmake ninja meson pkg-config

echo "[4/4] Installing mpv and runtime dependencies..."
brew install \
    ffmpeg \
    libplacebo \
    libass \
    luajit \
    vulkan-loader \
    vulkan-headers \
    molten-vk \
    sdl3 \
    lcms2 \
    libunibreak \
    zimg \
    create-dmg

echo ""
echo "Setup complete. Run build.sh to build."
