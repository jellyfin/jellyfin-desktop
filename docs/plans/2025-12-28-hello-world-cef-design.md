# Hello World CEF Application Design

## Overview

Minimal CEF application with SDL2 windowing and OpenGL rendering. Foundation for a Jellyfin desktop client with mpv video playback and web-based OSD overlay.

## Target Platforms

- Linux
- Windows
- macOS

## Technology Stack

| Component | Choice | Rationale |
|-----------|--------|-----------|
| Language | C++ | CEF's native language, best documented |
| Build | CMake | CEF's official build system |
| Windowing | SDL2 | Gamepad support, GL context control, libmpv compatible |
| Rendering | OpenGL (Vulkan later) | OSR compositing, texture-based CEF output |
| Browser | CEF (OSR mode) | Off-screen rendering to texture |

## Architecture

```
┌─────────────────────────┐
│   CEF (transparent OSD) │  ← Web UI overlay (future)
├─────────────────────────┤
│   mpv (video layer)     │  ← OpenGL/Vulkan rendering (future)
├─────────────────────────┤
│   SDL2 window           │  ← Input + window management
└─────────────────────────┘
```

MVP implements SDL2 + CEF OSR + OpenGL compositor. mpv integration comes later.

## Project Structure

```
jellyfin-desktop/
├── CMakeLists.txt
├── cmake/
│   └── FindCEF.cmake
├── src/
│   ├── main.cpp           # Entry point, SDL2 init, main loop
│   ├── app.h / app.cpp    # CefApp implementation
│   ├── client.h / client.cpp  # CefClient, CefRenderHandler (OSR)
│   └── renderer.h / renderer.cpp  # OpenGL texture rendering
├── resources/
│   └── index.html         # Hello world page
├── docs/
│   └── plans/
└── third_party/
    └── cef/               # CEF binary distribution (gitignored)
```

## CEF Acquisition

Manual download from [Spotify CEF Builds](https://cef-builds.spotifycdn.com/index.html) - the official distribution. Extract to `third_party/cef/`.

## MVP Scope

1. SDL2 window creation with OpenGL context
2. CEF initialization in off-screen rendering mode
3. Load `resources/index.html` displaying "Hello World"
4. CEF renders to texture via `OnPaint` callback
5. OpenGL renders texture to fullscreen quad
6. Clean shutdown of CEF and SDL2

**Not in MVP:**
- Mouse/keyboard input forwarding
- Gamepad input
- mpv integration
- Transparency/compositing
- Wayland subsurface path

## Key CEF Classes

- `CefApp` - Application-level callbacks, process startup
- `CefClient` - Browser instance callbacks
- `CefRenderHandler` - OSR callbacks (`GetViewRect`, `OnPaint`)
- `CefBrowserHost::CreateBrowser` - Creates browser with OSR enabled

## Render Loop

```
while running:
    SDL_PollEvent()           # Handle window events
    CefDoMessageLoopWork()    # Pump CEF messages

    if cef_texture_dirty:
        glTexSubImage2D()     # Upload CEF paint buffer

    glClear()
    draw_textured_quad()      # Render CEF texture
    SDL_GL_SwapWindow()
```

## Future Considerations

- **libmpv integration** - Render video to separate texture, composite under CEF
- **Vulkan migration** - SDL2 supports Vulkan surfaces, swap GL calls for Vulkan
- **Wayland subsurface** - Alternative to OSR compositing on Wayland
- **Gamepad input** - SDL2 controller API, translate to CEF key events or custom JS API
