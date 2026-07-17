#!/usr/bin/env sh
# Jellium Desktop - create distributable DMG from a built app bundle.
# Assumes `cargo xtask install --prefix build/output` has already run.
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
APP_NAME="Jellium Desktop.app"
APP_DIR="${PROJECT_ROOT}/build/output/${APP_NAME}"
DIST_DIR="${PROJECT_ROOT}/dist"

if [ ! -d "${APP_DIR}" ]; then
    echo "error: ${APP_DIR} not found. Run 'just build' first" >&2
    exit 1
fi

VERSION="$(cargo run --quiet --manifest-path "${PROJECT_ROOT}/src/xtask/Cargo.toml" -- version)"
ARCH="$(uname -m)"
mkdir -p "${DIST_DIR}"

DMG_NAME="JelliumDesktop-${VERSION}-macos-${ARCH}.dmg"
rm -f "${DIST_DIR}/${DMG_NAME}"

create-dmg \
    --volname "Jellium Desktop v${VERSION}" \
    --no-internet-enable \
    --window-size 500 300 \
    --icon-size 100 \
    --icon "${APP_NAME}" 125 150 \
    --app-drop-link 375 150 \
    "${DIST_DIR}/${DMG_NAME}" "${APP_DIR}" || true

if [ ! -f "${DIST_DIR}/${DMG_NAME}" ]; then
    echo "error: DMG creation failed" >&2
    exit 1
fi

echo "DMG: ${DIST_DIR}/${DMG_NAME}"
