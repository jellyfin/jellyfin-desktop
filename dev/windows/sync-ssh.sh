#!/usr/bin/env sh
# Sync local source tree to a Windows remote via SSH using rclone.
# Sourced by other scripts; can also be run directly.
#
# Usage (direct):  sync-ssh.sh <ssh-host>
# Usage (sourced): . sync-ssh.sh && sync_to_remote <ssh-host>

REMOTE_DIR='C:/jellyfin-desktop'

sync_to_remote() {
    _remote="$1"

    rclone --config /dev/null sync . ":sftp,ssh='ssh $_remote':$REMOTE_DIR" \
        --exclude '.git/**' \
        --exclude '/build/**' \
        --exclude '/dist/**' \
        --exclude '.cache/**' \
        --exclude '.vscode/**' \
        --exclude '.idea/**' \
        --exclude '*.swp' \
        --exclude '/compile_commands.json' \
        --exclude '/out' \
        --exclude '/app.log' \
        --exclude '.mcp.json' \
        --exclude '.flatpak-builder/**' \
        --exclude 'build-dir/**' \
        --filter '- /third_party/mpv/build/**' \
        --filter '+ /third_party/mpv/**' \
        --filter '+ /third_party/letsmove/**' \
        --filter '+ /third_party/quill/**' \
        --filter '+ /third_party/GL/**' \
        --filter '+ /third_party/KHR/**' \
        --filter '- /third_party/**' \
        -P

    echo "Synced to $_remote:$REMOTE_DIR"
}

# Run directly if not sourced
if [ "${0##*/}" = "sync-ssh.sh" ] && [ $# -ge 1 ]; then
    case "$1" in
        -h|--help)
            echo "Usage: $(basename "$0") <ssh-host>"
            echo "Sync local source tree to Windows remote."
            exit 0
            ;;
    esac
    sync_to_remote "$1"
fi
