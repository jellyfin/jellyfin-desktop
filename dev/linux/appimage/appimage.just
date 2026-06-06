set working-directory := '../../..'

[private]
mod linux_common '../linux_common.just'

[private]
default:
    @just --list appimage

# Build .AppImage
[group('package')]
[linux]
build: linux_common::deps
    ./dev/linux/appimage/build.sh

# Run the .AppImage
[group('run')]
[linux]
run: build
    ./dev/linux/appimage/run.sh

# Remove appimage build artifacts
[group('maintenance')]
[linux]
clean:
    rm -rf build/appimage
