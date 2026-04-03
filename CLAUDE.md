# Project Notes

## Constraints
- **No hand-rolled JSON** — never manually construct or parse JSON with string concatenation, manual escaping, or homebrew parsers. Always use a proper JSON library or API (e.g. CEF's `CefParseJSON`/`CefWriteJSON`, or a vendored library if CEF isn't available in that context).
- **No artificial heartbeats/polling** - event-driven architecture only. Never use timeouts as a workaround for proper event integration. No arbitrary timeout-based bailouts in shutdown paths either — fix the root cause instead.
- **No texture stretching during resize** - CEF content must always render at 1:1 pixel mapping. Never scale/stretch textures to fill the viewport. Gaps from stale texture sizes are acceptable; stretching is not.

## Build
```
cmake --build build
```

## Architecture
- **SDL3** — cross-platform window management, input events, and the main window surface that CEF and video layers composite into
- **CEF** (Chromium Embedded Framework) — hosts jellyfin-web as an embedded browser; handles JS-to-C++ IPC for player control commands and renders the UI as an overlay texture above the video layer. Multi-process: browser process (main app, owns CefBrowser), renderer process (V8/Blink), GPU process. IPC via `CefProcessMessage`.
- **mpv** via libmpv — video playback; the desktop client injects native shims to override browser media playback with libmpv
- **libplacebo** — Vulkan rendering backend for mpv's gpu-next renderer; provides color-managed video output
- Wayland subsurface for video layer

## mpv Integration
- Custom libmpv gpu-next path in `third_party/mpv/video/out/gpu_next/`
- `video.c` - main rendering, uses `map_scaler()` for proper filtering
- `context.c` - Vulkan FBO wrapping for libmpv
- `libmpv_gpu_next.c` - render backend glue
- **Never call sync mpv API (`mpv_get_property`, etc.) from event callbacks** - causes deadlock during video init. Use property observation or async variants instead.

## mpv Event Flow
mpv is the authoritative source of playback state. All state (position, speed, pause, seeking, etc.) flows from mpv property observations outward to the JS UI and OS media sessions. The JS UI and MPRIS/macOS media sessions are consumers — they never determine playback state, they only reflect what mpv reports. This means things like rate changes, seek completion, and position updates come from mpv, not from JS round-trips or manual bookkeeping.

## Debugging
- For mpv (third_party/mpv), jellyfin-web, CEF, SDL, and libplacebo: investigate source code directly before suggesting debug logs that require manual user action
