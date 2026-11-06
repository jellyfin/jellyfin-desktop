#!/usr/bin/env bash
# Jellyfin Desktop - Linux build & install script
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
PREFIX="/usr/local"

confirm() {
	local reply
	read -r -p "${1} [y/N] " reply
	[[ "${reply}" =~ ^[Yy]([Ee][Ss])?$ ]]
}

if ! pkg-config --exists Qt6Core 2>/dev/null; then
	echo "error: Qt6 dev packages not found. Run setup.sh first" >&2
	exit 1
fi

# Submodules (e.g. external/mpvqt) are required for -DUSE_STATIC_MPVQT=ON.
if [[ -z "$(ls -A "${PROJECT_ROOT}/external/mpvqt" 2>/dev/null)" ]]; then
	echo "Initializing submodules..."
	git -C "${PROJECT_ROOT}" submodule update --init --recursive
fi

if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
	cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt")"
	if [[ "${cached_source}" != "${PROJECT_ROOT}" ]]; then
		echo "Build directory was configured from ${cached_source}, removing stale build/"
		rm -rf "${BUILD_DIR}"
	fi
fi

echo "Configuring..."
cmake -B "${BUILD_DIR}" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DUSE_STATIC_MPVQT=ON \
	"${PROJECT_ROOT}"

echo "Building..."
cmake --build "${BUILD_DIR}"

BINARY="${BUILD_DIR}/src/jellyfin-desktop"
if [[ ! -x "${BINARY}" ]]; then
	echo "error: build did not produce ${BINARY}" >&2
	exit 1
fi

echo ""
echo "Build complete: ${BINARY}"
echo ""

BIN_DEST="${PREFIX}/bin/jellyfin-desktop"
DESKTOP_SRC="${PROJECT_ROOT}/resources/meta/org.jellyfin.JellyfinDesktop.desktop"
APPDATA_SRC="${PROJECT_ROOT}/resources/meta/org.jellyfin.JellyfinDesktop.appdata.xml"
ICON_SRC="${PROJECT_ROOT}/resources/images/icon.svg"
DESKTOP_DEST="${PREFIX}/share/applications/org.jellyfin.JellyfinDesktop.desktop"
APPDATA_DEST="${PREFIX}/share/metainfo/org.jellyfin.JellyfinDesktop.appdata.xml"
ICON_DEST="${PREFIX}/share/icons/hicolor/scalable/apps/org.jellyfin.JellyfinDesktop.svg"

if confirm "Install binary to ${BIN_DEST} (requires sudo)?"; then
	sudo install -Dm755 "${BINARY}" "${BIN_DEST}"
	echo "Installed ${BIN_DEST}"
else
	echo "Skipped binary install."
fi

if confirm "Copy .desktop file, icon, and appdata metadata for app menu integration (requires sudo)?"; then
	sudo install -Dm644 "${DESKTOP_SRC}" "${DESKTOP_DEST}"
	sudo install -Dm644 "${APPDATA_SRC}" "${APPDATA_DEST}"
	sudo install -Dm644 "${ICON_SRC}" "${ICON_DEST}"
	echo "Installed desktop entry, icon, and appdata metadata."
else
	echo "Skipped desktop integration files."
fi

echo ""
echo "Done."
