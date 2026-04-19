# Building Jellyfin Desktop on macOS

## Quick Start

```bash
dev/macos/setup.sh   # First time: install dependencies
dev/macos/build.sh   # Build
dev/macos/run.sh     # Run
```

## Prerequisites

- **Xcode Command Line Tools**: `xcode-select --install`
- **Homebrew**: https://brew.sh

`setup.sh` installs everything else:
- CMake, Ninja, Meson
- FFmpeg, libplacebo, libass, LuaJIT
- Vulkan (vulkan-loader, vulkan-headers, MoltenVK)
- lcms2, libunibreak, zimg
- create-dmg

## Directory Structure

- `third_party/cef/` - CEF binary distribution (downloaded by build.sh)
- `third_party/mpv/` - mpv source (git submodule, built by cmake)
- `build/` - Build output (safe to delete)
- `build/jellyfin-desktop` - Dev executable
- `build/output/Jellyfin Desktop.app` - App bundle (from bundle.sh)

## Scripts

- `setup.sh` - Install all dependencies
- `build.sh` - Configure and build
- `bundle.sh` - Create app bundle and DMG for distribution
- `run.sh` - Run the built executable (passes arguments through)
- `common.sh` - Shared variables (sourced by other scripts)

## Clean Build

```bash
rm -rf build
dev/macos/build.sh
```

## Troubleshooting

### Missing Dependencies

If build fails with missing libraries, re-run setup:
```bash
dev/macos/setup.sh
```

### CEF Download Issues

Manually download CEF:
```bash
python3 dev/tools/download_cef.py
```

### Web Debugger

To get browser devtools:
```bash
dev/macos/run.sh --remote-debugging-port=9222
```
Then open Chrome and navigate to `chrome://inspect/#devices`.

## Notes

- Intel and Apple Silicon both supported
- macOS 11+ required
- mpv is built from source (third_party/mpv submodule)
- CEF is downloaded automatically on first build
