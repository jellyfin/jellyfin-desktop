# macOS-specific recipes (imported by top-level justfile)

[macos]
deps:
    #!/bin/sh
    set -eu
    ./dev/macos/setup.sh
    if ! [ -e third_party/mpv/.git ]; then
        git submodule update --init --recursive
    fi
    if ! [ -d third_party/cef ]; then
        python3 dev/download_cef.py
    fi

[macos]
build: deps
    ./dev/macos/build.sh

[macos]
test: build
    ./dev/macos/test.sh

[macos]
run: build
    ./dev/macos/run.sh --log-level=debug --log-file=build/run.log

# Build app bundle and distributable DMG
[macos]
dmg:
    ./dev/macos/build_bundle.sh
