#!/usr/bin/env sh
# Jellyfin Desktop - Common variables
# Sourced by other scripts

QT_VERSION=6.10.1

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DEPS_DIR="${SCRIPT_DIR}/deps"
BUILD_DIR="${PROJECT_ROOT}/build"
APP_NAME="Jellyfin Desktop.app"

# Setup runtime environment for Qt libs from aqt installation (unbundled dev build).
# Call from run.sh / test.sh after sourcing this file.
setup_runtime() {
    QTROOT="${DEPS_DIR}/qt/${QT_VERSION}/macos"

    if [ ! -d "${BUILD_DIR}" ]; then
        echo "error: Build not found. Run build.sh first" >&2
        exit 1
    fi

    export DYLD_FRAMEWORK_PATH="${QTROOT}/lib"
    export QT_PLUGIN_PATH="${QTROOT}/plugins"
    export QML_IMPORT_PATH="${QTROOT}/qml"
}
