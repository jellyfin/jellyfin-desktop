#!/usr/bin/env sh
# Jellyfin Desktop - Common variables
# Sourced by other scripts

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
APP_NAME="Jellyfin Desktop.app"
