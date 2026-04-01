# Windows Development Setup

## Prerequisites

Install the following:

- **Visual Studio 2022** (Community, Professional, or Build Tools) with "Desktop development with C++" workload
- **Python 3.12+**: `winget install Python.Python.3.12`
- **CMake**: `winget install Kitware.CMake`
- **Ninja**: `winget install Ninja-build.Ninja`
- **7-Zip**: `winget install 7zip.7zip`
- **Vulkan SDK**: `winget install KhronosGroup.VulkanSDK`

## Quick Start

```powershell
# Clone and setup
git clone https://github.com/jellyfin/jellyfin-desktop
cd jellyfin-desktop

# Run setup (downloads CEF, SDL3, mpv)
.\dev\windows\setup.ps1

# Build (auto-detects VS and Vulkan SDK)
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
python dev\download_cef.py
```

### 3. SDL3

Downloaded automatically by `setup.ps1`, or manually:

```powershell
# Download SDL3 VC development package from GitHub releases
# Extract to third_party\SDL
```

### 4. libmpv

```powershell
.\dev\windows\build_mpv_source.ps1
```

This builds libmpv from the mpv submodule source using MSYS2, and generates an MSVC import library.

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
| `setup.ps1` | Full environment setup (CEF, SDL3, mpv, checks prerequisites) |
| `build.bat` | Entry point: loads VS environment and runs build.ps1 |
| `build.ps1` | Configure and build with Ninja |
| `build_cef.ps1` | Build CEF wrapper library |
| `build_mpv.ps1` | Setup mpv import library (from MSYS2 or downloaded) |
| `build_mpv_source.ps1` | Build mpv from submodule source using MSYS2 |

## Troubleshooting

### "MSVC environment not detected"

The scripts auto-detect VS installations (including Build Tools). If detection fails, run from "x64 Native Tools Command Prompt for VS 2022".

### CMake can't find Vulkan

`build.ps1` auto-detects `C:\VulkanSDK`. If installed elsewhere, set the `VULKAN_SDK` environment variable.

### CMake can't find SDL3

Ensure `third_party\SDL` exists with the prebuilt VC package. Re-run `setup.ps1` or download manually from [SDL releases](https://github.com/libsdl-org/SDL/releases).
