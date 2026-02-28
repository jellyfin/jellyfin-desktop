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

`setup.bat` installs everything else:
- Visual Studio 2022 Build Tools (v143 toolset)
- CMake, Ninja, 7-Zip, Inno Setup
- aqtinstall, Qt 6.10.1
- libmpv (AVX2 + fallback), VC++ redistributable
- MinGW, WiX (for packaging)

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

- Qt 6.10.1 requires VS 2022 toolset (v143) for ABI compatibility
- Version info centralized in `common.bat`
- `run.bat` adds Qt/mpv to PATH; `bundle.bat` creates standalone packages
