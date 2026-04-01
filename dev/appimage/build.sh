#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="${1:-$PROJECT_ROOT/build/appimage}"

mkdir -p "$OUTPUT_DIR"

echo "Building Jellyfin Desktop AppImage..."
docker build \
    -t jellyfin-desktop-appimage \
    -f "$SCRIPT_DIR/Dockerfile" \
    "$PROJECT_ROOT"

echo "Extracting AppImage..."
docker run --rm \
    -v "$OUTPUT_DIR:/host-output" \
    jellyfin-desktop-appimage

echo ""
echo "AppImage: $OUTPUT_DIR/JellyfinDesktop-x86_64.AppImage"
