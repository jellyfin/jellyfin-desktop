#!/usr/bin/env sh
# Jellyfin Desktop - Run built app
# Run build.sh first
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"
setup_runtime

APP_PATH="${BUILD_DIR}/src/${APP_NAME}"

if [ ! -d "${APP_PATH}" ]; then
    echo "error: App bundle not found at ${APP_PATH}" >&2
    exit 1
fi

exec "${APP_PATH}/Contents/MacOS/Jellyfin Desktop" ${1+"$@"}
