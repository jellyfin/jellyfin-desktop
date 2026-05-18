set working-directory := '../../..'

[private]
mod linux_common '../linux_common.just'

[private]
default:
    @just --list flatpak

# Build Flatpak bundle
[linux]
build: linux_common::deps
    ./dev/linux/flatpak/build.sh
