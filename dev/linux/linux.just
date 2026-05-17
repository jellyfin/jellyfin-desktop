# Linux-specific recipes (imported by top-level justfile)

[private]
mod linux_common 'linux_common.just'

# Build the app
[linux]
build: linux_common::deps
    #!/bin/sh
    set -eu
    if ! [ -f build/CMakeCache.txt ]; then
        cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DBUILD_MPV_CLI=ON -DEXTERNAL_MPV_DIR=
    fi
    cmake --build build

# Run tests (ctest + cargo workspace)
[linux]
test: build
    ctest --test-dir build --output-on-failure
    cargo test --manifest-path src/Cargo.toml --workspace

# Run the app
[linux]
run *args: build
    build/jellyfin-desktop --log-level=debug --log-file=build/run.log {{args}}

# Run the mpv CLI
[linux]
run-mpv *args: build
    #!/bin/sh
    set -eu
    build/mpv-build/mpv "$@"

mod appimage 'appimage/appimage.just'
mod flatpak 'flatpak/flatpak.just'
