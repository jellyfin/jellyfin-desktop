# Linux-specific recipes (imported by top-level justfile)

[private]
mod linux_common 'linux_common.just'

# Build the app
[group('build')]
[linux]
build: linux_common::deps
    cargo xtask build --mpv-cli

# Run the app
[group('run')]
[linux]
run *args: build
    build/jellium-desktop {{args}}

# Run the mpv CLI
[group('run')]
[linux]
run-mpv *args: build
    build/mpv-build/mpv {{args}}

[group('package')]
mod appimage 'appimage/appimage.just'

[group('package')]
mod flatpak 'flatpak/flatpak.just'
