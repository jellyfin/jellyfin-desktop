# Building Jellyfin Desktop on macOS

## Quick Start

### Setup and build
```bash
dev/macos/setup.sh   # First time: install dependencies
dev/macos/build.sh   # Build
```
### Run the application
```bash
dev/macos/run.sh
```
### Run the unit tests
```bash
dev/macos/test.sh
```

## Prerequisites

- **Xcode Command Line Tools**: `xcode-select --install`
- **Homebrew**: https://brew.sh

`setup.sh` installs everything else:
- CMake, Ninja, create-dmg
- aqtinstall, Qt 6.10.1
- mpv

## Directory Structure

- `dev/macos/deps/` - Downloaded dependencies (Qt)
- `build/` - Build output (safe to delete)
- `build/src/Jellyfin Desktop.app` - Dev build (unbundled, for run.sh)
- `build/output/Jellyfin Desktop.app` - Release build (bundled, from bundle.sh)

## Scripts

- `setup.sh` - Install all dependencies
- `build.sh` - Configure and build
- `bundle.sh` - Create DMG for distribution
- `run.sh` - Run the built app (passes arguments through)
- `test.sh` - Run unit tests (sets up Qt/mpv in PATH)
- `common.sh` - Shared variables (sourced by other scripts)

## Clean Build

```bash
rm -rf build
dev/macos/build.sh
```

## Troubleshooting

### Black Screen / GPU Issues

Try software rendering:
```bash
dev/macos/run.sh --software-rendering
```

### Log Files

```
~/Library/Logs/Jellyfin Desktop/
```

## Notes

- Intel builds require macOS 12+
- Apple Silicon builds require macOS 14+
- Qt 6.10+ required for WebEngine support
