#!/bin/bash
# Compare Wayland color management protocol messages between
# standalone mpv and jellyfin-desktop --player.
#
# Verifies that the wp_image_description parameters our app sends
# (primaries, transfer function, CLL/FALL, mastering display metadata)
# exactly match what standalone mpv's Vulkan swapchain produces for
# the same content on the same display.
#
# Tests both EDR-on and EDR-off modes automatically using kscreen-doctor.
# Exit code 0 = all match, 1 = mismatch.
#
# Usage:
#   ./dev/linux/color-mgmt-compare.sh --hdr test_hdr.mkv --sdr test_sdr.mkv
set -euo pipefail

APP=./build/jellyfin-desktop
MPV=./third_party/mpv/build/mpv
DURATION=8
HDR_FILE=""
SDR_FILE=""

usage() {
    echo "Usage: $0 --hdr <file> --sdr <file>"
    echo "  --hdr <file>  HDR10 test video (PQ/BT.2020, mastering metadata)"
    echo "  --sdr <file>  SDR test video (BT.709)"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --hdr) HDR_FILE="$2"; shift 2 ;;
        --sdr) SDR_FILE="$2"; shift 2 ;;
        *) usage ;;
    esac
done

if [[ -z "$HDR_FILE" || -z "$SDR_FILE" ]]; then
    usage
fi

for f in "$HDR_FILE" "$SDR_FILE"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: $f not found" >&2
        exit 1
    fi
done

for bin in "$APP" "$MPV"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: $bin not found or not executable" >&2
        exit 1
    fi
done

# Extract the last complete image description parameter block from a
# WAYLAND_DEBUG log. A "complete" block has at least primaries + tf + cll.
# Strips timestamps and object IDs so only the call name + args remain.
# An sRGB identity block (primaries_named(1) + tf_named(1) with nothing else)
# is treated as empty — it's Mesa's default and equivalent to no description.
extract_last_desc() {
    local raw
    raw=$(grep -E 'wp_image_description_creator_params_v1#[0-9]+\.(set_primaries_named|set_tf_named|set_max_cll|set_max_fall|set_mastering_display_primaries|set_mastering_luminance)' "$1" \
        | tail -6 \
        | sed 's/.*wp_image_description_creator_params_v1#[0-9]*\.//' \
        || true)
    # sRGB identity (primaries=1 sRGB, tf=1 sRGB, no HDR metadata) is a no-op
    local trimmed
    trimmed=$(echo "$raw" | grep -v '^$')
    if [[ "$trimmed" == "set_primaries_named(1)"$'\n'"set_tf_named(1)" ]]; then
        return
    fi
    echo "$raw"
}

run_capture() {
    local label="$1" cmd="$2" logfile="$3"
    echo "  Running $label for ${DURATION}s..."
    timeout "$DURATION" bash -c "$cmd" || true
}

compare_videos() {
    local exit_code=0
    for video in "$HDR_FILE" "$SDR_FILE"; do
        local tag
        tag=$(basename "$video" .mkv)
        echo "  --- $tag ---"

        local mpv_log app_log
        mpv_log=$(mktemp)
        app_log=$(mktemp)

        run_capture "mpv" \
            "WAYLAND_DEBUG=1 $MPV --vo=gpu-next --gpu-api=vulkan --target-colorspace-hint=yes --hwdec=no '$video' >/dev/null 2>'$mpv_log'" \
            "$mpv_log"
        run_capture "app" \
            "WAYLAND_DEBUG=1 $APP --player '$video' >/dev/null 2>'$app_log'" \
            "$app_log"

        local mpv_desc app_desc
        mpv_desc=$(extract_last_desc "$mpv_log")
        app_desc=$(extract_last_desc "$app_log")

        if [[ -z "$mpv_desc" && -z "$app_desc" ]]; then
            echo "  Both: no image description set (SDR default) — MATCH"
        elif diff <(echo "$mpv_desc") <(echo "$app_desc") > /dev/null 2>&1; then
            echo "  MATCH"
            echo "$mpv_desc" | sed 's/^/    /'
        else
            echo "  MISMATCH"
            diff --color=always <(echo "$mpv_desc") <(echo "$app_desc") | sed 's/^/    /' || true
            exit_code=1
        fi

        rm -f "$mpv_log" "$app_log"
    done
    return $exit_code
}

# Detect the output name for kscreen-doctor (strip ANSI codes first)
strip_ansi() { sed 's/\x1b\[[0-9;]*m//g'; }
OUTPUT=$(kscreen-doctor --outputs 2>&1 | strip_ansi | grep -oP '(?<=Output: )\d+' | head -1)
if [[ -z "$OUTPUT" ]]; then
    echo "ERROR: could not detect kscreen output" >&2
    exit 1
fi

# Save current EDR policy to restore later
ORIG_EDR=$(kscreen-doctor --outputs 2>&1 | strip_ansi | grep -oP '(?<=Allow EDR: )\S+')

restore_edr() {
    echo ""
    echo "Restoring EDR policy to: $ORIG_EDR"
    kscreen-doctor "output.$OUTPUT.edrPolicy.$ORIG_EDR" 2>/dev/null || true
}
trap restore_edr EXIT

overall=0

echo "========== EDR ON (HDR) =========="
kscreen-doctor "output.$OUTPUT.edrPolicy.always" 2>/dev/null
sleep 1  # let compositor apply
compare_videos || overall=1

echo ""
echo "========== EDR OFF (SDR) =========="
kscreen-doctor "output.$OUTPUT.edrPolicy.never" 2>/dev/null
sleep 1
compare_videos || overall=1

echo ""
if [[ $overall -eq 0 ]]; then
    echo "All tests passed."
else
    echo "FAILED: image descriptions differ."
fi
exit $overall
