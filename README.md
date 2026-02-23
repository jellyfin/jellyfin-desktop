# Jellyfin Desktop

Jellyfin desktop client built with Qt WebEngine and [libmpv](https://github.com/mpv-player/mpv). Supports audio passthrough, hardware decoding, and playback of more formats without transcoding.

![Screenshot of Jellyfin Desktop](screenshots/video_player.png)

## Downloads
- [Windows, Mac, and Linux Releases](https://github.com/jellyfin/jellyfin-desktop/releases)
  - Note for Mac users: builds for Intel require macOS 12+ and Apple Silicon builds requires macOS 14+
- [Flathub (Linux)](https://flathub.org/apps/details/org.jellyfin.JellyfinDesktop)

### Development Builds
Built from the latest commit on `master`.

#### macOS
- [Apple Silicon](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-macos/master/macos-arm64.zip)
- [Intel](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-macos/master/macos-x86_64.zip)

#### Windows
- [x64 Installer](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-windows/master/windows-x64-installer.zip)
- [x64 Portable](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-windows/master/windows-x64-portable.zip)

#### Linux
- [AppImage (x86_64)](https://nightly.link/jellyfin/jellyfin-desktop/workflows/build-appimage/master/linux-appimage-x86_64.zip)

## Building
See [dev/](dev/) for platform-specific build instructions.

## File Locations
Data is stored per-profile in a `profiles/<profile-id>/` subdirectory. The main configuration file is `jellyfin-desktop.conf`. You can also add `mpv.conf` to configure MPV directly.

**Windows:**
- Config: `%LOCALAPPDATA%\Jellyfin Desktop\profiles\<profile-id>\`
- Cache: `%LOCALAPPDATA%\Jellyfin Desktop\profiles\<profile-id>\`
- Logs: `%LOCALAPPDATA%\Jellyfin Desktop\profiles\<profile-id>\logs\`

**Linux:**
- Config: `~/.local/share/jellyfin-desktop/profiles/<profile-id>/`
- Cache: `~/.cache/jellyfin-desktop/profiles/<profile-id>/`
- Logs: `~/.local/share/jellyfin-desktop/profiles/<profile-id>/logs/`

**Linux (Flatpak):**
- Config: `~/.var/app/org.jellyfin.JellyfinDesktop/data/jellyfin-desktop/profiles/<profile-id>/`
- Cache: `~/.var/app/org.jellyfin.JellyfinDesktop/cache/jellyfin-desktop/profiles/<profile-id>/`
- Logs: `~/.var/app/org.jellyfin.JellyfinDesktop/data/jellyfin-desktop/profiles/<profile-id>/logs/`

**macOS:**
- Config: `~/Library/Application Support/Jellyfin Desktop/profiles/<profile-id>/`
- Cache: `~/Library/Caches/Jellyfin Desktop/profiles/<profile-id>/`
- Logs: `~/Library/Logs/Jellyfin Desktop/<profile-id>/`
