#!/usr/bin/env sh
# Sync source and build mpv from submodule on a Windows remote via SSH.
#
# Usage: build-mpv-source-ssh.sh <ssh-host>
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=sync-ssh.sh
. "$SCRIPT_DIR/sync-ssh.sh"

if [ $# -lt 1 ]; then
    echo "Usage: $(basename "$0") <ssh-host>" >&2
    exit 1
fi

case "$1" in
    -h|--help)
        echo "Usage: $(basename "$0") <ssh-host>"
        echo "Sync source and build mpv from submodule on Windows remote."
        exit 0
        ;;
esac

REMOTE="$1"

sync_to_remote "$REMOTE"

echo "=== Building mpv from source ==="
ssh "$REMOTE" 'C:\jellyfin-desktop\dev\windows\build_mpv_source.bat -Force'
