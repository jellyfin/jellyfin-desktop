#!/bin/sh
# Run the most recently built .AppImage via the stable symlink in build/appimage/.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
exec "${PROJECT_ROOT}/build/appimage/JellyfinDesktop.AppImage" "$@"
