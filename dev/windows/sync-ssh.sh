#!/usr/bin/env sh
# Sync local source tree to a Windows remote via SSH.
# Sourced by other scripts; can also be run directly.
#
# Usage (direct):  sync-ssh.sh <ssh-host>
# Usage (sourced): . sync-ssh.sh && sync_to_remote <ssh-host>

REMOTE_DIR='C:/jellyfin-desktop-cef'
REMOTE_TMP='C:/Users/user/AppData/Local/Temp'

sync_to_remote() {
    _remote="$1"
    _archive='/tmp/jellyfin-desktop-cef.tar'
    _sync_marker='.build-ssh-sync'
    _remote_marker="$(echo "$REMOTE_DIR/$_sync_marker" | tr '/' '\\')"
    _remote_archive="$REMOTE_TMP/jellyfin-desktop-cef.tar"

    if ! ssh "$_remote" "dir $_remote_marker" >/dev/null 2>&1; then
        echo "First sync: sending full source tree..."
        git archive --format=tar HEAD > "$_archive"

        # Overlay uncommitted tracked changes
        git diff --name-only HEAD | while read -r file; do
            [ -f "$file" ] && tar --update -f "$_archive" "$file"
        done

        # Add untracked files (excluding gitignored)
        git ls-files --others --exclude-standard | while read -r file; do
            [ -f "$file" ] && tar --update -f "$_archive" "$file"
        done

        # Include submodule contents (git archive excludes them)
        git submodule foreach --quiet 'echo $sm_path' | while read -r sm; do
            if [ -d "$sm" ]; then
                echo "  Including submodule: $sm"
                git -C "$sm" archive --format=tar --prefix="$sm/" HEAD > /tmp/sub.tar
                tar --concatenate -f "$_archive" /tmp/sub.tar
                rm -f /tmp/sub.tar
            fi
        done
    else
        echo "Incremental sync: sending changed files..."
        {
            git diff --name-only HEAD
            git diff --name-only --cached
            git ls-files --others --exclude-standard
            # Include submodule changes
            git submodule foreach --quiet \
                'git diff --name-only HEAD 2>/dev/null | sed "s|^|$sm_path/|"'
        } | sort -u > /tmp/changed-files.txt

        if [ -s /tmp/changed-files.txt ]; then
            tar -cf "$_archive" -T /tmp/changed-files.txt 2>/dev/null || true
        else
            echo "No changes to sync."
            tar -cf "$_archive" --files-from=/dev/null
        fi
        rm -f /tmp/changed-files.txt
    fi

    scp "$_archive" "$_remote:$_remote_archive"
    ssh "$_remote" "cd $REMOTE_DIR && tar -xf $_remote_archive && echo synced > $_sync_marker"

    rm -f "$_archive"
    ssh "$_remote" "del \"$(echo "$_remote_archive" | tr '/' '\\')\"" 2>/dev/null || true

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
