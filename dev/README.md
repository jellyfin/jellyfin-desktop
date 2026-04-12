# Development

## Quick Start (Linux)

```sh
git clone https://github.com/jellyfin/jellyfin-desktop
cd jellyfin-desktop
git submodule update --init --recursive
python3 dev/download_cef.py
cmake -B build -G Ninja
cmake --build build
./build/jellyfin-desktop
```

## Quick Start (macOS)

```sh
git clone https://github.com/jellyfin/jellyfin-desktop
cd jellyfin-desktop
git submodule update --init --recursive
python3 dev/download_cef.py
cmake -B build -G Ninja
cmake --build build
dev/macos/run.sh
```

## Quick Start (Windows)

See [dev/windows/README.md](windows/README.md) for detailed instructions.

```powershell
git clone https://github.com/jellyfin/jellyfin-desktop
cd jellyfin-desktop
.\dev\windows\setup.ps1
.\dev\windows\build.ps1
```

## CEF Version

The target CEF version is pinned in `CEF_VERSION` at the repo root. All platforms use this version.

- `dev/download_cef.py` reads it automatically when downloading CEF
- CMake verifies the installed CEF matches at configure time
- CI cache keys are based on it

To bump CEF:

```sh
echo "NEW_VERSION_STRING" > CEF_VERSION
python3 dev/download_cef.py              # download new version
python3 dev/flatpak/update_cef.py        # update Flatpak manifest URL + sha256
```

## Flatpak (Linux)

See [dev/flatpak/README.md](flatpak/README.md) for details.

```sh
cd dev/flatpak
./build.sh
flatpak install --user jellyfin-desktop.flatpak
```

## Web Debugger

To get browser devtools, use remote debugging:

1. Run with `--remote-debugging-port=9222`
2. Open Chromium/Chrome and navigate to `chrome://inspect/#devices`
3. Make sure "Discover Network Targets" is checked and `localhost:9222` is configured
