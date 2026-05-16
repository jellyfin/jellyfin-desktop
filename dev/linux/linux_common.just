set working-directory := '../..'

# Ensure submodules and CEF are present
[linux]
deps:
    #!/bin/sh
    set -eu
    if ! [ -e third_party/mpv/.git ]; then
        git submodule update --init --recursive
    fi
    if ! command -v cargo >/dev/null 2>&1; then
        echo "error: cargo not found in PATH. Install rustup or distro rust toolchain." >&2
        exit 1
    fi
    python3 dev/tools/download_cef.py
