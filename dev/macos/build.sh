#!/usr/bin/env sh
# Jellyfin Desktop - macOS build script
# Run setup.sh first to install dependencies
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

# Check dependencies
if ! command -v cmake > /dev/null; then
    echo "error: cmake not found. Run setup.sh first" >&2
    exit 1
fi

if ! command -v ninja > /dev/null; then
    echo "error: ninja not found. Run setup.sh first" >&2
    exit 1
fi

if ! command -v meson > /dev/null; then
    echo "error: meson not found. Run setup.sh first" >&2
    exit 1
fi

# Initialize submodules if needed
if [ ! -f "${PROJECT_ROOT}/third_party/mpv/meson.build" ]; then
    echo "Initializing git submodules..."
    (cd "${PROJECT_ROOT}" && git submodule update --init --recursive)
fi

# Download CEF if needed
if [ ! -d "${PROJECT_ROOT}/third_party/cef" ]; then
    echo "Downloading CEF..."
    python3 "${PROJECT_ROOT}/dev/tools/download_cef.py"
fi

# Configure
echo "Configuring..."
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DBUILD_MPV_CLI=ON \
    -DEXTERNAL_MPV_DIR= \
    "${PROJECT_ROOT}"

# Build
echo "Building..."
cmake --build "${BUILD_DIR}"

# Create app bundle (required to run on macOS)
echo "Creating app bundle..."
OUTPUT_DIR="${BUILD_DIR}/output"
cmake --install "${BUILD_DIR}" --prefix "${OUTPUT_DIR}"

APP_DIR="${OUTPUT_DIR}/${APP_NAME}"
if [ ! -d "${APP_DIR}" ]; then
    echo "error: App bundle not created" >&2
    exit 1
fi

echo ""
echo "Build complete!"
echo "Run: ${APP_DIR}"
