set working-directory := '../../..'

[private]
mod linux_common '../linux_common.just'

[private]
default:
    @just --list appimage

# Build .AppImage
[linux]
build: linux_common::deps
    ./dev/linux/appimage/build.sh

# Run the .AppImage
[linux]
run: build
    ./dev/linux/appimage/run.sh

# Remove appimage build artifacts
[linux]
clean:
    rm -rf build/appimage
