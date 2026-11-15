#!/usr/bin/env sh
# Jellyfin Desktop - macOS signing script
# Signs the app bundle with hardened runtime and entitlements.
#
# Uses MACOS_SIGNING_IDENTITY when set, otherwise preserves the existing
# ad-hoc signing behavior used by local and credential-free CI builds.
# Set MACOS_CODESIGN_TIMESTAMP=none when using a local self-signed identity.
#
# Usage: dev/macos/sign.sh [path-to-app]

set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

APP_PATH="${1:-${BUILD_DIR}/src/${APP_NAME}}"
ENTITLEMENTS="${SCRIPT_DIR}/jellyfin-desktop.entitlements"

if [ ! -d "${APP_PATH}" ]; then
    echo "error: App bundle not found at ${APP_PATH}" >&2
    exit 1
fi

IDENTITY="${MACOS_SIGNING_IDENTITY:-}"

if [ -z "${IDENTITY}" ]; then
    echo "warning: no signing identity found, falling back to ad-hoc signature" >&2
    codesign --force --deep --sign - "${APP_PATH}"
    codesign --verify --deep --strict "${APP_PATH}"
    echo "Ad-hoc signed and verified: ${APP_PATH}"
    exit 0
fi

TIMESTAMP="--timestamp"
if [ "${MACOS_CODESIGN_TIMESTAMP:-}" = "none" ]; then
    TIMESTAMP="--timestamp=none"
fi

echo "Signing with identity: ${IDENTITY}"

# Sign Mach-O files first, then bundles from the inside out. Applying the
# entitlements only to executables avoids granting them to libraries.
find "${APP_PATH}/Contents/Frameworks" "${APP_PATH}/Contents/PlugIns" \
    -type f -print | while IFS= read -r FILE; do
    if file "${FILE}" | grep -q 'Mach-O'; then
        codesign --force "${TIMESTAMP}" --options runtime \
            --sign "${IDENTITY}" "${FILE}"
    fi
done

find "${APP_PATH}/Contents" -depth -type d \
    \( -name '*.framework' -o -name '*.app' \) -print | \
    while IFS= read -r BUNDLE; do
        case "${BUNDLE}" in
            *.app)
                codesign --force "${TIMESTAMP}" --options runtime \
                    --entitlements "${ENTITLEMENTS}" \
                    --sign "${IDENTITY}" "${BUNDLE}"
                ;;
            *)
                codesign --force "${TIMESTAMP}" --options runtime \
                    --sign "${IDENTITY}" "${BUNDLE}"
                ;;
        esac
    done

codesign --force "${TIMESTAMP}" --options runtime \
    --entitlements "${ENTITLEMENTS}" \
    --sign "${IDENTITY}" "${APP_PATH}"

codesign --verify --deep --strict --verbose=2 "${APP_PATH}"
echo "Signed and verified: ${APP_PATH}"
