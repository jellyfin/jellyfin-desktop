# Option D: mpv Primary Surface + CEF Subsurface Above

## Summary

Invert the current surface hierarchy: mpv renders to the **parent (top-level) surface** and CEF content is placed on a **Wayland subsurface above** the parent. This puts `wp_image_description` on the same surface mpv renders to, eliminating the current mismatch where HDR metadata is set on the EGL/CEF surface while the actual HDR content is on the subsurface below.

This matches standalone mpv's architecture where the video VO owns the surface that carries the image description.

## Architecture

```
parent surface (xdg_toplevel) -- mpv renders here via dmabuf or swapchain
  |                              wp_color_management on THIS surface
  |                              wp_content_type_v1 = VIDEO
  |
  +-- cef_subsurface (place_above parent)
        |  ARGB8888 dmabuf or wl_shm buffers
        |  Transparent background, premultiplied alpha
        |  wp_content_type_v1 = NONE (default)
        |  No color management (implicitly sRGB)
        |
        +-- Input region: full window (CEF receives all input)
```

### Why This Is Better

**Current architecture problem**: `wp_color_management_surface_v1` is set on the parent surface (SDL/EGL surface) which hosts CEF, but the actual HDR content is on the mpv subsurface below. The compositor sees a PQ/BT.2020 image description on a surface that contains sRGB CEF content. Only the subsurface below carries HDR video.

**This architecture**: `wp_image_description` goes on the surface mpv actually renders to. The compositor correctly understands that the parent surface contains PQ/BT.2020 video. The CEF subsurface above has no image description (implicit sRGB) -- the compositor applies appropriate alpha blending across color spaces.

This is exactly how standalone mpv works: the video surface (parent) owns the image description, and the OSD surface (subsurface above) is implicitly sRGB.

## Surface Hierarchy Comparison

### Standalone mpv (vo_dmabuf_wayland)
```
main surface (xdg_toplevel) -- black, receives input
  +-- video_surface (subsurface, above) -- hw-decoded dmabuf, color-managed
  |     +-- osd_surface (subsurface, above video) -- wl_shm ARGB8888
  +-- callback_surface (hidden) -- frame callbacks, presentation timing
```

### Current jellyfin-desktop
```
parent surface (xdg_toplevel) -- EGL/CEF, wp_color_management HERE (mismatch)
  +-- mpv_subsurface (below parent) -- Vulkan dmabuf/swapchain
```

### Proposed (Option D)
```
parent surface (xdg_toplevel) -- mpv dmabuf, wp_color_management HERE (correct)
  +-- cef_subsurface (above parent) -- ARGB8888 dmabuf from CEF
```

## Rendering Flow

### mpv on Parent Surface

```
Per frame:
  1. handlePreferredChanged() — compositor color feedback on parent
  2. acquireBuffer() — dmabuf from pool (VkImage → dmabuf fd → wl_buffer)
  3. mpv_render_context_render() — FBO path, libplacebo gpu-next
  4. updateContentPeak() — update wp_image_description if metadata changed
  5. wl_surface_attach(parent_surface, buf->buffer, 0, 0)
  6. wl_surface_damage_buffer(parent_surface, ...)
  7. wl_surface_commit(parent_surface) — atomically presents video + CEF sub
```

### CEF on Subsurface Above

```
CEF render process:
  OnAcceleratedPaint(type, dirtyRects, info)
    → info.planes[0].fd = dmabuf fd
    → info.planes[0].stride = stride
    → info.modifier = DRM modifier
    → dup(fd) + queue for main thread

Main thread (per CEF paint, ~60Hz):
  1. importQueuedDmabuf()
     → zwp_linux_dmabuf_v1_create_params(dmabuf_)
     → zwp_linux_buffer_params_v1_add(params, fd, 0, offset, stride, mod_hi, mod_lo)
     → wl_buf = zwp_linux_buffer_params_v1_create_immed(params, w, h, DRM_FORMAT_ARGB8888, 0)
  2. wl_surface_attach(cef_surface_, wl_buf, 0, 0)
  3. wl_surface_damage_buffer(cef_surface_, dirty_x, dirty_y, dirty_w, dirty_h)
  4. wl_surface_commit(cef_surface_)
     → in sync mode: cached until parent commits
     → in desync mode: committed immediately
```

## Implementation Prototype

### Phase 1: Create CEF Subsurface

```cpp
// WaylandSubsurface::init() or new method
bool WaylandSubsurface::createCefSubsurface() {
    cef_surface_ = wl_compositor_create_surface(wl_compositor_);
    if (!cef_surface_) return false;

    cef_subsurface_ = wl_subcompositor_get_subsurface(
        wl_subcompositor_, cef_surface_, parent_surface_);
    if (!cef_subsurface_) return false;

    // CEF above parent (video)
    wl_subsurface_place_above(cef_subsurface_, parent_surface_);

    // Desync: CEF updates at its own rate (browser paint callbacks)
    // Could use sync for atomic resize -- see tradeoffs below
    wl_subsurface_set_desync(cef_subsurface_);

    // CEF needs input (the UI receives all clicks/keys)
    // Don't set empty input region -- CEF surface IS the input target

    // Viewporter for HiDPI
    if (viewporter_) {
        cef_viewport_ = wp_viewporter_get_viewport(viewporter_, cef_surface_);
    }

    wl_surface_commit(cef_surface_);
    return true;
}
```

### Phase 2: mpv Renders to Parent Surface

```cpp
// Instead of creating VkSurfaceKHR on mpv_surface_, create on parent_surface_
// WaylandSubsurface::init():
VkWaylandSurfaceCreateInfoKHR si{};
si.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
si.display = wl_display_;
si.surface = parent_surface_;  // <-- parent, not mpv_surface_
vkCreateWaylandSurfaceKHR(instance_, &si, nullptr, &vk_surface_);
```

Or for the dmabuf path (recommended for HDR parity):
```cpp
// VulkanSubsurfaceRenderer::render() — dmabuf path
void WaylandSubsurface::presentBufferOnParent(DmabufBuffer* buf) {
    buf->busy = true;
    wl_surface_attach(parent_surface_, buf->buffer, 0, 0);
    wl_surface_damage_buffer(parent_surface_, 0, 0, dmabuf_width_, dmabuf_height_);
    // Don't commit yet — let the main loop commit parent after both layers ready
}
```

### Phase 3: CEF Dmabuf to Subsurface

```cpp
// New class: WaylandCefPresenter
// Replaces OpenGLCompositor for Wayland CEF rendering

class WaylandCefPresenter {
    wl_surface* cef_surface_;
    zwp_linux_dmabuf_v1* dmabuf_;
    wl_display* display_;

    struct PendingFrame {
        int fd;
        uint32_t stride;
        uint64_t modifier;
        int width, height;
    };
    std::atomic<bool> frame_pending_{false};
    PendingFrame pending_{};
    wl_buffer* current_buffer_ = nullptr;

public:
    // Called from CEF's OnAcceleratedPaint (any thread)
    void queueFrame(int fd, uint32_t stride, uint64_t modifier, int w, int h) {
        pending_ = {dup(fd), stride, modifier, w, h};
        frame_pending_.store(true, std::memory_order_release);
    }

    // Called from main thread before parent commit
    void presentIfReady() {
        if (!frame_pending_.load(std::memory_order_acquire))
            return;
        frame_pending_.store(false, std::memory_order_relaxed);

        auto& f = pending_;

        // Create wl_buffer from CEF's dmabuf
        auto* params = zwp_linux_dmabuf_v1_create_params(dmabuf_);
        zwp_linux_buffer_params_v1_add(params, f.fd, 0, 0, f.stride,
                                        f.modifier >> 32, f.modifier & 0xffffffff);
        auto* buf = zwp_linux_buffer_params_v1_create_immed(
            params, f.width, f.height, DRM_FORMAT_ARGB8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        close(f.fd);

        if (!buf) return;

        // Attach to CEF subsurface
        wl_surface_attach(cef_surface_, buf, 0, 0);
        wl_surface_damage_buffer(cef_surface_, 0, 0, f.width, f.height);
        wl_surface_commit(cef_surface_);

        // Destroy previous buffer (compositor holds reference until release)
        if (current_buffer_)
            wl_buffer_destroy(current_buffer_);
        current_buffer_ = buf;
    }
};
```

### Phase 4: Coordinated Commit (Main Loop)

```cpp
// main.cpp render loop
void renderFrame() {
    // 1. mpv renders to dmabuf, attaches to parent
    videoController.render(viewport_w, viewport_h);
    // -> presentBufferOnParent() attaches but doesn't commit

    // 2. CEF frame (if available) attaches to cef_subsurface
    cefPresenter.presentIfReady();

    // 3. Single commit on parent presents everything
    wl_surface_commit(parent_surface_);

    // With sync subsurfaces, this atomically applies:
    //   - Video buffer on parent
    //   - CEF buffer on cef_subsurface
    // With desync subsurfaces, CEF committed independently in step 2
}
```

### Phase 5: Color Management on Parent

```cpp
// setupColorFeedback() — on parent_surface_ (same as now, but now correct)
color_feedback_ = wp_color_manager_v1_get_surface_feedback(
    color_manager_, parent_surface_);

// activateColorManagement() — on parent_surface_ (same as now)
color_surface_ = wp_color_manager_v1_get_surface(
    color_manager_, parent_surface_);
setHdrImageDescription();

// This is now correct: the image description describes the content
// actually rendered to this surface (mpv video), not CEF's sRGB UI
```

## Sync vs Desync Subsurface for CEF

### Desync (Recommended Initially)

```
CEF subsurface commits independently of parent.
+ CEF can update at its own rate (browser refresh rate may differ from video)
+ No frame timing coordination needed
+ Simple implementation
- Video and CEF may not resize atomically (1-2 frame gap during resize)
```

### Sync (Future Optimization for Atomic Resize)

```
CEF subsurface commits are cached until parent commits.
+ Atomic resize: video + CEF update in same compositor frame
+ No visual glitches during resize
- CEF frame rate tied to video frame rate (parent commit frequency)
- Must ensure CEF has a frame ready before each parent commit
- More complex coordination
```

Recommendation: Start with desync (simpler, proven pattern), add sync mode later if resize quality is insufficient.

## Handling SDL's EGL Surface

**The biggest implementation challenge**: SDL creates an EGL surface on the parent `wl_surface`. If mpv uses the parent surface for dmabuf presentation, the EGL surface must not be active simultaneously.

### Approach 1: Disable SDL's EGL rendering (Recommended)

```cpp
// Don't create EGL context on the SDL window at all
// SDL_CreateWindow without SDL_WINDOW_OPENGL flag
SDL_Window* window = SDL_CreateWindow("Jellyfin", w, h, SDL_WINDOW_VULKAN);

// Use SDL only for:
// - Window management (create, resize, fullscreen, minimize)
// - Input events (keyboard, mouse, gamepad)
// - Platform properties (wl_display, wl_surface pointers)

// CEF no longer renders via EGL on the parent
// CEF renders offscreen (already does) and presents to cef_subsurface via dmabuf
```

This means: no OpenGL compositor, no `eglSwapBuffers`, no `glTexSubImage2D`. All rendering goes through Vulkan (mpv) and dmabuf presentation (CEF).

### Approach 2: EGL on CEF subsurface instead of parent

```cpp
// Create EGL surface on cef_surface_ instead of parent_surface_
// SDL doesn't support this directly — need manual EGL setup

EGLSurface cef_egl = eglCreateWindowSurface(egl_display, config,
    (EGLNativeWindowType)cef_surface_, nullptr);

// CEF renders to GL textures → composite to cef_egl → eglSwapBuffers
// Parent surface is free for mpv dmabuf presentation
```

This preserves the existing OpenGL compositor path but moves it to the CEF subsurface. The EGL surface on the subsurface presents to the compositor via Mesa's Wayland EGL backend.

### Approach 3: Keep EGL on parent, mpv on subsurface (minimal change variant)

Keep the current surface hierarchy but fix the color management by setting `wp_image_description` on the **mpv subsurface**, not the parent.

```cpp
// Change: color management on mpv_surface_ instead of parent_surface_
color_surface_ = wp_color_manager_v1_get_surface(color_manager_, mpv_surface_);
color_feedback_ = wp_color_manager_v1_get_surface_feedback(color_manager_, mpv_surface_);
```

**Caveat**: KWin may not send `preferred_changed` to subsurfaces (only top-level surfaces). This needs testing. If KWin doesn't support it, this variant is non-viable.

## CEF Dmabuf Details

### What CEF Provides

CEF's `OnAcceleratedPaint` on Linux provides:
```cpp
struct CefAcceleratedPaintInfo {
    struct Plane {
        int fd;        // dmabuf file descriptor
        uint32_t stride;
        uint32_t offset;
        uint32_t size;
    };
    Plane planes[4];   // up to 4 planes
    int num_planes;
    uint64_t modifier; // DRM format modifier
    uint32_t format;   // DRM fourcc (inferred as ARGB8888)
};
```

### Format Compatibility

CEF exports ARGB8888 (DRM_FORMAT_ARGB8888) dmabufs with premultiplied alpha. This is universally supported by Wayland compositors.

The alpha channel is critical for the overlay to work — transparent areas (video visible behind UI) have `alpha = 0`, opaque areas (UI elements) have `alpha = 1.0`.

### Popup Handling

CEF popups (dropdown menus, context menus) come as separate `OnAcceleratedPaint` calls with `PaintElementType == PET_POPUP`. In the current architecture, popups are composited in the OpenGL shader.

For the subsurface approach, popups could be:
1. **Pre-composited by CEF** — request CEF to blend popups into the main view paint buffer. This eliminates separate popup handling.
2. **Separate popup subsurface** — create a third subsurface for popup content. More complex but allows proper stacking.
3. **Composited in GPU before subsurface commit** — import popup dmabuf, blend with main CEF dmabuf in a quick Vulkan pass, present result on cef_subsurface.

Recommendation: Start with option 1 (request CEF to composite popups internally via `CefBrowserHost::SetShowPopupRects`).

## Pros

- **Correct color management**: `wp_image_description` on the surface mpv renders to, matching standalone mpv exactly
- **Clean separation**: Video surface = color-managed HDR/SDR, overlay surface = implicit sRGB
- **Compositor handles cross-color-space blending**: The compositor knows the parent is PQ/BT.2020 and the subsurface is sRGB. It applies correct color-space conversion during alpha compositing.
- **Matches standalone mpv pattern**: mpv's `vo_dmabuf_wayland` uses the same parent=video, child=overlay architecture
- **Zero-copy CEF presentation**: CEF dmabuf → `wl_buffer` → `wl_surface_attach` — no GPU copy
- **Can match standalone mpv protocol output exactly**: Same surface hierarchy, same `wp_image_description` location

## Cons

- **Requires decoupling CEF from SDL's EGL surface**: Significant refactoring. The OpenGL compositor must be replaced with dmabuf presentation to a subsurface.
- **Input routing changes**: Currently SDL/EGL surface gets input. Moving CEF to a subsurface means the subsurface needs the input region. SDL may not handle this transparently — may need manual Wayland input protocol handling.
- **CEF Wayland OSR issues**: CEF's Wayland OSR path has scaling bugs (hence the current `--ozone-platform=x11` workaround). Even as an offscreen renderer, CEF's dmabuf format/size may not match expectations on all drivers.
- **Popup handling complexity**: Dropdown menus and context menus need special treatment in the subsurface model.
- **More surfaces to manage**: Three Wayland surfaces (parent, cef_sub, video_viewport) instead of two.

## Risk Assessment

| Risk | Severity | Mitigation |
|---|---|---|
| CEF dmabuf format incompatible with `wl_buffer` | Medium | Verify DRM_FORMAT_ARGB8888 universally supported; fallback to wl_shm |
| KWin preferred_changed only on top-level surfaces | Low (Option D avoids this — parent IS top-level) | N/A for Option D |
| SDL input routing broken without EGL surface | Medium | Manually set input region on cef_subsurface; handle via wl_pointer/wl_keyboard directly if needed |
| CEF popup rendering broken in subsurface model | Low | Pre-composite popups; test with dropdown menus |
| Performance regression from dmabuf presentation vs EGL swap | Low | dmabuf is zero-copy; EGL swap may actually be slower |

## Validation

1. `WAYLAND_DEBUG=1` comparison: `wp_image_description` now on parent (where mpv renders) — should match standalone mpv
2. Visual: CEF UI transparent over video, alpha blending correct (compositor handles cross-color-space blend)
3. Resize: Both layers resize (gaps OK per CLAUDE.md, no stretching)
4. Input: Click on UI element works, click on transparent area passes to video
5. HDR: PQ/BT.2020 signaling on parent matches `dev/linux/color-mgmt-compare.sh`
6. SDR: sRGB signaling on parent, no color management on CEF subsurface

## Comparison with Option A

| Aspect | Option A (Unified Dmabuf) | Option D (mpv Primary) |
|---|---|---|
| Surface hierarchy | Current (parent=CEF, sub=mpv) | Inverted (parent=mpv, sub=CEF) |
| Color management location | Parent surface (mismatch) | Parent surface (correct) |
| CEF rendering | OpenGL compositor on parent EGL | Dmabuf to subsurface |
| Implementation delta | Small (extend HDR path to SDR) | Large (invert hierarchy + CEF subsurface) |
| Standalone mpv parity | Protocol values match, surface mismatch | Protocol values AND surface match |
| Risk | Low | Medium-High |

**Option A is simpler to implement now.** The color management "mismatch" (image description on parent while video is on subsurface) may not cause visible issues in practice — the compositor still receives the correct metadata and applies it to the subsurface's content.

**Option D is architecturally cleaner.** If the mismatch causes compositor-specific bugs (some compositors may apply the image description only to the parent surface's own content, not its children), Option D eliminates the issue entirely.

**Recommendation**: Implement Option A first (low risk, proven approach). Pursue Option D if compositor-specific color management issues arise, or as the long-term target architecture.
