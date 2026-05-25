#!/bin/sh
# Print the full app version: VERSION file contents, with "+<short hash>[-dirty]"
# appended when VERSION is a pre-release (contains "-").
set -eu
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VERSION="$(cat "${REPO_ROOT}/VERSION")"
case "${VERSION}" in
    *-*)
        HASH="$(cd "${REPO_ROOT}" && git rev-parse --short HEAD 2>/dev/null || true)"
        if [ -n "${HASH}" ]; then
            if [ -n "$(cd "${REPO_ROOT}" && git status --porcelain 2>/dev/null)" ]; then
                HASH="${HASH}-dirty"
            fi
            VERSION="${VERSION}+${HASH}"
        fi
        ;;
esac
printf '%s\n' "${VERSION}"
