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

# Run the app inside a nested niri session (stdio stays on this terminal)
[group('run')]
[linux]
[positional-arguments]
run-niri *args:
    env -u JUST_JUSTFILE -u JUST_WORKING_DIRECTORY '{{ source_directory() }}/niri-nested-run' just run "$@"

[group('package')]
mod appimage 'appimage/appimage.just'

[group('package')]
mod flatpak 'flatpak/flatpak.just'
