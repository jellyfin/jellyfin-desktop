#!/usr/bin/env sh
# Jellyfin Desktop - macOS dependency installer
# Run once to install all build dependencies
set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

echo "[1/5] Checking Xcode Command Line Tools..."
if ! xcode-select -p > /dev/null 2>&1; then
    echo "Installing Xcode Command Line Tools..."
    xcode-select --install
    echo "Please re-run this script after installation completes"
    exit 0
fi

echo "[2/5] Checking Homebrew..."
if ! command -v brew > /dev/null; then
    echo "error: Homebrew not found. Install from https://brew.sh" >&2
    exit 1
fi

echo "[3/5] Installing build tools..."
brew install aqtinstall mpv ninja cmake create-dmg

echo "[4/5] Installing Qt ${QT_VERSION}..."
if [ ! -d "${DEPS_DIR}/qt/${QT_VERSION}/macos" ]; then
    mkdir -p "${DEPS_DIR}/qt"
    (cd "${DEPS_DIR}" && aqt install-qt mac desktop "${QT_VERSION}" -m qtwebengine qtwebchannel qtpositioning -O "qt")
else
    echo "Qt already installed, skipping"
fi

echo "[5/5] Generating CMakePresets.json..."
BREW_PREFIX="$(brew --prefix)"
cat > "${PROJECT_ROOT}/CMakePresets.json" << PRESETS_EOF
{
  "version": 6,
  "configurePresets": [
    {
      "name": "macos-dev",
      "displayName": "macOS Development",
      "description": "Debug build using deps from dev/macos/setup.sh",
      "generator": "Ninja",
      "binaryDir": "\${sourceDir}/build",
      "condition": {
        "type": "equals",
        "lhs": "\${hostSystemName}",
        "rhs": "Darwin"
      },
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_PREFIX_PATH": "${BREW_PREFIX}",
        "QTROOT": "\${sourceDir}/dev/macos/deps/qt/${QT_VERSION}/macos",
        "USE_STATIC_MPVQT": "ON",
        "MACOS_LAUNCH_WRAPPER": "ON"
      }
    },
    {
      "name": "windows-dev",
      "displayName": "Windows Development",
      "description": "Debug build using deps from dev/windows/setup.bat",
      "generator": "Ninja",
      "binaryDir": "\${sourceDir}/build",
      "condition": {
        "type": "equals",
        "lhs": "\${hostSystemName}",
        "rhs": "Windows"
      },
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "QTROOT": "\${sourceDir}/dev/windows/deps/qt/${QT_VERSION}/msvc2022_64",
        "MPV_INCLUDE_DIR": "\${sourceDir}/dev/windows/deps/mpv/include",
        "MPV_LIBRARY": "\${sourceDir}/dev/windows/deps/mpv/libmpv-2.dll.lib",
        "USE_STATIC_MPVQT": "ON"
      }
    },
    {
      "name": "linux-dev",
      "displayName": "Linux Development",
      "description": "Debug build using system Qt and libraries",
      "generator": "Ninja",
      "binaryDir": "\${sourceDir}/build",
      "condition": {
        "type": "equals",
        "lhs": "\${hostSystemName}",
        "rhs": "Linux"
      },
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "USE_STATIC_MPVQT": "ON"
      }
    }
  ]
}
PRESETS_EOF

echo ""
echo "Setup complete. Run build.sh to build."
