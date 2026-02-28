#!/usr/bin/env sh
# Jellyfin Desktop - Run unit tests
# Run build.sh first
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"
setup_runtime

cd "${BUILD_DIR}"
ctest --output-on-failure "$@"
