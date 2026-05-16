set working-directory := '../..'

# Ensure submodules and CEF are present
[linux]
deps:
    #!/bin/sh
    set -eu
    if ! [ -e third_party/mpv/.git ] || ! [ -e third_party/wl-proxy/.git ]; then
        git submodule update --init --recursive
    fi
    if ! command -v cargo >/dev/null 2>&1; then
        echo "error: cargo not found in PATH. Install rustup or distro rust toolchain." >&2
        exit 1
    fi
    # Overlay protocol XMLs onto wl-proxy and regenerate bindings if any
    # overlay is newer than the corresponding file already in the submodule
    # (covers fresh clones + edits to dev/linux/wl-proxy-overlay/).
    needs_regen=0
    for src in dev/linux/wl-proxy-overlay/*.xml; do
        dst="third_party/wl-proxy/protocols/external/$(basename "$src")"
        if ! [ -e "$dst" ] || ! cmp -s "$src" "$dst"; then
            cp "$src" "$dst"
            needs_regen=1
        fi
    done
    if [ "$needs_regen" = 1 ]; then
        echo "Regenerating wl-proxy bindings with overlay XMLs..."
        cargo run --release --manifest-path third_party/wl-proxy/generator/Cargo.toml
    fi
    python3 dev/tools/download_cef.py
