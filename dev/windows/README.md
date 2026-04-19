# Windows Development Setup

## Prerequisites

Install the following:

- **Visual Studio 2022** (Community, Professional, or Build Tools) with "Desktop development with C++" workload
- **Python 3.12+**: `winget install Python.Python.3.12`
- **CMake**: `winget install Kitware.CMake`
- **Ninja**: `winget install Ninja-build.Ninja`
- **7-Zip**: `winget install 7zip.7zip`

## Quick Start

```powershell
# Clone and setup
git clone https://github.com/jellyfin/jellyfin-desktop
cd jellyfin-desktop

# Run setup (downloads CEF, builds mpv)
.\dev\windows\setup.ps1

# Build (auto-detects VS)
.\dev\windows\build.bat
```

Or from a VS Developer Command Prompt:

```powershell
.\dev\windows\build.ps1
```

## Manual Setup

### 1. Git Submodules

```powershell
git submodule update --init --recursive
```

### 2. CEF (Chromium Embedded Framework)

```powershell
python dev\tools\download_cef.py
```

### 3. mpv

```powershell
.\dev\windows\build_mpv_source.ps1
```

This builds mpv from the submodule source using MSYS2, and generates an MSVC import library.

## Building

```powershell
# Using build.bat (auto-detects VS installation):
.\dev\windows\build.bat

# Or from VS Developer Command Prompt:
.\dev\windows\build.ps1
```

### CMake Options

| Option | Description |
|--------|-------------|
| `EXTERNAL_MPV_DIR` | Path to mpv installation (auto-detected from `third_party/mpv-install`) |
| `EXTERNAL_CEF_DIR` | Path to CEF installation (auto-detected from `third_party/cef`) |

## Scripts

| Script | Description |
|--------|-------------|
| `setup.ps1` | Full environment setup (CEF, mpv, checks prerequisites) |
| `build.bat` | Entry point: loads VS environment and runs build.ps1 |
| `build.ps1` | Configure and build with Ninja |
| `build_cef.ps1` | Build CEF wrapper library |
| `build_mpv_source.ps1` | Build mpv from submodule source using MSYS2 |

## Troubleshooting

### "MSVC environment not detected"

The scripts auto-detect VS installations (including Build Tools). If detection fails, run from "x64 Native Tools Command Prompt for VS 2022".
