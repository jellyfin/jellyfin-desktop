#!/usr/bin/env sh
set -eu

REMOTE_DIR='C:/jellyfin-desktop-cef'
REMOTE_TMP='C:/Users/user/AppData/Local/Temp'
LOCAL_OUT='dist'
ARCHIVE='/tmp/jellyfin-desktop-cef.tar'

show_help() {
    cat << EOF
Usage: $(basename "$0") <ssh-host>

Sync, build, and package jellyfin-desktop-cef on a Windows VM via SSH.
Uses incremental tar sync (only changed files after first full sync).

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
SYNC_MARKER=".build-ssh-sync"

# --- Sync ---
# Check if remote has the repo (first sync = full archive)
REMOTE_MARKER="$(echo "$REMOTE_DIR/$SYNC_MARKER" | tr '/' '\\')"
if ! ssh "$REMOTE" "dir $REMOTE_MARKER" >/dev/null 2>&1; then
    echo "First sync: sending full source tree..."
    git archive --format=tar HEAD > "$ARCHIVE"

    # Overlay uncommitted tracked changes
    git diff --name-only HEAD | while read -r file; do
        [ -f "$file" ] && tar --update -f "$ARCHIVE" "$file"
    done

    # Add untracked files (excluding gitignored)
    git ls-files --others --exclude-standard | while read -r file; do
        [ -f "$file" ] && tar --update -f "$ARCHIVE" "$file"
    done
else
    echo "Incremental sync: sending changed files..."
    # Collect all files that differ from what's on the remote
    # This includes: staged, unstaged, and untracked (non-ignored)
    {
        git diff --name-only HEAD
        git diff --name-only --cached
        git ls-files --others --exclude-standard
    } | sort -u > /tmp/changed-files.txt

    if [ -s /tmp/changed-files.txt ]; then
        # Create tar of just the changed files
        tar -cf "$ARCHIVE" -T /tmp/changed-files.txt 2>/dev/null || true
    else
        echo "No changes to sync."
        # Still run build in case remote needs it
        tar -cf "$ARCHIVE" --files-from=/dev/null
    fi
    rm -f /tmp/changed-files.txt
fi

REMOTE_ARCHIVE="$REMOTE_TMP/jellyfin-desktop-cef.tar"
scp "$ARCHIVE" "$REMOTE:$REMOTE_ARCHIVE"

# Extract on VM (overwrite existing files)
ssh "$REMOTE" "cd $REMOTE_DIR && tar -xf $REMOTE_ARCHIVE && echo synced > $REMOTE_MARKER"

# Clean up
rm -f "$ARCHIVE"
ssh "$REMOTE" "del \"$(echo "$REMOTE_ARCHIVE" | tr '/' '\\')\"" 2>/dev/null || true

echo "Synced to $REMOTE:$REMOTE_DIR"

# --- Build ---
ssh "$REMOTE" 'C:\jellyfin-desktop-cef\dev\windows\build.bat'

# --- Package ---
ssh "$REMOTE" "cd $REMOTE_DIR/build && cmake --install . --prefix install && cpack -G ZIP"

# Find and copy zip back
mkdir -p "$LOCAL_OUT"
scp "$REMOTE:$REMOTE_DIR/build/jellyfin-desktop-cef-*.zip" "$LOCAL_OUT/"

echo "Created:"
ls -lh "$LOCAL_OUT"/*.zip
