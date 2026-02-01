# Jellyfin Desktop CEF

Experimental rewrite of [Jellyfin Desktop](https://github.com/jellyfin/jellyfin-desktop) built on [CEF](https://bitbucket.org/chromiumembedded/cef).

## Downloads
### Linux (X11 and Wayland)
- Arch Linux (AUR): [jellyfin-desktop-cef-git](https://aur.archlinux.org/packages/jellyfin-desktop-cef-git)
- [Flatpak (non-Flathub bundle)](https://nightly.link/jellyfin-labs/jellyfin-desktop-cef/workflows/build-linux-flatpak/main/linux-flatpak.zip)

### macOS
- [Apple Silicon](https://nightly.link/jellyfin-labs/jellyfin-desktop-cef/workflows/build-macos/main/macos-arm64.zip)

After installing, remove quarantine: 
```
sudo xattr -cr /Applications/Jellyfin\ Desktop\ CEF.app
```

### Windows
- [x64](https://nightly.link/jellyfin-labs/jellyfin-desktop-cef/workflows/build-windows/main/windows-x64.zip)
- [arm64](https://nightly.link/jellyfin-labs/jellyfin-desktop-cef/workflows/build-windows/main/windows-arm64.zip)


## Architecture

- **CEF** (Chromium Embedded Framework) - windowless browser for Jellyfin web UI
- **mpv + libplacebo** - gpu-next backend for video with HDR/tone-mapping
- **SDL3** - window management and input
- **Vulkan** - video rendering to Wayland subsurface (Linux) or CAMetalLayer (macOS)
- **OpenGL/EGL** - CEF overlay compositing
- **MPRIS** - D-Bus media controls integration (Linux)

## Building

See [dev/](dev/README.md) for build instructions.

