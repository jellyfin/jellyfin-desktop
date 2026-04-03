# Project Notes

## Constraints
- **No hand-rolled JSON** — never manually construct or parse JSON with string concatenation, manual escaping, or homebrew parsers. Always use a proper JSON library or API (e.g. CEF's `CefParseJSON`/`CefWriteJSON`, or a vendored library if CEF isn't available in that context).
- **No artificial heartbeats/polling** - event-driven architecture only. Never use timeouts as a workaround for proper event integration. No arbitrary timeout-based bailouts in shutdown paths either — fix the root cause instead. Note: per-frame `wl_display_dispatch_queue_pending` is *not* polling — it's the standard Wayland event drain pattern (same as standalone mpv's `vo_wayland_dispatch_events`). Don't confuse non-blocking queue drains with artificial polling.
- **No debouncing** — debouncing is a hack that masks the real problem. If events fire too many times, fix the event dispatch (e.g. use a separate event queue like standalone mpv does), don't paper over it with timers or flags.
- **No texture stretching during resize** - CEF content must always render at 1:1 pixel mapping. Never scale/stretch textures to fill the viewport. Gaps from stale texture sizes are acceptable; stretching is not.
- **No hand-rolled math that libraries already provide** — never reimplement luminance scaling, color space conversions, or protocol value encoding that libplacebo or mpv already compute. Delegate to mpv or libplacebo code rather than duplicating logic. If you're finding yourself duplicating logic, there's likely something from mpv/libplacebo that could be used instead. Rounding errors from DIY math are unacceptable.
- **HDR Wayland protocol values must match standalone mpv** — the `wp_color_management` / `wp_image_description` protocol messages our app sends (luminances, mastering metadata, CLL, primaries) must be identical to what standalone mpv produces for the same content on the same display, as verified by `WAYLAND_DEBUG=1` comparison. Use the standalone player mode (`--player`) to validate. No "close enough" — exact match.

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

## Test Harnesses

Test harnesses in `dev/` run the video stack (SDL window, EGL, Vulkan subsurface, mpv, libplacebo) with scripted scenarios for autonomous reproduction of bugs. They don't include CEF — they isolate the video layer for issues where CEF interaction isn't the variable. For full-stack verification (CEF compositing + video), test with the actual app.

### When to write a test harness

When the user reports a visual issue (wrong size, color, position, timing) that you can't diagnose from code reading alone, **write a harness that reproduces it**. Don't guess at fixes — prove the bug exists first, then prove the fix works.

### How to write one

1. **Copy the pattern** from the standalone player mode in `src/main.cpp` (`--player` flag) — it has the canonical setup: SDL window → EGL → VideoStack::create → VideoRenderController (threaded) → event loop with mpv.
2. **Script the scenario**: use timers and SDL APIs to trigger the condition (e.g. `SDL_SetWindowFullscreen` after 3 seconds, resize to specific dimensions, toggle visibility).
3. **Add targeted logging** at the boundaries you suspect: log dimensions at SDL level, at the renderer, at the surface. Compare what each layer thinks the size/state is.
4. **Add the CMake target** following the standalone player pattern in `CMakeLists.txt` (same source list, just swap the test `.cpp`).
5. **Run it, read the logs, find the mismatch**. The bug is wherever two layers disagree about a value.

### Existing harnesses

- Standalone player mode (`--player`) — runs the full video stack without CEF, useful for HDR color management validation against standalone mpv via `WAYLAND_DEBUG=1` comparison.
- `dev/linux/color-mgmt-compare.sh` — automated comparison of Wayland `wp_image_description` parameters between `jellyfin-desktop --player` and `third_party/mpv/build/mpv`. Toggles EDR on/off via `kscreen-doctor` and tests both modes. Usage: `./dev/linux/color-mgmt-compare.sh --hdr test_hdr.mkv --sdr test_sdr.mkv`

### Limitations

These harnesses don't include CEF. The transparent EGL surface stands in for where CEF would render, but the CEF compositor, browser, and paint callbacks are absent. This means they can't catch issues involving CEF/video layer interaction (z-ordering, input passthrough, compositing alpha). For those, use the real app.

### Tips

- The harnesses share the same video stack code as the real app — a fix verified in a harness is verified in the app.
- Temporary debug logging in the real source files is fine during investigation — just clean it up before committing.
- For protocol-level debugging, combine with `WAYLAND_DEBUG=1` (stderr capture) to see actual Wayland messages.
