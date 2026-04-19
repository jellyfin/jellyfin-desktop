#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUTPUT_DIR="${1:-${PROJECT_ROOT}/dist}"

if command -v podman > /dev/null 2>&1; then
    CONTAINER_CMD=podman
elif command -v docker > /dev/null 2>&1; then
    CONTAINER_CMD=docker
else
    echo 'error: podman or docker required' >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

echo "Building Jellyfin Desktop AppImage (${CONTAINER_CMD})..."
${CONTAINER_CMD} build \
    -t jellyfin-desktop-appimage \
    -f "${SCRIPT_DIR}/Dockerfile" \
    "${PROJECT_ROOT}"

echo "Extracting AppImage..."
${CONTAINER_CMD} run --rm \
    -v "${OUTPUT_DIR}:/host-output" \
    jellyfin-desktop-appimage

echo ""
echo "AppImage: ${OUTPUT_DIR}/JellyfinDesktop-x86_64.AppImage"
