# Linux-specific recipes (imported by top-level justfile)

# Build AppImage (outputs to dist/)
[linux]
appimage:
    ./dev/appimage/build.sh

# Build Flatpak bundle (outputs to dist/)
[linux]
flatpak:
    ./dev/flatpak/build.sh
