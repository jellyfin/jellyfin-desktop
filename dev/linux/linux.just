# Linux-specific recipes (imported by top-level justfile)

# Build AppImage via container (podman/docker)
[linux]
appimage:
    ./dev/appimage/build.sh

# Build Flatpak bundle (requires flatpak-builder)
[linux]
flatpak:
    ./dev/flatpak/build.sh
