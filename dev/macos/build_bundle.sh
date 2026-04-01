#!/usr/bin/env sh
# Jellyfin Desktop - macOS bundle + DMG script
# Creates distributable app bundle and DMG
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

# Build first
"${SCRIPT_DIR}/build.sh"

# Determine version
if [ -f "${PROJECT_ROOT}/VERSION" ]; then
    VERSION="$(cat "${PROJECT_ROOT}/VERSION")"
else
    VERSION="$(cd "${PROJECT_ROOT}" && git describe --tags --always --dirty 2>/dev/null || echo "0.0.0")"
fi

ARCH="$(uname -m)"
APP_DIR="${BUILD_DIR}/output/${APP_NAME}"

# Create DMG
echo "Creating DMG..."
DMG_NAME="JellyfinDesktop-${VERSION}-macos-${ARCH}.dmg"
rm -f "${BUILD_DIR}/${DMG_NAME}"

# create-dmg returns non-zero if icon positioning fails (no icon), ignore that
create-dmg \
    --volname "Jellyfin Desktop v${VERSION}" \
    --no-internet-enable \
    --window-size 500 300 \
    --icon-size 100 \
    --icon "Jellyfin Desktop.app" 125 150 \
    --app-drop-link 375 150 \
    "${BUILD_DIR}/${DMG_NAME}" "${APP_DIR}" || true

# Verify DMG was created
if [ ! -f "${BUILD_DIR}/${DMG_NAME}" ]; then
    echo "error: DMG creation failed" >&2
    exit 1
fi

echo ""
echo "Bundle complete!"
echo "App: ${APP_DIR}"
echo "DMG: ${BUILD_DIR}/${DMG_NAME}"
