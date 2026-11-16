# Building Jellyfin Desktop on Windows

## Quick Start

### Setup and build
```cmd
dev\windows\setup.bat   # First time: download dependencies
dev\windows\build.bat   # Build
```
### Run the application
```cmd
dev\windows\run.bat
```
### Run the unit tests
```cmd
dev\windows\test.bat
```

## Prerequisites

- **winget** (Windows Package Manager)

On x64, `setup.bat` installs everything else:
- Visual Studio 2022 Build Tools
- CMake, Ninja, 7-Zip, Inno Setup
- aqtinstall, Qt 6.9.3
- libmpv (AVX2 + fallback), VC++ redistributable
- MinGW, WiX (for packaging)

## Native Windows ARM64

The same scripts select ARM64 automatically on a Windows ARM64 host, including
when launched from an emulated terminal. Install Python and Visual Studio C++
build tools with the **ARM64 compiler**, Windows SDK and CMake/Ninja components
first. Both Visual Studio 2022 and newer installations are discovered.

```cmd
dev\windows\setup.bat
dev\windows\build.bat
dev\windows\test.bat
dev\windows\run.bat
```

ARM64 dependencies are isolated in `dev\windows\deps\arm64`, and build output is
in `build\arm64`. ARM64 uses Qt 6.11.3 and official libmpv 0.42.0 with the
Direct3D 11 render API. Qt Quick, WebEngine and video rendering use Direct3D 11;
no OpenGL compatibility pack or browser software-rendering workaround is needed.
Existing hardware-decoding settings continue to apply.

`aqtinstall` 3.3.0 does not discover Qt's native ARM64 WebEngine extension.
`install-qtwebengine-arm64.ps1` installs it from Qt's official repository and
verifies its published archive checksum. The official libmpv ZIP is SHA-256
pinned in `common.bat`. The SourceForge ARM64 builds cannot be substituted for
the current renderer: they disable OpenGL, and older versions lack libmpv's
Direct3D 11 render API.

The Direct3D renderer uses Qt's RHI headers (`GuiPrivate`), so deployment must use
the exact Qt build used for compilation. CMake enables `USE_MPV_D3D11` by default
only for Windows ARM64; other platforms retain the existing MpvQt/OpenGL path.
The ARM64 test suite additionally checks rendered video pixels, orientation,
resize and renderer destruction/recreation.
Recovery tests retain the same mpv core through scene-graph/graphics recreation,
including playing/paused video-only streams, audio/video, and moving between
windows. They do not force a system-wide GPU reset.

For packaging, install Inno Setup 6 and run:

```cmd
winget install JRSoftware.InnoSetup
dev\windows\bundle.bat
```

An Inno Setup 6 portable installation in
`dev\windows\deps\arm64\tools\innosetup` is also discovered automatically.

The installer and portable ZIP have an `arm64` suffix, include ARM64 runtime
DLLs, and target Windows 11 ARM64. To build the existing x64 version on an ARM64
host instead, set `JELLYFIN_ARCH=x64` before running the scripts; its dependencies
and build directory remain separate.

## Directory Structure

- `dev/windows/deps/` - Downloaded dependencies (Qt, mpv, etc.)
- `build/` - Build output (safe to delete)
- `build/src/Jellyfin Desktop.exe` - Built executable

## Scripts

- `setup.bat` - Download all dependencies
- `build.bat` - Configure and build
- `bundle.bat` - Create installer and portable ZIP
- `run.bat` - Run executable (sets up Qt/mpv in PATH)
- `test.bat` - Run unit tests (sets up Qt/mpv in PATH)
- `common.bat` - Shared variables (sourced by other scripts)

## Clean Build

```cmd
rmdir /s /q build
dev\windows\build.bat
```

## Troubleshooting

### Black Screen / GPU Issues

Try software rendering:
```cmd
dev\windows\run.bat --software-rendering
```

Common causes:
- Outdated GPU drivers
- Missing DirectX components
- Hardware acceleration incompatibility

### Log Files

```
%LOCALAPPDATA%\Jellyfin Desktop\logs\jellyfin-desktop.log
```

## Notes

- x64 retains Qt 6.9.3 and its AVX2/non-AVX2 libmpv selection
- Version info centralized in `common.bat`
- `run.bat` adds Qt/mpv to PATH; `bundle.bat` creates standalone packages
