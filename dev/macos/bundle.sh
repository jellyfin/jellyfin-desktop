#!/usr/bin/env sh
# Jellyfin Desktop - macOS bundling script
# Run build.sh first
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

# Check build exists
if [ ! -d "${BUILD_DIR}/src/${APP_NAME}" ]; then
    echo "error: Build not found. Run build.sh first" >&2
    exit 1
fi

cd "${BUILD_DIR}"

# Install (runs macdeployqt to bundle Qt frameworks)
echo "Bundling Qt frameworks..."
ninja install

APP_BUNDLE="${BUILD_DIR}/output/${APP_NAME}"

echo "Signing app bundle..."
"${SCRIPT_DIR}/sign.sh" "${APP_BUNDLE}"

echo "Creating DMG..."
VERSION="$(cat "${PROJECT_ROOT}/VERSION")"
ARCH="$(uname -m)"
DMG_NAME="JellyfinDesktop-${VERSION}-${ARCH}.dmg"
rm -f "${DMG_NAME}"
create-dmg \
    --volname "Jellyfin Desktop v${VERSION}" \
    --no-internet-enable \
    --window-size 500 300 \
    --icon-size 100 \
    --icon "Jellyfin Desktop.app" 125 150 \
    --app-drop-link 375 150 \
    "${DMG_NAME}" "${APP_BUNDLE}"

DMG_PATH="${BUILD_DIR}/${DMG_NAME}"
if [ -n "${MACOS_SIGNING_IDENTITY:-}" ]; then
    TIMESTAMP="--timestamp"
    if [ "${MACOS_CODESIGN_TIMESTAMP:-}" = "none" ]; then
        TIMESTAMP="--timestamp=none"
    fi
    codesign --force "${TIMESTAMP}" --sign "${MACOS_SIGNING_IDENTITY}" "${DMG_PATH}"
fi

"${SCRIPT_DIR}/notarize.sh" "${DMG_PATH}"

echo ""
echo "Bundle complete!"
echo "DMG: ${DMG_PATH}"
