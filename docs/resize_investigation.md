# Wayland Resize Gap Investigation

## Problem

When dragging the bottom window edge to resize, gaps flash on both sides (left and right) of the window. The CEF layer appears to shrink rapidly during resize. The CEF content is not anchored to the top — instead it appears to float or jump, creating visible gaps or misalignment.

- Scale: 100% (1x)
- Platform: Arch Linux, Wayland
- Gaps appear as vertical strips of background on left and right edges

## Hard Constraints

- **No modifications to SDL source** (third_party/sdl_src/) - explicitly forbidden
- **No diagnostic logging** - user rejected this approach
- **No texture stretching** - CEF content must render at 1:1 pixel mapping (per CLAUDE.md)
- **No artificial heartbeats/polling** - event-driven architecture only

## Minimal Repro

Reproduced in a minimal test binary (`tests/resize_gap_repro.cpp`) containing only SDL3 + app-managed EGL + CEF OSR compositing — no video, no mpv, no complex app logic. A version with NO CEF (just `glClear` with a solid color) resizes smoothly with no gaps. The problem is specifically in how the stale CEF texture interacts with the viewport/surface during resize.

### Build & Run

```bash
cmake --build build --target resize_gap_repro
./build/resize_gap_repro
```

Drag the bottom edge to resize. Observe:
- Orange = CEF content (good)
- Dark blue = glClear background (gap — means CEF texture didn't cover that area)

## Architecture of the Repro

The test (`tests/resize_gap_repro.cpp`) replicates the main app's exact rendering setup:

1. **SDL window**: `SDL_CreateWindow` with `SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY`, no `SDL_WINDOW_OPENGL`
2. **App-managed EGL**: `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wl_display)` → `wl_egl_window_create` → `eglCreateWindowSurface`
3. **CEF OSR**: Offscreen browser renders BGRA via `OnPaint` callback → uploaded to GL texture via `glTexSubImage2D`
4. **Compositor shader**: Full-screen triangle, `texelFetch` with manual Y-flip, BGRA swizzle, bounds check (discards pixels outside texture)
5. **Viewport protocol**: `wp_viewport_set_destination(logical_w, logical_h)` called before `eglSwapBuffers`

### Render loop (every frame)

```
SDL_PollEvent → handle SDL_EVENT_WINDOW_RESIZED:
    wl_egl_window_resize(egl_window, physical_w, physical_h, 0, 0)
    client->resize(logical_w, logical_h, physical_w, physical_h)

CefDoMessageLoopWork()
Upload CEF paint buffer to texture if new data available

SDL_GetWindowSizeInPixels → pixel_w, pixel_h
glViewport(0, 0, pixel_w, pixel_h)
glClear(dark blue)
Draw fullscreen triangle with CEF texture (shader uses texelFetch)
wp_viewport_set_destination(sdl_viewport, logical_w, logical_h)
eglSwapBuffers(egl_display, egl_surface)
```

### Resize handler

On `SDL_EVENT_WINDOW_RESIZED`:
- `logical_w/h` = event data (compositor logical coords)
- `pixel_w/h` = `SDL_GetWindowSizeInPixels` (physical pixels for EGL/GL)
- `wl_egl_window_resize(egl_window, pixel_w, pixel_h, 0, 0)` — resize EGL backing buffer
- `client->resize(logical_w, logical_h, pixel_w, pixel_h)` — tells CEF: `NotifyScreenInfoChanged` → `WasResized` → `Invalidate(PET_VIEW)`

### Fragment shader (the compositor)

```glsl
int px = int(gl_FragCoord.x);
int tex_y = int(viewSize.y) - 1 - int(gl_FragCoord.y);  // Y-flip
if (px < 0 || tex_y < 0 || px >= int(texSize.x) || tex_y >= int(texSize.y))
    discard;  // Outside texture bounds → shows glClear color
vec4 color = texelFetch(overlayTex, ivec2(px, tex_y), 0);
```

This means: if the viewport is 1280x800 but the CEF texture is still 1280x720 (stale), the bottom 80px discard → dark blue gap. The texture is anchored to **top-left** via the Y-flip formula. So top/left should always be correct, and gaps should only appear at bottom/right.

## CEF-to-Window Size Interaction (full analysis)

There are **three independent size domains** that interact during resize:

| Size | Source | Updated when | Used for |
|------|--------|-------------|----------|
| **viewSize** (glViewport / shader uniform) | `SDL_GetWindowSizeInPixels()` each frame (main.cpp:1626-1628) | Immediately after SDL processes configure | Y-flip reference in shader, glViewport |
| **texSize** (shader uniform) | `cef_texture_width_` / `cef_texture_height_` in compositor | When CEF paints AND flush delivers data via `updateOverlayPartial()` | Bounds check in shader |
| **EGL framebuffer** | Actual back buffer allocated by Mesa EGL | After `wl_egl_window_resize()` + next `eglSwapBuffers()` | Physical pixel area fragments are generated for |

### The one-frame EGL buffer lag

`wl_egl_window_resize()` (called from `egl.resize()` at main.cpp:1333) sets the *pending* size. The actual back buffer stays at the old size until the next `eglSwapBuffers()`. This means for one frame after every resize:

- `SDL_GetWindowSizeInPixels()` → **new** size (SDL already updated)
- `glViewport(0, 0, new_w, new_h)` → set to **new** size
- Actual EGL framebuffer → still **old** size
- Fragments only generated for pixels within the **old** framebuffer bounds

### Y-flip offset bug during EGL buffer lag

The shader Y-flip: `tex_y = viewSize.y - 1 - gl_FragCoord.y`

When viewSize.y (from SDL) is LARGER than the actual EGL framebuffer height:
- `gl_FragCoord.y` ranges from `0` to `old_h - 1` (clamped by framebuffer)
- `tex_y` ranges from `viewSize.y - 1` down to `viewSize.y - old_h`
- Instead of sampling texture rows `0..old_h-1`, it samples `(viewSize.y - old_h)..viewSize.y-1`
- **The content is sampled from the WRONG rows** — offset by `(viewSize.y - old_h)` pixels

Example: old=720, new=820 → tex_y ranges 100..819 instead of 0..719.
The top 100 rows of the CEF texture are never shown; rows 720-819 are discarded by bounds check.

**This causes vertical mis-rendering but does NOT explain horizontal (left+right) gaps for a height-only resize.**

### Resize order of operations

On `SDL_EVENT_WINDOW_RESIZED` (main.cpp:1306-1343):
1. `current_width/height` ← logical from event (line 1307-1308)
2. `physical_w/h` ← `SDL_GetWindowSizeInPixels()` (line 1311-1312)
3. `paint_size_matched = false` if compositor size changed (line 1317-1320)
4. `browsers.resizeAll(logical, physical)` → compositor.resize() + CEF WasResized() + Invalidate() (line 1323)
5. `egl.resize(physical)` → `wl_egl_window_resize()` (line 1333)

Then on next render frame (main.cpp:1626-1637):
1. `SDL_GetWindowSizeInPixels()` → viewport_w/h (NEW size)
2. `glViewport(0, 0, viewport_w, viewport_h)` (NEW size, but framebuffer still OLD)
3. `browsers.renderAll(viewport_w, viewport_h)` which calls:
   - `flushPaintBuffer()` → `updateOverlayPartial()` (CEF texture may still be old size)
   - `composite(viewport_w, viewport_h)` → viewSize=NEW, texSize=whatever CEF painted

### CEF paint is asynchronous

CEF paints on its own thread. After `WasResized()` + `Invalidate()`:
1. CEF queries `GetViewRect()` → returns logical size (cef_client.cpp:369-377)
2. CEF queries `GetScreenInfo()` → computes `device_scale_factor = physical / logical` (cef_client.cpp:379-395)
3. CEF renders at `logical * scale = physical` size
4. `OnPaint()` delivers buffer at physical dimensions (cef_client.cpp:408-472)
5. Paint callback stores in double buffer (browser_stack.cpp:49-74)
6. Next frame's `flushPaintBuffer()` picks it up

Between steps 1-5, the main thread may have already rendered one or more frames with mismatched sizes.

The `paint_size_matched` guard (main.cpp:1652-1657) re-requests paint when CEF delivers at stale size, but this still allows multiple frames of mismatch.

### What the shader does with mismatched sizes

Given `viewSize`, `texSize`, and actual framebuffer:
- Pixels where `px >= texSize.x` → **discarded** (gap on RIGHT)
- Pixels where `tex_y >= texSize.y` → **discarded** (gap on BOTTOM, visually)
- Pixels where `tex_y < 0` → **discarded** (gap on TOP, visually)
- Pixels beyond framebuffer bounds → **never generated** (OpenGL clips)

For a pure height increase (bottom-edge drag), width should stay constant.
**texSize.x == viewSize.x → no horizontal gap from the shader.**

This means the horizontal gaps during vertical resize are NOT caused by the shader's size handling. The cause must be elsewhere.

## Root Cause: CEF delivers partially-rendered frames during resize

### Discovery process

Systematic shader isolation narrowed it down:

| Test | Texture access | Result |
|------|---------------|--------|
| Checkerboard (uniform pattern, no texture) | None | **No gaps** |
| Two-region orange/blue (same boundary logic, no texture) | None | **No gaps** |
| texelFetch but output computed color | Read, ignore result | **No gaps** |
| texelFetch with actual texture content | Read, use result | **Gaps** |
| Alpha visualization (RED = alpha=0 pixels) | Read, check alpha | **Red gaps** |

The gaps are caused by **transparent pixels in the CEF texture itself**.

### Mechanism

During resize, CEF re-renders the page at the new size. Before all tiles are complete, CEF delivers an `OnPaint` frame where:
- Some pixels are fully rendered (opaque page content)
- Edge/corner pixels are still at their initial value: `(0,0,0,0)` (transparent black)

These transparent pixels, when composited with premultiplied alpha blending (`GL_ONE, GL_ONE_MINUS_SRC_ALPHA`), show the `glClear` background through — creating the visible "gaps."

Key observations:
- The partial paints arrive at the **same size** as the current texture (`size_changed=0`), not just during size transitions
- Up to **2754 edge pixels** can be transparent in a single partial paint
- Width is always correct (1280) — the "side gaps" are transparent pixels within the texture, not a size mismatch

### Validated fix (repro only)

In the repro (`tests/resize_gap_repro.cpp`), rejecting CEF paints with transparent edge pixels completely eliminates resize gaps. The repro scans the first/last row and first/last column for alpha=0; if found, the paint is discarded and the existing texture is kept.

## Eliminated Hypotheses

| Hypothesis | Test | Result |
|-----------|------|--------|
| EGL buffer misplaced by compositor | Green/red scissor test | **Eliminated** — buffer is correctly anchored |
| App-managed EGL conflicts with SDL viewport | SDL_WINDOW_OPENGL test | **Eliminated** — same gaps with SDL managing everything |
| Vsync race condition | eglSwapInterval(0) | **Eliminated** — no change |
| Back buffer lag after wl_egl_window_resize | Deferred resize | **Eliminated** — no change |
| discard + blend leaving undefined alpha | Explicit output replacement | **Eliminated** — no change |
| SDL modifying viewport asynchronously | Multiple viewport override approaches | **Eliminated** — SDL_WINDOW_OPENGL has same issue |
| Wayland compositor centering | Protocol tracing | **Eliminated** — buffer anchoring is correct |
| wp_viewport_set_destination race | Removed/overrode viewport calls | Changes pattern but not root cause |

## Failed Approaches

### 1. Diagnostic logging (REJECTED)
Adding LOG_DEBUG/LOG_WARN to main.cpp, opengl_compositor.cpp, egl_context.cpp. User explicitly rejected this: "stop with the log nonsense."

### 2. SDL_AddEventWatch for live resize render (FAILED - no effect)
**Hypothesis:** SDL3's Wayland backend updates wp_viewport_set_destination and commits the surface (via libdecor) before the app renders a new buffer. The compositor scales the stale buffer to the new viewport, causing shrinkage/gaps.

**Implementation:** Added a `linuxResizeCallback` event watch that intercepts `SDL_EVENT_WINDOW_RESIZED` during SDL's internal event dispatch and immediately resizes EGL surface, browser compositors, video layer, and renders a full frame.

**Result:** Zero difference. The gaps still appear.

**Why it likely failed:** The event watch fires when SDL pushes the event, but the actual wl_surface_commit from libdecor may happen at a different point in SDL's internal flow.

### 3. Viewport override event watch + buffer size tracking (FAILED - no effect)
**Hypothesis:** The wp_viewport destination size set by SDL causes the compositor to stretch the stale EGL buffer to the new window size.

**Implementation:** Override the viewport destination to match the actual EGL buffer size (not the requested window size).

**Result:** Zero difference. The gaps still appear.

### 4. Patching SDL Wayland backend (FORBIDDEN)
User explicitly stated SDL source must not be modified.

## Unsolved: Applying the fix to the real app

The edge-rejection approach breaks video playback. The OSD (transport controls) legitimately has transparent edges for compositing over the video layer. Rejecting transparent-edge paints prevents the OSD from ever appearing.

Attempted solutions that failed:
1. **Track opaque vs transparent mode** (`last_paint_opaque_edges_`): Arm the filter only when content was opaque before resize. Failed because the Jellyfin video player UI can have opaque corners while the rest of the edge is transparent.
2. **Fixed skip count after resize**: Skip N paints unconditionally after each resize. Failed because resize events fire continuously during drag.
3. **Skip only when growing**: Failed because shrinking also produces partial tiles.
4. **Edge scan + counter limit**: During continuous drag, the counter resets on each resize event.

## Possible approaches not yet tried

- **CEF `background_color`**: Set `CefBrowserSettings::background_color` to an opaque color so unrasterized tiles are filled with that color instead of transparent. Would eliminate transparent pixels at the source. Downside: the CEF layer becomes opaque, which may break video compositing.
- **Merge paints**: Only overwrite pixels that are opaque in the new paint; keep old texture data for transparent pixels. Requires per-pixel GPU upload logic (e.g., shader-based merge via FBO).
- **CEF shared texture / accelerated compositing**: Use CEF's GPU-accelerated rendering path instead of software OSR. May avoid the partial-tile issue entirely.
- **Chromium `--disable-partial-raster`**: May force CEF to deliver complete frames only. Untested.
- **Wait for activation**: Chromium's cc system has a pending→active tree gate. Investigate whether CEF OSR bypasses this and whether it can be configured to wait for full rasterization before calling OnPaint.

## Key Files

| File | Role |
|------|------|
| `tests/resize_gap_repro.cpp` | **Minimal repro** — edit this to test hypotheses |
| `CMakeLists.txt` | Build target `resize_gap_repro` (bottom of file) |
| `src/context/egl_context.cpp` | Main app's EGL init and resize |
| `src/main.cpp:1306-1342` | Main app's resize event handler |
| `src/main.cpp:1622-1646` | Main app's Linux render path |
| `src/compositor/opengl_compositor.cpp` | Main app's CEF texture compositing |

## Important details

- The CEF texture is vertically flipped (origin at bottom-left in GL, rendered with gl_FragCoord flipping in the fragment shader)
- The GL shader correctly anchors the CEF texture to the top-left corner
- The window is created WITHOUT SDL_WINDOW_OPENGL (just SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY), so SDL's data->egl_window is NULL — SDL does NOT manage the EGL window
- The app creates its own EGL context on SDL's wl_surface
- User hint: "something else is NOT anchored to the top" → "CEF's parent" is what's not anchored
