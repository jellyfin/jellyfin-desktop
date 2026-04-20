#!/bin/sh
# Print the full app version: VERSION file contents, with "+<git describe>"
# appended when VERSION is a pre-release (contains "-").
set -eu
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
VERSION="$(cat "${REPO_ROOT}/VERSION")"
case "${VERSION}" in
    *-*) VERSION="${VERSION}+$(cd "${REPO_ROOT}" && git describe --always --dirty)" ;;
esac
printf '%s\n' "${VERSION}"
