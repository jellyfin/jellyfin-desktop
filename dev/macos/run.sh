#!/usr/bin/env sh
# Jellyfin Desktop - Run built app
# Run build.sh first
set -e

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

APP_DIR="${BUILD_DIR}/output/${APP_NAME}"
EXECUTABLE="${APP_DIR}/Contents/MacOS/jellyfin-desktop"

# Check app bundle exists
if [ ! -d "${APP_DIR}" ]; then
    echo "error: App bundle not found. Run build.sh first" >&2
    exit 1
fi

# Run with stderr visible
exec "${EXECUTABLE}" "${@}" 2>&1
