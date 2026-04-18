#!/usr/bin/env sh
# Jellyfin Desktop - Run unit tests
# Run build.sh first
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
    echo "error: Build directory not found. Run build.sh first" >&2
    exit 1
fi

exec ctest --test-dir "${BUILD_DIR}" --output-on-failure ${1+"$@"}
