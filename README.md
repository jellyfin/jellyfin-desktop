# Jellyfin Desktop

A [Jellyfin](https://jellyfin.org) desktop client built on [CEF](https://github.com/chromiumembedded/cef) and [mpv](https://mpv.io/). A complete rewrite of the previous [Qt-based client](https://github.com/jellyfin-archive/jellyfin-desktop-qt/).

## Downloads
> [!WARNING]
> This client is still under active development and may have bugs or missing features.
> The previous Qt-based Jellyfin Media Player is available at [v1.12.0](https://github.com/jellyfin-archive/jellyfin-desktop-qt/releases/tag/v1.12.0) but is no longer supported.
> See [Supported Versions](https://github.com/jellyfin/jellyfin-desktop/wiki/Supported-Versions) for compatibility details.

### Linux
- AppImage
  - [x86_64](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-linux-appimage/main/linux-appimage-x86_64.zip)
  - [aarch64](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-linux-appimage/main/linux-appimage-aarch64.zip)
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
    [package]
    appimage ...    # [linux] build AppImage
    flatpak ...     # [linux] build Flatpak bundle
    dmg             # [macos] build Apple Disk Image (.dmg)

    [maintenance]
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

### Desktop Localization

The embedded jellyfin-web UI uses Jellyfin's own translation catalogue. Desktop-owned surfaces use a small catalogue in `src/jfn_cef/src/i18n.rs`: the connect overlay follows `navigator.language`, and native/CEF shell strings use the OS locale when available. Unsupported or missing strings fall back to English.

## LLM Development

LLMs were used in the development of this project. LLM-assisted contributions are welcome.
