# Jellyfin Desktop Flatpak

## Build Bundle

```bash
./build.sh
```

Creates `jellyfin-desktop.flatpak`.

## Install Bundle

```bash
flatpak install --user jellyfin-desktop.flatpak
```

## Development

Build and install directly:
```bash
flatpak-builder --install --user --force-clean build-dir org.jellyfin.JellyfinDesktop.yml
```

Test run without installing:
```bash
flatpak-builder --user --force-clean build-dir org.jellyfin.JellyfinDesktop.yml
flatpak-builder --run build-dir org.jellyfin.JellyfinDesktop.yml jellyfin-desktop
```
