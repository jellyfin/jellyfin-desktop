# Jellyfin Desktop

> [!WARNING]
> This client is still under active development and may have bugs or missing features.

A [Jellyfin](https://jellyfin.org) desktop client built on [CEF](https://github.com/chromiumembedded/cef) and [mpv](https://mpv.io/). A complete rewrite of the previous [Qt-based client](https://github.com/jellyfin-archive/jellyfin-desktop-qt/).

## Downloads
### Linux
- [AppImage](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-linux-appimage/main/linux-appimage-x86_64.zip)
- Arch Linux (AUR): [jellyfin-desktop-git](https://aur.archlinux.org/packages/jellyfin-desktop-git)
- [Flatpak (non-Flathub bundle)](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-linux-flatpak/main/linux-flatpak-x86_64.zip)

### macOS
- [Apple Silicon](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-macos/main/macos-arm64.zip)
- [Intel](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-macos/main/macos-x86_64.zip)

After installing, remove quarantine: 
```
sudo xattr -cr /Applications/Jellyfin\ Desktop.app
```

### Windows
- [x64](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-windows/main/windows-x64.zip)
- [arm64](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-windows/main/windows-arm64.zip)


## Development

This project uses [just](https://github.com/casey/just) as a command runner.

```
Available recipes:
    appimage               # Build AppImage (outputs to dist/)
    build                  # Configure (if needed) + build the main app
    clean                  # Remove build artifacts (keeps CEF SDK download)
    deps                   # Ensure submodules and CEF are present
    flatpak                # Build Flatpak bundle (outputs to dist/)
    install-deps-fedora    # Install build dependencies on Fedora/RHEL
    list                   # List available recipes
    mpv *args              # Run the standalone mpv CLI built from the submodule (forwards args)
    run *args              # Run the app with debug logging (logs to build/run.log)
    test                   # Run unit tests
    update-deps *args      # Update vendored/fetched deps (CEF, doctest, quill); pass --check to verify only
```

### Building from source on Linux

`just build` handles everything: submodule init, CEF download, mpv build, and the app itself. It builds mpv from the bundled submodule (a fork that exposes Wayland surface handles required for the overlay architecture).

**Arch Linux** — install the [AUR package](https://aur.archlinux.org/packages/jellyfin-desktop-git) or use the packages listed in `dev/appimage/Dockerfile`.

**Fedora / RHEL** — install dependencies first (requires [RPM Fusion free](https://rpmfusion.org/Configuration) for `ffmpeg-devel`):

```sh
just install-deps-fedora
just build
```

> **ARM64 / Asahi Linux note:** `just build` works natively on `aarch64` — no cross-compilation needed. The pre-built AppImage and Flatpak are x86_64 only, so building from source is the supported path on ARM64.
