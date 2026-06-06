# macOS-specific recipes (imported by top-level justfile)

[group('deps')]
[macos]
deps:
    #!/bin/sh
    set -eu
    ./dev/macos/setup.sh
    if ! [ -e third_party/mpv/.git ]; then
        git submodule update --init --recursive
    fi
    cargo xtask fetch-cef

[group('build')]
[macos]
build: deps
    cargo xtask install --mpv-cli --prefix build/output

[group('run')]
[macos]
run *args: build
    "build/output/Jellyfin Desktop.app/Contents/MacOS/jellyfin-desktop" {{args}}

# Run the standalone mpv CLI built from the submodule (forwards args)
[group('run')]
[macos]
run-mpv *args: build
    build/mpv-build/mpv {{args}}

# Build Apple Disk Image (.dmg)
[group('package')]
[macos]
dmg: build
    ./dev/macos/build_dmg.sh
