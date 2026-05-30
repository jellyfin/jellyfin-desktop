# Building Jellyfin Desktop on macOS

## Quick Start

```bash
dev/macos/setup.sh    # First time: install dependencies
just build            # Build app bundle (build/output/Jellyfin Desktop.app)
just run              # Run the built bundle
just dmg              # Build distributable DMG (dist/)
```

## Prerequisites

- **Xcode Command Line Tools**: `xcode-select --install`
- **Homebrew**: https://brew.sh

`setup.sh` installs everything else:
- CMake (needed by cef-dll-sys), Ninja, Meson, Rust
- FFmpeg, libplacebo, libass, LuaJIT
- Vulkan (vulkan-loader, vulkan-headers, MoltenVK)
- lcms2, libunibreak, zimg
- create-dmg

## Directory Structure

- `.cache/cef/` - CEF binary distribution (downloaded on first build)
- `third_party/mpv/` - mpv source (git submodule, built via meson by `cargo xtask`)
- `build/` - Build output (safe to delete)
- `build/jellyfin-desktop` - Staged binary tree (build step)
- `build/output/Jellyfin Desktop.app` - App bundle (install step)
- `dist/` - Distributable DMG output

## Build commands

All driven by `cargo xtask` via `just`:

- `just build` → `cargo xtask install --mpv-cli --prefix build/output`
- `just dmg`   → builds the bundle then runs `dev/macos/build_dmg.sh`

`cargo xtask build` alone produces `build/jellyfin-desktop` + staged runtime
resources without assembling an .app bundle.

## Clean Build

```bash
just clean
just build
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
cargo xtask fetch-cef
```

### Web Debugger

To get browser devtools:
```bash
just run -- --remote-debugging-port=9222
```
Then open Chrome and navigate to `chrome://inspect/#devices`.

## Notes

- Intel and Apple Silicon both supported
- macOS 11+ required
- mpv is built from source (third_party/mpv submodule)
- CEF is downloaded automatically on first build
