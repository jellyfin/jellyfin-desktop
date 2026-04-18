#!/bin/sh
set -eu

cd "$(dirname "$0")"

MANIFEST="org.jellyfin.JellyfinDesktop.yml"
APP_ID="org.jellyfin.JellyfinDesktop"
BUNDLE_NAME="jellyfin-desktop.flatpak"
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

# Ensure manifest CEF version matches CEF_VERSION
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CEF_VERSION="$(cat "${REPO_ROOT}/CEF_VERSION")"
if ! grep -q "cef_binary_${CEF_VERSION}" "$MANIFEST"; then
    echo "Manifest CEF version doesn't match CEF_VERSION (${CEF_VERSION}), updating..."
    python3 "${REPO_ROOT}/dev/tools/update_flatpak_manifest.py"
fi

# Build
echo "Building flatpak..."
flatpak-builder --user --repo=repo --force-clean build-dir "$MANIFEST"

# Create bundle
echo "Creating bundle..."
flatpak build-bundle repo "$BUNDLE_NAME" "$APP_ID"

echo "Done: $BUNDLE_NAME"
