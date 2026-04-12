#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# shellcheck source=sync-ssh.sh
. "$SCRIPT_DIR/sync-ssh.sh"

show_help() {
    cat << EOF
Usage: $(basename "$0") <ssh-host>

Sync, build, and package jellyfin-desktop on a Windows VM via SSH.
Uses rclone for efficient file sync over SSH.

Arguments:
    ssh-host    SSH host
EOF
}

if [ $# -lt 1 ]; then
    show_help >&2
    exit 1
fi

case "$1" in
    -h|--help)
        show_help
        exit 0
        ;;
esac

REMOTE="$1"

# --- Sync ---
sync_to_remote "$REMOTE"

# --- Build mpv from source (meson handles incremental builds) ---
echo "=== Building mpv ==="
ssh "$REMOTE" 'C:\jellyfin-desktop\dev\windows\build_mpv_source.bat'

# --- Build ---
ssh "$REMOTE" 'C:\jellyfin-desktop\dev\windows\build.bat'

# --- Package ---
ssh "$REMOTE" "cd $REMOTE_DIR/build && cmake --install . --prefix install && cpack -G ZIP"

# Find and copy zip back
LOCAL_OUT='dist'
mkdir -p "$LOCAL_OUT"
scp "$REMOTE:$REMOTE_DIR/build/jellyfin-desktop-*.zip" "$LOCAL_OUT/"

echo "Created:"
ls -lh "$LOCAL_OUT"/*.zip
