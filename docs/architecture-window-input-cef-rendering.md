# Architecture: Window, SDL, Input, CEF, and the Rendering Pipeline

This document describes how the window system, input handling, CEF browser integration, and rendering pipeline interact in jellyfin-desktop-cef. It covers the flow from hardware events to pixels on screen, the threading model, synchronization boundaries, and platform-specific divergences.

---

## Table of Contents

1. [High-Level Overview](#1-high-level-overview)
2. [SDL Window](#2-sdl-window)
3. [Input Pipeline](#3-input-pipeline)
4. [CEF Browser Integration](#4-cef-browser-integration)
5. [Rendering Pipeline](#5-rendering-pipeline)
6. [Main Loop Orchestration](#6-main-loop-orchestration)
7. [Threading Model and Synchronization](#7-threading-model-and-synchronization)
8. [Resize Flow](#8-resize-flow)
9. [Platform Divergences](#9-platform-divergences)

---

## 1. High-Level Overview

The application is a single-window desktop player built on three major subsystems:

- **SDL3** -- window creation, event pumping, platform abstraction
- **CEF (Chromium Embedded Framework)** -- off-screen rendered web UI (Jellyfin web client)
- **mpv via libmpv** -- video decoding and rendering through Vulkan/libplacebo

These subsystems are coordinated by a single main thread that owns the event loop. CEF renders its content off-screen (either to CPU buffers or GPU-shared textures); mpv renders to a separate Vulkan surface. The final composite is handled by the platform's native compositor (Wayland subsurfaces, Windows DirectComposition, macOS Core Animation) rather than a manual blit loop.

```
                    SDL3
                     |
         +-----------+-----------+
         |                       |
    Window/Events          Platform Handles
         |                  (wl_display, HWND,
         |                   NSWindow)
         |
    +----+----+
    |         |
  Input    Window Events
    |         |
    v         v
 InputStack   Resize/Focus/Fullscreen
    |              |
    v              v
 BrowserLayer   CEF browsers    mpv player
    |           (off-screen)    (Vulkan)
    v              |                |
 CefBrowser       |                |
 Send*Event    OnPaint /        render to
               OnAccelPaint    subsurface
                   |                |
                   v                v
              Compositor      VideoRenderer
              (GL/Metal)      (Vulkan swapchain)
                   |                |
                   +-------+--------+
                           |
                    Platform Compositor
                    (Wayland/DComp/CA)
                           |
                        Display
```

### Key source files

| Area | Files |
|------|-------|
| Entry point & main loop | `src/main.cpp` |
| SDL window helpers | `src/window_geometry.h`, `src/window_activation.h` |
| Input routing | `src/input/input_layer.h`, `src/input/browser_layer.h`, `src/input/menu_layer.h` |
| Window state | `src/input/window_state.h`, `src/input/mpv_layer.h` |
| Key translation | `src/input/sdl_to_vk.h` |
| CEF application | `src/cef/cef_app.h`, `src/cef/cef_app.cpp` |
| CEF client | `src/cef/cef_client.h`, `src/cef/cef_client.cpp` |
| Browser management | `src/browser/browser_stack.h`, `src/browser/browser_stack.cpp` |
| OpenGL compositor | `src/compositor/opengl_compositor.h`, `src/compositor/opengl_compositor.cpp` |
| Metal compositor | `src/compositor/metal_compositor.h`, `src/compositor/metal_compositor.mm` |
| Frame context | `src/context/frame_context.h`, `src/context/opengl_frame_context.h` |
| EGL/WGL/CGL contexts | `src/context/egl_context.h`, `src/context/wgl_context.h`, `src/context/cgl_context.h` |
| Vulkan context | `src/context/vulkan_context.h`, `src/context/vulkan_context.cpp` |
| Video renderer | `src/player/video_renderer.h`, `src/player/vulkan_subsurface_renderer.h` |
| OpenGL video renderer | `src/player/opengl_renderer.h` |
| Video render controller | `src/player/video_render_controller.h` |
| Video stack factory | `src/player/video_stack.h` |
| Wayland subsurface | `src/platform/wayland_subsurface.h`, `src/platform/wayland_subsurface.cpp` |
| X11 video layer | `src/platform/x11_video_layer.h`, `src/platform/x11_video_layer.cpp` |
| Linux event loop | `src/platform/event_loop_linux.h`, `src/platform/event_loop_linux.cpp` |
| macOS platform | `src/platform/macos_app.h`, `src/platform/macos_app.mm` |

---

## 2. SDL Window

### 2.1 Creation

The window is created in `main.cpp` with:

```
SDL_Init(SDL_INIT_VIDEO)
SDL_CreateWindow("Jellyfin Desktop CEF", width, height, flags)
```

Flags: `SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY`. On Windows, `SDL_WINDOW_HIDDEN` is also set to avoid a titlebar flash before the DWM attributes are applied.

Default dimensions are 1280x720, overridden by restored geometry from settings.

### 2.2 Hit testing

A custom hit-test callback (`windowHitTest` in `main.cpp`) enables frameless window resizing. It defines a 5-pixel border around the window edges and returns `SDL_HITTEST_RESIZE_*` for corners and edges, `SDL_HITTEST_NORMAL` for the interior. This is what allows the app to use a borderless/custom-decorated window while still supporting standard drag-resize.

### 2.3 Window state tracking

Window lifecycle events flow from SDL through a `WindowStateNotifier` (`src/input/window_state.h`) that broadcasts to registered `WindowStateListener` implementations:

```
WindowStateListener
    onMinimized()
    onRestored()
    onFocusGained()
    onFocusLost()

Listeners:
    BrowserLayer   -- focus forwarding to CEF
    MpvLayer       -- pause on minimize, resume on restore
    MediaSession   -- OS media control sync
```

The notifier is driven by SDL window events in the main loop:

| SDL Event | Notifier Call |
|-----------|---------------|
| `SDL_EVENT_WINDOW_FOCUS_GAINED` | `notifyFocusGained()` |
| `SDL_EVENT_WINDOW_FOCUS_LOST` | `notifyFocusLost()` |
| `SDL_EVENT_WINDOW_MINIMIZED` | `notifyMinimized()` |
| `SDL_EVENT_WINDOW_RESTORED` | `notifyRestored()` |

### 2.4 Fullscreen

Fullscreen state is tracked with a source discriminator:

```cpp
enum class FullscreenSource { NONE, WM, CEF };
```

This resolves conflicts between WM-initiated fullscreen (e.g., window manager keyboard shortcut) and CEF-initiated fullscreen (web content calling `requestFullscreen()`). Each source can only exit fullscreen if it was the one that entered it. On fullscreen entry, `was_maximized_before_fullscreen` is saved so the maximized state can be restored on exit.

### 2.5 Geometry persistence

`saveWindowGeometry()` and `restoreWindowGeometry()` in `src/window_geometry.cpp` persist the window position, size, and maximized state across sessions. Position restoration is skipped on Wayland (the compositor controls placement). `clampWindowToDisplay()` ensures the window fits within display bounds after monitor changes.

### 2.6 Two coordinate spaces

SDL maintains two distinct coordinate spaces:

- **Logical coordinates** -- what the user sees, used for input event positions, window size, and CEF view rect. Obtained via `SDL_GetWindowSize()`.
- **Physical coordinates** -- actual framebuffer pixels, relevant for rendering. Obtained via `SDL_GetWindowSizeInPixels()`.

The ratio `physical / logical` is the display scale factor. This ratio is passed to CEF as `device_scale_factor` in `GetScreenInfo()`, and to compositors for texture sizing. On a 2x HiDPI display, a 1280x720 logical window produces a 2560x1440 physical framebuffer.

### 2.7 Display scale changes

When `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` fires (e.g., dragging between monitors with different DPI), the window is resized to maintain the same physical pixel count. All browsers, compositors, and video surfaces are notified of the new scale.

---

## 3. Input Pipeline

### 3.1 Architecture: InputStack

Input uses a stack-based routing pattern defined in `src/input/input_layer.h`:

```
InputLayer (interface)
    handleInput(SDL_Event) -> bool   // true = consumed

InputStack
    push(InputLayer*)
    remove(InputLayer*)
    route(SDL_Event) -> bool         // iterates rbegin->rend, first true wins
```

Events are routed from the top of the stack downward. The first layer to return `true` consumes the event; remaining layers never see it. This is a priority system -- the topmost layer has first refusal.

### 3.2 Layer composition

At runtime, the stack typically contains:

```
Top  -> MenuLayer        (only present while context menu is open)
         BrowserLayer     (overlay browser -- server selection UI)
Bottom   BrowserLayer     (main browser -- Jellyfin web UI)
```

When the overlay is hidden, its `BrowserLayer` is removed and the main browser's layer becomes the top. When a context menu opens, `MenuLayer` is pushed on top, intercepting all input until the menu closes.

### 3.3 BrowserLayer: SDL to CEF translation

`BrowserLayer` (`src/input/browser_layer.h`) is the core translation layer. It implements `InputLayer` and `WindowStateListener`, and holds a pointer to an `InputReceiver` (the CEF `Client` or `OverlayClient`).

#### Mouse

| SDL Event | CEF Call | Notes |
|-----------|----------|-------|
| `SDL_EVENT_MOUSE_MOTION` | `sendMouseMove(x, y, mods)` | Coordinates in logical space. Button-held state queried via `SDL_GetMouseState()` and added as modifier flags. |
| `SDL_EVENT_MOUSE_BUTTON_DOWN` | `sendFocus(true)` then `sendMouseClick(x, y, true, btn, clickCount, mods)` | Focus sent first to ensure browser has focus before click. Multi-click detection via time/distance tracking (500ms, 5px thresholds). |
| `SDL_EVENT_MOUSE_BUTTON_UP` | `sendMouseClick(x, y, false, btn, clickCount, mods)` | Click count matches the preceding down event. |
| `SDL_EVENT_MOUSE_WHEEL` | `sendMouseWheel(x, y, deltaX, deltaY, mods)` | SDL3 smooth scroll values, scaled to CEF units (x53) in Client. |

Mouse buttons X1/X2 are mapped to `goBack()`/`goForward()` for browser navigation.

#### Keyboard

Key events flow through two stages:

**Stage 1 -- Shortcut interception.** On key-down, if the platform action modifier is held (Cmd on macOS, Ctrl elsewhere), edit shortcuts are intercepted before CEF sees them:

| Shortcut | Action |
|----------|--------|
| Cmd/Ctrl+V | Paste -- tries MIME types in order: image/png, image/jpeg, image/gif, text/html, text/plain |
| Cmd/Ctrl+C | Copy |
| Cmd/Ctrl+X | Cut |
| Cmd/Ctrl+A | Select all |
| Cmd/Ctrl+Z | Undo (or Redo if Shift held) |
| Cmd/Ctrl+Y | Redo |

These return `true` immediately, consuming the event.

**Stage 2 -- Key forwarding.** If no shortcut matched, the event is forwarded to `sendKeyEvent(key, down, mods)`.

Inside `Client::sendKeyEvent()` (`src/cef/cef_client.cpp`), the SDL keycode is translated to two values:

- `windows_key_code` via `sdlKeyToWindowsVK()` -- cross-platform, used by CEF on all platforms
- `native_key_code` via `sdlKeyToMacNative()` on macOS, or the raw SDL key elsewhere

The translation in `src/input/sdl_to_vk.h` explicitly maps punctuation keys (`comma`, `minus`, `period`, etc.) to their Windows `VK_OEM_*` codes because the raw SDL ASCII values collide with unrelated VK codes (e.g., `','` = 0x2C = `VK_SNAPSHOT`).

On macOS, `KEYEVENT_RAWKEYDOWN` is used instead of `KEYEVENT_KEYDOWN` (required for proper Chromium key handling on Mac). For Enter/Return, a supplementary `KEYEVENT_CHAR` with `'\r'` is also sent to trigger form submission.

#### Text input

`SDL_EVENT_TEXT_INPUT` delivers composed text characters. Each character is forwarded to `sendChar(ch, mods)`, which sends a `KEYEVENT_CHAR` to CEF. Control characters below 0x20 and 0x7F are filtered out because macOS generates `TEXT_INPUT` events for keys like Tab, Backspace, and Delete that are already handled by `KEY_DOWN`.

`SDL_StartTextInput(window)` is called at startup to enable IME composition.

#### Touch

Touch events (`SDL_EVENT_FINGER_*`) carry normalized 0-1 coordinates. `BrowserLayer` converts these to logical window pixels:

```
x = tfinger.x * window_width_
y = tfinger.y * window_height_
```

These are forwarded as `CefTouchEvent` with `CEF_POINTER_TYPE_TOUCH`.

#### Modifier handling

Modifiers are queried fresh on every event via `SDL_GetModState()`:

| SDL Mod | CEF Flag |
|---------|----------|
| `SDL_KMOD_SHIFT` | `EVENTFLAG_SHIFT_DOWN` |
| `SDL_KMOD_CTRL` | `EVENTFLAG_CONTROL_DOWN` |
| `SDL_KMOD_ALT` | `EVENTFLAG_ALT_DOWN` |

During mouse motion, held button state is also queried and added as `EVENTFLAG_*_MOUSE_BUTTON` flags.

### 3.4 MenuLayer: context menu interception

When a context menu is open, `MenuLayer` (`src/input/menu_layer.h`) is pushed onto the input stack. It intercepts:

- Mouse motion/clicks -> forwarded to `MenuOverlay::handleMouseMove/Click()`
- Key down -> Escape closes the menu; other keys go to `MenuOverlay::handleKeyDown()`
- All other events -> consumed (returns `true` to block passthrough)

The menu layer is removed when the menu closes, restoring normal input flow.

### 3.5 MpvLayer: video playback state

`MpvLayer` (`src/input/mpv_layer.h`) is a `WindowStateListener` (not an `InputLayer`). It pauses mpv on window minimize and resumes on restore, but only if video was playing at the time of minimize.

### 3.6 Input coordinate space

All input coordinates flow in **logical (CSS) space**. CEF expects logical coordinates and internally multiplies by `device_scale_factor` for its physical paint buffer. No coordinate scaling is done in the input path.

---

## 4. CEF Browser Integration

### 4.1 Process architecture

CEF runs as a multi-process system:

- **Browser process** -- the main application. Owns the UI thread, creates/manages browsers.
- **Renderer processes** -- child processes that run JavaScript and DOM. Communicate via IPC.
- **GPU process** -- handles hardware-accelerated operations.

On startup, `CefExecuteProcess()` is called early in `main()`. If the current process is a CEF subprocess (`--type=` flag present), it runs the subprocess logic and exits. Otherwise, the main application continues initialization.

### 4.2 External message pump

CEF is configured with `external_message_pump = true` on all platforms. This means CEF does not run its own event loop -- the application is responsible for pumping CEF work.

The pump is driven by `App::DoWork()` (`src/cef/cef_app.cpp`), which calls `CefDoMessageLoopWork()` in a loop (up to 5 iterations) to drain pending CEF work. A re-entrancy guard (`is_active_`) prevents nested calls.

CEF signals that work is pending via `OnScheduleMessagePumpWork(delay_ms)`. When `delay_ms <= 0`, the wake callback is called immediately to unblock the main loop. For delayed work, an SDL timer is scheduled.

On Windows, CEF initialization and its message loop run on a dedicated `CefThread` (separate OS thread) to avoid blocking the SDL event loop.

### 4.3 Off-screen rendering (OSR)

Both browsers are created with `CefWindowInfo::SetAsWindowless(0)` -- there is no native window backing the browser. CEF renders to an off-screen buffer and delivers frames via callbacks.

#### GetViewRect

Returns the logical size of the browser viewport. CEF multiplies this by `device_scale_factor` to determine the physical paint buffer size.

```cpp
// Client returns: CefRect(0, 0, logical_width, logical_height)
```

#### GetScreenInfo

Returns the device scale factor, computed as `physical_width / logical_width`. This tells CEF the HiDPI scaling ratio.

```cpp
// device_scale_factor = physical_w / logical_w  (default 1.0 if not available)
```

The critical ordering for resize is: `NotifyScreenInfoChanged()` **before** `WasResized()`. This ensures CEF re-queries the scale factor before computing the new paint buffer size, preventing pixel offset artifacts.

#### OnPaint (software path)

CEF delivers a complete BGRA pixel buffer at physical dimensions. For the main view (`PET_VIEW`), the buffer is written to a double-buffered `PaintBuffer` in `BrowserEntry`. For popups (`PET_POPUP`), the popup is alpha-composited onto the main view buffer.

#### OnAcceleratedPaint (GPU zero-copy path)

Platform-specific zero-copy texture sharing:

| Platform | Mechanism | Data |
|----------|-----------|------|
| Linux | dmabuf | File descriptor, stride, DRM modifier, dimensions |
| macOS | IOSurface | IOSurfaceRef, pixel format, dimensions |
| Windows | DirectComposition | D3D11 shared texture handle |

On Linux, the dmabuf fd is `dup()`'d (CEF may close its copy after the callback returns) and queued for EGL import on the GL thread. On macOS, the IOSurface is passed directly to the Metal compositor. On Windows, the shared texture handle is passed to the DComp layer.

### 4.4 Browser stack

`BrowserStack` (`src/browser/browser_stack.h`) manages multiple browsers in z-order. Currently two:

1. **Main browser** -- the Jellyfin web client
2. **Overlay browser** -- the server URL selection screen

Each browser is wrapped in a `BrowserEntry` that owns:

- The `CefClient` (either `Client` or `OverlayClient`)
- A `BrowserLayer` for input routing
- Double-buffered `PaintBuffer` arrays for software paint
- A `Compositor` instance (OpenGL or Metal, depending on platform)
- An alpha value for fade animations

The stack's `renderAll(width, height)` method performs:

1. `flushPaintBuffer()` -- copy dirty software paint data to GL/Metal texture
2. `importQueued()` -- import GPU textures (dmabuf/IOSurface)
3. `composite()` -- render each browser's texture to the framebuffer

### 4.5 Double-buffered paint

CEF's `OnPaint` callback fires on an internal CEF thread. The application uses double-buffered `PaintBuffer` arrays to avoid blocking:

- CEF writes to `paint_buffers[write_idx]`, marks it dirty, then atomically swaps the index.
- The main thread reads from `paint_buffers[1 - write_idx]` during `flushPaintBuffer()`.

This is lock-free on the read path (the main thread never blocks CEF).

### 4.6 CefClient handler interfaces

`Client` (`src/cef/cef_client.h`) implements multiple CEF handler interfaces in a single class:

```
Client : CefClient
       + CefRenderHandler       -- GetViewRect, GetScreenInfo, OnPaint, OnAcceleratedPaint, popups
       + CefLifeSpanHandler     -- OnAfterCreated, OnBeforeClose
       + CefDisplayHandler      -- OnConsoleMessage, OnCursorChange, OnFullscreenModeChange
       + CefLoadHandler         -- OnLoadEnd (sets focus after page load)
       + CefContextMenuHandler  -- RunContextMenu (delegates to MenuOverlay)
       + InputReceiver          -- sendMouseMove, sendKeyEvent, etc.
```

`OverlayClient` is a simplified variant with the same handler interfaces but no player messaging, no menu handling, and no-op goBack/goForward.

### 4.7 JavaScript bridge

The renderer process injects native APIs via `OnContextCreated()` in `src/cef/cef_app.cpp`:

- `window.jmpNative` namespace with V8 functions: `playerLoad`, `playerStop`, `playerPlay`, `playerSeek`, `playerSetVolume`, etc.
- `window.api` and `window.NativeShell` shimmed via injected JavaScript files
- Settings JSON and server URL injected as template replacements

V8 function calls in the renderer process send `CefProcessMessage` via IPC to the browser process. `Client::OnProcessMessageReceived()` dispatches these to player control, settings management, theme color changes, clipboard operations, and more.

### 4.8 CEF command-line switches

The application disables numerous Chromium services (background networking, phishing detection, translate, sync, extensions, spell-checking) and sets Google API keys to empty strings. On Linux, `ozone-platform=x11` forces X11 for CEF's internal use (required for OSR). The `app://` scheme is registered as standard, secure, local, and CORS-enabled for serving embedded resources.

---

## 5. Rendering Pipeline

### 5.1 Two independent render paths

The application does **not** composite video and UI into a single framebuffer. Instead, it maintains two independent rendering surfaces that the platform compositor layers:

```
Back layer:   Video -- Vulkan swapchain on a separate surface
Front layer:  CEF UI -- OpenGL/Metal on the main window surface
```

This layering is achieved through platform-native mechanisms:

| Platform | Video Surface | UI Surface | Compositing |
|----------|---------------|------------|-------------|
| Wayland | `wl_subsurface` (placed below parent) | Main `wl_surface` | Wayland compositor |
| X11 | Child window (lowered below parent) | Parent window | X server/window manager |
| Windows | DComp visual (background) | DComp visual (foreground) | DirectComposition |
| macOS | `CAMetalLayer` sublayer | `CAMetalLayer` main layer | Core Animation |

### 5.2 CEF rendering: OpenGL/Metal compositor

Each browser entry has a `Compositor` (`OpenGLCompositor` on Linux/Windows, `MetalCompositor` on macOS) that owns a texture representing the browser's current frame.

#### Software paint flow

```
CEF OnPaint()
    |
    v
PaintBuffer[write_idx]  (double-buffered, lock-free swap)
    |
    v  (main thread flushes)
glTexSubImage2D() -> cef_texture_
    |
    v
Fragment shader composite -> framebuffer
```

The compositor tracks texture validity: after a resize causes texture recreation, `texture_valid_` is set false until a non-black frame is received. This prevents flashing stale content.

#### Accelerated paint flow (Linux dmabuf)

```
CEF OnAcceleratedPaint()
    |
    v
Queue: {fd, stride, modifier, w, h}  (atomic flag, no mutex on queue path)
    |
    v  (main/GL thread imports)
eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, ...)
    |
    v
glEGLImageTargetTexture2DOES() -> dmabuf_texture_
    |
    v
Fragment shader composite -> framebuffer
```

The dmabuf fd is `dup()`'d in the CEF callback to prevent CEF from closing it before the GL thread imports it.

#### Fragment shader: 1:1 pixel mapping

The compositor shader uses `gl_FragCoord` for direct pixel-to-texel mapping:

```glsl
texCoord = gl_FragCoord.xy - texOffset;
// Out-of-bounds pixels are discarded (transparent)
```

This ensures CEF content is **never stretched**. If the texture is smaller than the viewport (e.g., during a resize race), the excess area is transparent -- gaps are acceptable, stretching is not. This is a hard architectural constraint.

#### Popup rendering

CEF dropdowns/selects are rendered as popups. In the software path, popup pixels are alpha-composited onto the main view buffer at the popup's CSS position (scaled to physical pixels). In the dmabuf path, popup dmabufs are imported separately and rendered at their CSS position with the scale factor applied.

### 5.3 Video rendering: Vulkan/libplacebo

#### VulkanSubsurfaceRenderer

On Wayland and macOS, video renders to a separate Vulkan swapchain on the subsurface:

```
VideoRenderController::notify()  <-- mpv redraw callback
    |
    v  (render thread wakes)
surface->startFrame()
    |  -> vkAcquireNextImageKHR() with fence
    |  -> VkFence wait
    v
mpv_render_context_render()  <-- libplacebo gpu-next backend
    |
    v
surface->submitFrame()
    |  -> vkQueuePresentKHR()
    |  -> wl_surface_commit() + wl_display_flush()
    v
Subsurface buffer is now committed to Wayland compositor
```

mpv renders directly into the swapchain images. No intermediate copy occurs.

#### OpenGLRenderer (X11 fallback)

On X11, where the video window lacks its own Vulkan surface, mpv renders to a shared-context GL FBO. The main thread composites this FBO texture to the X11 child window.

```
mpv -> shared GL context -> FBO[write_idx]
    |
    v  (main thread composites)
FBO texture -> child window draw
```

Double-buffered FBOs allow the render thread and main thread to work without contention.

### 5.4 Vulkan infrastructure

The Vulkan context (`src/context/vulkan_context.cpp`) creates:

- **Instance** with SDL3 surface extensions + `VK_EXT_swapchain_colorspace`
- **Device** with extensions: `VK_KHR_swapchain`, `VK_KHR_timeline_semaphore`, `VK_KHR_external_memory_fd`, and optionally `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`, `VK_EXT_hdr_metadata`
- **Swapchain** with `VK_FORMAT_B8G8R8A8_UNORM` (CEF surface), `VK_PRESENT_MODE_FIFO_KHR`, premultiplied alpha

The video subsurface creates its **own separate Vulkan instance and device** (not shared with the main context). This isolation prevents interference between the CEF compositor's GL context and mpv's Vulkan rendering.

For HDR video, the video swapchain prefers `VK_FORMAT_R16G16B16A16_UNORM` with `VK_COLOR_SPACE_PASS_THROUGH_EXT`, and Wayland color management (`wp_color_manager_v1`) applies BT.2020 primaries with PQ transfer function.

### 5.5 Frame context abstraction

The `FrameContext` interface (`src/context/frame_context.h`) abstracts frame begin/end:

```cpp
class FrameContext {
    virtual void beginFrame(float bg_color, float alpha) = 0;  // clear
    virtual void endFrame() = 0;                                // swap
};
```

`OpenGLFrameContext` implements this with glClear + EGL/WGL swap. This is used on Linux (EGL) and Windows (WGL) for the CEF UI surface.

---

## 6. Main Loop Orchestration

The main loop in `main.cpp` coordinates all subsystems in a single-threaded event pump:

```
while (running && !client->isClosed()) {
    1. Drain mpv events (from MpvEventThread queue)
    2. Check deferred signal flag (SIGINT/SIGTERM -> SDL_EVENT_QUIT)
    3. Poll/wait for SDL events
    4. Process all SDL events:
       - Input events -> input_stack.route(event)
       - Window events -> resize, focus, fullscreen, scale changes
    5. Drain pending player commands (from CEF IPC queue)
    6. Update overlay state machine
    7. Pump CEF: App::DoWork()
    8. Decide if rendering is needed
    9. Render: import textures, composite browsers, present
   10. Log slow frames (>50ms during video)
}
```

### Event polling strategy

The polling strategy adapts to the application's activity state:

| State | Strategy | Rationale |
|-------|----------|-----------|
| Active (rendering, video, pending content) | `SDL_PollEvent()` | Non-blocking, allows immediate rendering |
| Idle with overlay fade | `SDL_WaitEventTimeout(fade_remaining)` | Wake when fade timer expires |
| Fully idle | `SDL_WaitEvent()` / `eventLoopWake.waitForEvent()` | Block until something happens |

On Linux, the idle path uses `EventLoopWake` (`src/platform/event_loop_linux.cpp`) which combines an `eventfd` with `poll()` on both the display fd and the wake fd. This allows CEF's `OnScheduleMessagePumpWork` to reliably wake the main loop from any thread -- something `SDL_WaitEvent()` alone cannot guarantee because it only monitors the display fd.

### Render decision

A frame is rendered if any of:

- User input occurred this frame (`activity_this_frame`)
- Video is playing (`has_video`)
- Any browser has pending painted content (`browsers.anyHasPendingContent()`)
- Overlay fade animation is in progress

When none of these conditions are true, the loop blocks in `SDL_WaitEvent()`, consuming zero CPU.

---

## 7. Threading Model and Synchronization

### Thread inventory

| Thread | Owner | Purpose | Lifetime |
|--------|-------|---------|----------|
| Main thread | SDL | Event loop, CEF pump, rendering | Process lifetime |
| CEF internal threads | CEF | Browser internals, IPC | CEF init to shutdown |
| CefThread (Windows only) | App | CefRunMessageLoop wrapper | CEF init to shutdown |
| MpvEventThread | App | Drain mpv event queue | Player init to shutdown |
| VideoRenderController | App | Vulkan video rendering | Player init to shutdown |
| MediaSession thread | App | OS media controls | Player init to shutdown |

### Synchronization boundaries

**CEF -> Main thread (paint callbacks):**
Double-buffered `PaintBuffer` with atomic index swap. No mutex on the read path. For dmabuf/IOSurface, an atomic flag guards the queue.

**CEF -> Main thread (player commands):**
`cmd_mutex` protects the `pending_cmds` vector. CEF's `OnProcessMessageReceived` pushes; main loop drains.

**mpv -> Main thread (events):**
`MpvEventThread` uses internal mutex + condition variable. Main thread calls `drain()` which swaps the buffer atomically.

**Main thread -> Video render thread (resize, notify):**
Atomic flags (`resize_pending_`, `frame_notified_`, `active_`, etc.) and a condition variable. The main thread stores new dimensions atomically; the render thread reads them before rendering.

**CEF wake -> Main loop:**
`OnScheduleMessagePumpWork` calls a wake callback. On Linux, this writes to an eventfd. On macOS/Windows, it pushes a custom SDL event (`SDL_EVENT_WAKE`).

### Lock-free patterns

The codebase avoids locks on hot paths:

- Paint buffer swap: atomic index, no mutex on read
- Dmabuf queue: atomic flag, fd/stride/modifier written before flag set
- Frame notification: atomic bool + condition variable (CV only used for thread sleep)
- Video ready state: atomic bool, no mutex

---

## 8. Resize Flow

Resize is the most complex cross-cutting operation because it touches every subsystem. Here is the complete flow:

```
SDL_EVENT_WINDOW_RESIZED
    |
    v
main.cpp event handler
    |
    +---> Update logical width/height from event.window.data1/2
    |
    +---> Get physical dimensions: SDL_GetWindowSizeInPixels()
    |
    +---> browsers.resizeAll(logical_w, logical_h, physical_w, physical_h)
    |         |
    |         +---> For each BrowserEntry:
    |                   Client::resize(logical_w, logical_h, physical_w, physical_h)
    |                       |
    |                       +---> Store new dimensions
    |                       +---> NotifyScreenInfoChanged()  (update scale factor FIRST)
    |                       +---> WasResized()               (recalculate paint buffer)
    |                       +---> Invalidate(PET_VIEW)       (request repaint)
    |                   |
    |                   BrowserLayer::setWindowSize(logical_w, logical_h)
    |                   |
    |                   Compositor::resize(physical_w, physical_h)
    |
    +---> videoController.requestResize(physical_w, physical_h)
    |         |
    |         +---> If active: store dimensions, set resize_pending_ flag, wake render thread
    |         +---> If inactive: set resized_while_inactive_ flag (applied on reactivation)
    |                   |
    |                   v  (render thread)
    |               renderer->resize(width, height)
    |                   |
    |                   v
    |               Vulkan: vkDeviceWaitIdle() -> destroy views -> recreate swapchain -> create views
    |               Wayland: wp_viewport_set_destination() for HiDPI logical size
    |
    +---> frameContext.resize(physical_w, physical_h)
    |
    +---> Set paint_size_matched = false (invalidate until new frame arrives at correct size)
```

### Key ordering constraints

1. `NotifyScreenInfoChanged()` **must** precede `WasResized()` so CEF caches the correct scale factor before computing the new paint buffer dimensions.
2. Vulkan swapchain recreation uses the old swapchain as `VkSwapchainCreateInfoKHR::oldSwapchain` for smooth transition.
3. `paint_size_matched` remains false until a CEF frame arrives at the new physical dimensions, preventing presentation of stale/mismatched content.

### Live resize (platform-specific)

**macOS:** An SDL event watcher callback processes resize events during the Cocoa modal drag loop. It renders every frame with CA transaction synchronization for fluid live resize.

**Windows:** An SDL event watcher handles resize during the Win32 modal resize loop, resizing DComp layers and the WGL context.

**Linux (Wayland):** Resize is non-modal. The render thread processes resize requests asynchronously via condition variable notification.

---

## 9. Platform Divergences

### 9.1 macOS

- **Graphics:** Metal compositor, CAMetalLayer, MoltenVK for Vulkan
- **CEF mode:** Single-process (`--single-process` flag)
- **Titlebar:** Transparent titlebar with custom `TitlebarDragView` overlay for window dragging. Traffic light (close/minimize/zoom) visibility toggled by web content.
- **Key events:** `KEYEVENT_RAWKEYDOWN` instead of `KEYEVENT_KEYDOWN`. Native key codes use Carbon `kVK_*` values.
- **Window activation:** Deferred until first `SDL_EVENT_WINDOW_EXPOSED` via `activateMacWindow()`. Uses `[NSApp activateIgnoringOtherApps:YES]`.
- **Video render thread:** Not used -- video renders synchronously on the main thread.

### 9.2 Windows

- **Graphics:** WGL for OpenGL, DirectComposition for zero-copy CEF+video layering
- **CEF mode:** Dedicated `CefThread` with `CefRunMessageLoop()`
- **DWM:** Dark mode and background color (#101010) set via `DwmSetWindowAttribute()`
- **Window visibility:** Hidden at creation (`SDL_WINDOW_HIDDEN`), shown after DWM attributes applied
- **Job object:** `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` ensures CEF subprocesses die with parent
- **Shutdown:** `_exit(0)` force-exit to avoid CEF/D3D/Vulkan destructor hangs

### 9.3 Linux

- **Graphics:** EGL for OpenGL, Vulkan for video subsurface
- **CEF mode:** External message pump on main thread
- **Display servers:**
  - **Wayland:** Subsurface for video layer, `wp_viewporter` for HiDPI, `xdg_activation_v1` for window raise, `wp_color_manager_v1` for HDR
  - **X11:** Child window for video layer, `XLowerWindow()` for z-order
- **Event loop:** `EventLoopWake` with `eventfd` + `poll()` for reliable cross-thread wakeup
- **GPU acceleration:** dmabuf zero-copy for CEF, with runtime probe for driver support. Falls back to software paint if dmabuf is unavailable.
- **CEF Ozone:** Forced to X11 (`--ozone-platform=x11`) for OSR compatibility

### 9.4 Feature matrix

| Feature | macOS | Windows | Linux (Wayland) | Linux (X11) |
|---------|-------|---------|-----------------|-------------|
| CEF texture path | IOSurface | DComp shared | dmabuf | dmabuf or software |
| Video surface | CAMetalLayer | DComp visual | wl_subsurface | Child window |
| Vulkan renderer | MoltenVK | Native Vulkan | Native Vulkan | Native Vulkan |
| UI compositor | Metal | OpenGL (WGL) | OpenGL (EGL) | OpenGL (EGL) |
| Video render thread | No (sync) | Yes | Yes | Sync (GL context not shareable) |
| HDR video | No | No | Wayland color mgmt | No |
| Transparent titlebar | Yes | No | No | No |
| Live resize | CA transaction | DComp layer resize | Async | Async |
