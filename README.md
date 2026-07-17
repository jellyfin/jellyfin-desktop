# Jellium Desktop

An unofficial [Jellyfin](https://jellyfin.org) desktop client built on [CEF](https://github.com/chromiumembedded/cef) and [mpv](https://mpv.io/).

## Downloads
### Linux
- AppImage
  - [x86_64](https://nightly.link/andrewrabert/jellium-desktop/workflows/build-linux-appimage/main/linux-appimage-x86_64.zip)
  - [aarch64](https://nightly.link/andrewrabert/jellium-desktop/workflows/build-linux-appimage/main/linux-appimage-aarch64.zip)
- Arch Linux (AUR): [jellium-desktop-git](https://aur.archlinux.org/packages/jellium-desktop-git)
- [Flatpak (non-Flathub bundle)](https://nightly.link/andrewrabert/jellium-desktop/workflows/build-linux-flatpak/main/linux-flatpak-x86_64.zip)

### macOS
- [Apple Silicon](https://nightly.link/andrewrabert/jellium-desktop/workflows/build-macos/main/macos-arm64.zip)
- [Intel](https://nightly.link/andrewrabert/jellium-desktop/workflows/build-macos/main/macos-x86_64.zip)

After installing, remove quarantine: 
```
sudo xattr -cr /Applications/Jellium\ Desktop.app
```

### Windows
- [x64](https://nightly.link/andrewrabert/jellium-desktop/workflows/build-windows/main/windows-x64.zip)
- [arm64](https://nightly.link/andrewrabert/jellium-desktop/workflows/build-windows/main/windows-arm64.zip)


## Development

This project uses [just](https://github.com/casey/just) as a command runner.

```
Available recipes:
    [package]
    appimage ...    # [linux] build AppImage
    flatpak ...     # [linux] build Flatpak bundle
    dmg             # [macos] build Apple Disk Image (.dmg)

    [maintenance]
    outdated      # List outdated dependencies
    clean         # Remove build artifacts

    [test]
    test          # Run tests

    [lint]
    fmt           # Format workspace
    fmt-check     # Check formatting
    clippy        # Run clippy
    lint          # Lint workspace
    strict-lint   # Strict lint workspace

    [build]
    build         # Build the app

    [run]
    run *args     # Run the app
    run-mpv *args # Run the mpv CLI
```
