#!/bin/sh
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_OUT="${REPO_ROOT}/build/flatpak"
DIST_DIR="${REPO_ROOT}/dist"
mkdir -p "$BUILD_OUT" "$DIST_DIR"
cd "$BUILD_OUT"

MANIFEST="${SCRIPT_DIR}/org.jellyfin.JellyfinDesktop.yml"
APP_ID="org.jellyfin.JellyfinDesktop"
VERSION="$("${REPO_ROOT}/dev/tools/version.sh")"
DATE="$(date -u +%Y-%m-%d)"
ARCH="$(uname -m)"
BUNDLE_NAME="JellyfinDesktop-${VERSION}-linux-${ARCH}.flatpak"
RUNTIME_VERSION="25.08"

# Check dependencies
command -v flatpak >/dev/null || { echo "Error: flatpak not found"; exit 1; }
command -v flatpak-builder >/dev/null || { echo "Error: flatpak-builder not found"; exit 1; }

# Install SDK and runtime if needed
if ! flatpak info --user org.freedesktop.Sdk//$RUNTIME_VERSION >/dev/null 2>&1 && \
   ! flatpak info --system org.freedesktop.Sdk//$RUNTIME_VERSION >/dev/null 2>&1; then
    echo "Installing Freedesktop SDK $RUNTIME_VERSION..."
    flatpak install --user -y flathub org.freedesktop.Sdk//$RUNTIME_VERSION org.freedesktop.Platform//$RUNTIME_VERSION
fi

# Ensure CEF is extracted at third_party/cef
if [ ! -d "${REPO_ROOT}/third_party/cef" ]; then
    python3 "${REPO_ROOT}/dev/tools/download_cef.py"
fi

# Generate metainfo.xml with the current version injected.
python3 "${SCRIPT_DIR}/generate_metainfo.py" \
    --template "${REPO_ROOT}/resources/linux/org.jellyfin.JellyfinDesktop.metainfo.xml" \
    --output "${BUILD_OUT}/generated.metainfo.xml" \
    --version "$VERSION" \
    --date "$DATE"

# Build
echo "Building flatpak..."
flatpak-builder --user --repo=repo --force-clean build-dir "$MANIFEST"

# Create bundle
echo "Creating bundle..."
flatpak build-bundle repo "${DIST_DIR}/${BUNDLE_NAME}" "$APP_ID"

echo "Done: ${DIST_DIR}/${BUNDLE_NAME}"
