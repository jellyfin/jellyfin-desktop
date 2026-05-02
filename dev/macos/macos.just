# macOS-specific recipes (imported by top-level justfile)

[macos]
deps:
    #!/bin/sh
    set -eu
    ./dev/macos/setup.sh
    if ! [ -e third_party/mpv/.git ]; then
        git submodule update --init --recursive
    fi
    python3 dev/tools/download_cef.py

[macos]
build: deps
    ./dev/macos/build.sh

[macos]
test: build
    ./dev/macos/test.sh

[macos]
run: build
    ./dev/macos/run.sh --log-level=debug --log-file=build/run.log

# Run the standalone mpv CLI built from the submodule (forwards args)
[macos]
mpv *args: build
    third_party/mpv/build/mpv {{args}}

# Build app bundle and distributable DMG (outputs to dist/)
[macos]
dmg:
    ./dev/macos/build_bundle.sh
