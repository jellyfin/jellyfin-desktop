# Linux-specific recipes (imported by top-level justfile)

# Install build dependencies on Fedora/RHEL
[linux]
install-deps-fedora:
    ./dev/linux/install-deps-fedora.sh

# Build AppImage (outputs to dist/)
[linux]
appimage:
    ./dev/appimage/build.sh

# Build Flatpak bundle (outputs to dist/)
[linux]
flatpak:
    ./dev/flatpak/build.sh
