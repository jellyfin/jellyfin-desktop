#!/usr/bin/env sh
# Jellyfin Desktop - macOS notarization script
# Submits a signed DMG to Apple and staples the notarization ticket.
#
# Requires an App Store Connect API key. Reads:
#   NOTARY_API_KEY_FILE   - path to AuthKey_XXXXXXXXXX.p8
#   NOTARY_API_KEY_ID     - key ID from App Store Connect
#   NOTARY_API_KEY_ISSUER - issuer ID from App Store Connect
# Or set NOTARY_PROFILE to a notarytool keychain profile for local use.
#
# Usage: dev/macos/notarize.sh path-to-dmg

set -eu

SCRIPT_DIR="$(cd "$(dirname "${0}")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

DMG_PATH="${1:?path to DMG required}"

KEY_FILE="${NOTARY_API_KEY_FILE:-}"
KEY_ID="${NOTARY_API_KEY_ID:-}"
ISSUER_ID="${NOTARY_API_KEY_ISSUER:-}"
PROFILE="${NOTARY_PROFILE:-}"

if [ -z "${PROFILE}" ] && [ -z "${KEY_FILE}" ] && \
    [ -z "${KEY_ID}" ] && [ -z "${ISSUER_ID}" ]; then
    echo "notarization: credentials not configured (NOTARY_API_KEY_FILE, NOTARY_API_KEY_ID, NOTARY_API_KEY_ISSUER) - skipping"
    exit 0
fi

if [ -z "${PROFILE}" ] && \
    { [ -z "${KEY_FILE}" ] || [ -z "${KEY_ID}" ] || [ -z "${ISSUER_ID}" ]; }; then
    echo "error: notarization credentials are incomplete" >&2
    exit 1
fi

if [ -z "${PROFILE}" ] && [ ! -f "${KEY_FILE}" ]; then
    echo "error: notarization key not found at ${KEY_FILE}" >&2
    exit 1
fi

if [ ! -f "${DMG_PATH}" ]; then
    echo "error: DMG not found at ${DMG_PATH}" >&2
    exit 1
fi

echo "Submitting to notary service..."
if [ -n "${PROFILE}" ]; then
    xcrun notarytool submit "${DMG_PATH}" \
        --keychain-profile "${PROFILE}" --wait
else
    xcrun notarytool submit "${DMG_PATH}" \
        --key "${KEY_FILE}" \
        --key-id "${KEY_ID}" \
        --issuer "${ISSUER_ID}" \
        --wait
fi

echo "Stapling ticket..."
xcrun stapler staple "${DMG_PATH}"

echo "Verifying stapled ticket..."
xcrun stapler validate "${DMG_PATH}"

echo "Notarization complete."
