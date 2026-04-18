#!/usr/bin/env sh
# Jellyfin Desktop - macOS dependency installer
# Run once to install all build dependencies
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

echo "Checking Xcode Command Line Tools..."
if ! xcode-select -p > /dev/null 2>&1; then
    echo "Installing Xcode Command Line Tools..."
    xcode-select --install
    echo "Please re-run this script after installation completes"
    exit 0
fi

echo "Checking Homebrew..."
if ! command -v brew > /dev/null; then
    echo "error: Homebrew not found. Install from https://brew.sh" >&2
    exit 1
fi

PACKAGES="
cmake
ninja
meson
pkgconf
ffmpeg
libplacebo
libass
luajit
vulkan-loader
vulkan-headers
molten-vk
little-cms2
libunibreak
zimg
create-dmg
"

echo "Checking installed packages..."
INSTALLED="$(brew list --formula -1)"
MISSING=""
for pkg in ${PACKAGES}; do
    if ! printf '%s\n' "${INSTALLED}" | grep -qx "${pkg}"; then
        MISSING="${MISSING} ${pkg}"
    fi
done

if [ -n "${MISSING}" ]; then
    echo "Installing missing packages:${MISSING}"
    # shellcheck disable=SC2086
    brew install ${MISSING}
fi
