# Project Notes

## Constraints
- **No hand-rolled JSON** — never manually construct or parse JSON with string concatenation, manual escaping, or homebrew parsers. Always use a proper JSON library or API (e.g. CEF's `CefParseJSON`/`CefWriteJSON`, or a vendored library if CEF isn't available in that context).
- **No artificial heartbeats/polling** - event-driven architecture only. Never use timeouts as a workaround for proper event integration. No arbitrary timeout-based bailouts in shutdown paths either — fix the root cause instead.
- **No texture stretching during resize** - CEF content must always render at 1:1 pixel mapping. Never scale/stretch textures to fill the viewport. Gaps from stale texture sizes are acceptable; stretching is not.
- **No single-process mode on macOS** — `--single-process` causes Mojo IPC sources to fire on the main thread's CFRunLoop, making ANY CFRunLoop-based blocking wait (including `SDL_WaitEvent`) spin at 100-166% CPU. Pipe-based workarounds avoid CFRunLoop but miss Cocoa user input events. The fix is multi-process mode (same as Linux), where renderer runs in a separate process and `SDL_WaitEvent` idles correctly.

## Build
```
cmake --build build
```

## Architecture
- CEF (Chromium Embedded Framework) for web UI
- mpv via libmpv for video playback
- Vulkan rendering with libplacebo (gpu-next backend)
- Wayland subsurface for video layer

## mpv Integration
- Custom libmpv gpu-next path in `third_party/mpv/video/out/gpu_next/`
- `video.c` - main rendering, uses `map_scaler()` for proper filtering
- `context.c` - Vulkan FBO wrapping for libmpv
- `libmpv_gpu_next.c` - render backend glue
- **Never call sync mpv API (`mpv_get_property`, etc.) from event callbacks** - causes deadlock during video init. Use property observation or async variants instead.

## Debugging
- For mpv (third_party/mpv) and external Jellyfin repos (jellyfin-web, jellyfin-desktop): investigate source code directly before suggesting debug logs that require manual user action
