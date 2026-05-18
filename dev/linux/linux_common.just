set working-directory := '../..'

# Ensure submodules and CEF are present
[linux]
deps:
    #!/bin/sh
    set -eu
    if ! [ -e third_party/mpv/.git ]; then
        git submodule update --init --recursive
    fi
    python3 dev/tools/download_cef.py
