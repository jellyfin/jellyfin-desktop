---
name: investigate-sdl
description: Use when needing to understand SDL APIs, look up SDL function signatures or documentation, debug SDL issues, or find how SDL features are implemented
---

# Investigating SDL Source Code

## Overview

SDL (Simple DirectMedia Layer) provides cross-platform window management, input handling, and event loops for this app. The upstream source is the authoritative reference for API behavior, platform-specific backends, and hints.

## Repo Location

SDL source lives in `third_party/` (gitignored):
- `third_party/sdl_src` - full upstream source for reference

**Note:** `third_party/SDL` is the build tree used by CMake -- it is NOT the full source repo. Always use `third_party/sdl_src` when investigating APIs or implementation.

Clone if not present:

```sh
git clone https://github.com/libsdl-org/SDL.git third_party/sdl_src
```

## Key Directories

| Path | Purpose |
|------|---------|
| `include/SDL3/` | **Public API headers** - primary reference for all SDL functions |
| `src/video/` | Window management, display, and platform video backends |
| `src/video/wayland/` | Wayland backend (subsurfaces, decorations, hit testing) |
| `src/video/x11/` | X11 backend |
| `src/events/` | Event pump, keyboard/mouse/window event handling |
| `src/render/` | SDL_Renderer backends (software, OpenGL, Vulkan, GPU) |
| `src/gpu/` | SDL_GPU abstraction layer |
| `src/audio/` | Audio subsystem and platform backends |
| `src/joystick/` | Joystick/gamepad input |
| `src/haptic/` | Force feedback / haptics |
| `src/core/` | Platform core init (linux/, windows/, etc.) |
| `src/thread/` | Threading primitives per platform |
| `src/timer/` | Timer implementation per platform |
| `src/tray/` | System tray support |
| `test/` | Test programs - useful usage examples |
| `examples/` | Example programs demonstrating SDL features |
| `docs/` | Documentation and migration guides |

## Common Lookup Patterns

### Finding a function signature
SDL3 headers follow the naming convention `SDL3/SDL_<subsystem>.h`:

- Window/display: `include/SDL3/SDL_video.h`
- Events: `include/SDL3/SDL_events.h`
- Keyboard: `include/SDL3/SDL_keyboard.h`
- Mouse: `include/SDL3/SDL_mouse.h`
- Rendering: `include/SDL3/SDL_render.h`
- GPU: `include/SDL3/SDL_gpu.h`
- Audio: `include/SDL3/SDL_audio.h`
- Hints/config: `include/SDL3/SDL_hints.h`
- Init/quit: `include/SDL3/SDL_init.h`
- Surface/pixels: `include/SDL3/SDL_surface.h`, `include/SDL3/SDL_pixels.h`
- Vulkan: `include/SDL3/SDL_vulkan.h`
- Clipboard: `include/SDL3/SDL_clipboard.h`
- Filesystem: `include/SDL3/SDL_filesystem.h`
- Properties: `include/SDL3/SDL_properties.h`

### Finding platform-specific behavior
Platform backends are in subdirectories of the relevant subsystem:

- Wayland video: `src/video/wayland/`
- X11 video: `src/video/x11/`
- Windows video: `src/video/windows/`
- Linux core: `src/core/linux/`

### Finding hints
SDL behavior is often configurable via hints. All hint constants are defined in `include/SDL3/SDL_hints.h` with documentation comments explaining each one.

### Finding how events work
The event loop and event types are defined in:
- `include/SDL3/SDL_events.h` - event structs and types
- `src/events/SDL_events.c` - event pump implementation
- `src/events/SDL_windowevents.c` - window event dispatch
- `src/events/SDL_keyboard.c` - keyboard event handling
- `src/events/SDL_mouse.c` - mouse event handling

### Understanding the hit test callback
SDL supports custom hit testing for window dragging/resizing via `SDL_SetWindowHitTest()`. The Wayland implementation is in `src/video/wayland/SDL_waylandwindow.c`.

## Workflow

1. Clone the repo if not present
2. Search `include/SDL3/` headers for function signatures and documentation comments
3. Search `test/` and `examples/` for usage examples
4. Search `src/<subsystem>/` for implementation details when debugging
5. For platform issues, look in the relevant platform subdirectory (e.g., `src/video/wayland/`)
6. Apply findings to this project's SDL integration
