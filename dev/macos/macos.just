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
    cargo xtask install --mpv-cli --prefix build/output

[macos]
test: build
    cargo test --manifest-path src/Cargo.toml --workspace --exclude jfn-wlproxy

[macos]
run *args: build
    "build/output/Jellyfin Desktop.app/Contents/MacOS/jellyfin-desktop" --log-level=debug --log-file=build/run.log {{args}}

# Run the standalone mpv CLI built from the submodule (forwards args)
[macos]
run-mpv *args: build
    build/mpv-build/mpv {{args}}

# Build app bundle and distributable DMG (outputs to dist/)
[macos]
dmg: build
    ./dev/macos/build_dmg.sh
