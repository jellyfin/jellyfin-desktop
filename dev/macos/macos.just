# macOS-specific recipes (imported by top-level justfile)

[macos]
deps:
    #!/bin/sh
    set -eu
    ./dev/macos/setup.sh
    if ! [ -e third_party/mpv/.git ]; then
        git submodule update --init --recursive
    fi
    cargo xtask fetch-cef

[macos]
build: deps
    cargo xtask install --mpv-cli --prefix build/output

[macos]
run *args: build
    "build/output/Jellyfin Desktop.app/Contents/MacOS/jellyfin-desktop" {{args}}

# Run the standalone mpv CLI built from the submodule (forwards args)
[macos]
run-mpv *args: build
    build/mpv-build/mpv {{args}}

# Build app bundle and distributable DMG (outputs to dist/)
[macos]
dmg: build
    ./dev/macos/build_dmg.sh
