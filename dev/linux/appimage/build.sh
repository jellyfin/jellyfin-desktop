#!/bin/sh
# Build .AppImage in dist/. Runs the container-based build via :base image.
# Source / cmake / ninja / meson state persists in build/appimage/ on the host.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
IMG="jellyfin-desktop-appimage:base"
VERSION="$("${PROJECT_ROOT}/dev/tools/version.sh")"

if command -v podman >/dev/null 2>&1; then
    cmd=podman
elif command -v docker >/dev/null 2>&1; then
    cmd=docker
else
    echo 'error: podman or docker required' >&2
    exit 1
fi

if [ ! -d "${PROJECT_ROOT}/build/appimage" ]; then
    # Fresh build dir → clean image rebuild (pull latest base, no layer cache)
    "$cmd" build --pull=always --no-cache \
        -t "$IMG" -f "${SCRIPT_DIR}/Dockerfile" "$PROJECT_ROOT"
fi

mkdir -p "${PROJECT_ROOT}/build/appimage/build" "${PROJECT_ROOT}/dist"

"$cmd" run --rm \
    -v "${PROJECT_ROOT}:/src" \
    -v "${PROJECT_ROOT}/build/appimage/build:/build" \
    -v "${PROJECT_ROOT}/dist:/host-output" \
    -e VERSION="$VERSION" \
    "$IMG" /src/dev/linux/appimage/container-build.sh

ARCH="$(uname -m)"
BUNDLE="dist/JellyfinDesktop-${VERSION}-${ARCH}.AppImage"
LINK="${PROJECT_ROOT}/build/appimage/JellyfinDesktop.AppImage"
ln -sf "../../${BUNDLE}" "$LINK"
echo "AppImage: ${BUNDLE} (-> ${LINK})"
