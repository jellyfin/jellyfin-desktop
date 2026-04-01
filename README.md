# Jellyfin Desktop

> [!WARNING]
> This client is still under active development and may have bugs or missing features.

A [Jellyfin](https://jellyfin.org) desktop client built on [CEF](https://bitbucket.org/chromiumembedded/cef). A complete rewrite of the previous [Qt-based client](https://github.com/jellyfin-archive/jellyfin-desktop-qt/).

- **CEF** - embedded Chromium browser
- **mpv** - forked libmpv: gpu-next, Vulkan, HDR passthrough (Linux Wayland, macOS, Windows)
- **SDL3** - cross-platform window management and input

## Downloads
### Linux (X11 and Wayland)
- [AppImage](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-linux-appimage/main/linux-appimage-x86_64.zip)
- Arch Linux (AUR): [jellyfin-desktop-git](https://aur.archlinux.org/packages/jellyfin-desktop-git)
- [Flatpak (non-Flathub bundle)](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-linux-flatpak/main/linux-flatpak.zip)

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


## Building

See [dev/](dev/README.md) for build instructions.

