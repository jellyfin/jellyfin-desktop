# Linux-specific recipes (imported by top-level justfile)

[private]
mod linux_common 'linux_common.just'

# Build the app
[linux]
build: linux_common::deps
    cargo xtask build --mpv-cli

# Run the app
[linux]
run *args: build
    build/jellyfin-desktop {{args}}

# Run the mpv CLI
[linux]
run-mpv *args: build
    build/mpv-build/mpv {{args}}

mod appimage 'appimage/appimage.just'
mod flatpak 'flatpak/flatpak.just'
