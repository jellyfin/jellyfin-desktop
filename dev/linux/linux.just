# Linux-specific recipes (imported by top-level justfile)

# Build AppImage (outputs to dist/)
[linux]
appimage:
    ./dev/linux/appimage/build.sh

# Build Flatpak bundle (outputs to dist/)
[linux]
flatpak:
    ./dev/linux/flatpak/build.sh
