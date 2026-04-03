# Option E: Triple Subsurface (mpv Architecture Mirror)

## Summary

Mirror standalone mpv's `vo_dmabuf_wayland` surface architecture exactly: a minimal parent surface with three subsurfaces (video, CEF overlay, and optionally a callback/timing surface). The parent holds no content — it's a coordinator that commits all children atomically. This is the closest to how standalone mpv works on Wayland.

## Architecture

```
parent surface (xdg_toplevel) -- minimal (single-pixel black buffer or empty)
  |                               Coordinates commits, receives xdg events
  |                               NO rendering here
  |
  +-- video_subsurface (below parent) -- mpv dmabuf FBO
  |     wp_color_management on video_surface
  |     wp_content_type_v1 = VIDEO
  |     wp_viewporter for scaling
  |
  +-- cef_subsurface (above parent) -- CEF dmabuf ARGB8888
  |     No color management (implicit sRGB)
  |     Transparent background, premultiplied alpha
  |     Receives input (full window input region)
  |
  +-- [optional] callback_subsurface -- hidden, frame timing only
        wp_presentation_feedback attached here
```

### Why Mirror mpv's Architecture

Standalone mpv's `vo_dmabuf_wayland` has the most battle-tested Wayland video surface architecture:
- Parent surface is minimal (single-pixel black buffer via `wp_single_pixel_buffer_v1`)
- Video on subsurface with its own color management
- OSD on subsurface above video with sRGB wl_shm buffers
- Presentation feedback on a dedicated callback surface

By mirroring this:
1. Color management goes on the video subsurface — exactly where standalone mpv puts it
2. Parent surface is inert — no color space confusion
3. Each layer has a clear role with independent update rates

## Protocol Flow (HDR Example)

```
Initialization:
  1. SDL creates parent wl_surface (xdg_toplevel)
  2. wp_single_pixel_buffer_v1 → black buffer on parent (or empty)
  3. Create video_surface as subsurface below parent
  4. Create cef_surface as subsurface above parent
  5. wp_color_manager_v1_get_surface(color_manager, video_surface) → color management on VIDEO
  6. wp_color_manager_v1_get_surface_feedback(color_manager, video_surface) → feedback on VIDEO
  7. Initial preferred_changed query → determine HDR/SDR

Frame presentation (HDR):
  1. mpv renders to VkImage FBO via libplacebo gpu-next
  2. video.c writes content mastering metadata to display_profile
  3. Export VkImage as dmabuf → wl_buffer
  4. wl_surface_attach(video_surface, video_buf, 0, 0)
  5. wl_surface_damage_buffer(video_surface, ...)
  6. setHdrImageDescription() on video_surface (PQ/BT.2020 + mastering data)
  7. CEF dmabuf → wl_buffer
  8. wl_surface_attach(cef_surface, cef_buf, 0, 0)
  9. wl_surface_damage_buffer(cef_surface, ...)
  10. wl_surface_commit(parent_surface) → atomically presents all subsurfaces (sync mode)

Frame presentation (SDR):
  Same as HDR but:
  - video_surface image description = sRGB/BT.709
  - No mastering metadata or CLL
```

## Implementation Prototype

### Surface Setup

```cpp
class TripleSubsurfaceStack {
    wl_display* display_;
    wl_compositor* compositor_;
    wl_subcompositor* subcompositor_;
    wl_surface* parent_;           // SDL's surface
    wl_surface* video_surface_;
    wl_surface* cef_surface_;
    wl_subsurface* video_sub_;
    wl_subsurface* cef_sub_;
    wp_viewport* video_viewport_;
    wp_viewport* cef_viewport_;
    wp_color_management_surface_v1* color_surface_;  // on video_surface_

public:
    bool init(SDL_Window* window, wl_compositor* comp, wl_subcompositor* subcomp,
              wp_viewporter* viewporter, wp_color_manager_v1* color_mgr) {
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        display_ = (wl_display*)SDL_GetPointerProperty(props,
            SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        parent_ = (wl_surface*)SDL_GetPointerProperty(props,
            SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        compositor_ = comp;
        subcompositor_ = subcomp;

        // Make parent minimal — black background
        // (letterbox when video aspect != window aspect)
        // Parent buffer set by SDL or via wp_single_pixel_buffer

        // Video subsurface (below parent)
        video_surface_ = wl_compositor_create_surface(compositor_);
        video_sub_ = wl_subcompositor_get_subsurface(subcompositor_,
            video_surface_, parent_);
        wl_subsurface_place_below(video_sub_, parent_);
        wl_subsurface_set_desync(video_sub_);  // or sync for atomic

        wl_region* empty = wl_compositor_create_region(compositor_);
        wl_surface_set_input_region(video_surface_, empty);
        wl_region_destroy(empty);

        if (viewporter)
            video_viewport_ = wp_viewporter_get_viewport(viewporter, video_surface_);

        // CEF subsurface (above parent)
        cef_surface_ = wl_compositor_create_surface(compositor_);
        cef_sub_ = wl_subcompositor_get_subsurface(subcompositor_,
            cef_surface_, parent_);
        wl_subsurface_place_above(cef_sub_, parent_);
        wl_subsurface_set_desync(cef_sub_);

        // CEF GETS input (no empty region — it's the interactive layer)
        if (viewporter)
            cef_viewport_ = wp_viewporter_get_viewport(viewporter, cef_surface_);

        // Color management on VIDEO surface
        if (color_mgr) {
            color_surface_ = wp_color_manager_v1_get_surface(color_mgr, video_surface_);
        }

        wl_surface_commit(video_surface_);
        wl_surface_commit(cef_surface_);
        wl_surface_commit(parent_);
        wl_display_roundtrip(display_);
        return true;
    }
};
```

### Video Presentation

```cpp
void presentVideo(DmabufBuffer* buf, int w, int h) {
    buf->busy = true;

    // Apply viewport destination for HiDPI
    if (video_viewport_) {
        int dest_w = pending_dest_width_.load(std::memory_order_relaxed);
        int dest_h = pending_dest_height_.load(std::memory_order_relaxed);
        if (dest_w > 0 && dest_h > 0)
            wp_viewport_set_destination(video_viewport_, dest_w, dest_h);
    }

    wl_surface_attach(video_surface_, buf->buffer, 0, 0);
    wl_surface_damage_buffer(video_surface_, 0, 0, w, h);

    // Don't commit yet in sync mode — parent commit will apply
    // In desync mode, commit immediately:
    wl_surface_commit(video_surface_);
}
```

### CEF Presentation

```cpp
void presentCef(int fd, uint32_t stride, uint64_t modifier, int w, int h) {
    auto* params = zwp_linux_dmabuf_v1_create_params(dmabuf_);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride,
        modifier >> 32, modifier & 0xffffffff);
    auto* buf = zwp_linux_buffer_params_v1_create_immed(
        params, w, h, DRM_FORMAT_ARGB8888, 0);
    zwp_linux_buffer_params_v1_destroy(params);

    if (!buf) { close(fd); return; }

    if (cef_viewport_) {
        wp_viewport_set_destination(cef_viewport_,
            pending_cef_dest_w_, pending_cef_dest_h_);
    }

    wl_surface_attach(cef_surface_, buf, 0, 0);
    wl_surface_damage_buffer(cef_surface_, 0, 0, w, h);
    wl_surface_commit(cef_surface_);

    if (current_cef_buffer_)
        wl_buffer_destroy(current_cef_buffer_);
    current_cef_buffer_ = buf;
    close(fd);
}
```

### Color Management on Video Surface

```cpp
void setupColorFeedback() {
    // Feedback on VIDEO surface, not parent
    color_feedback_ = wp_color_manager_v1_get_surface_feedback(
        color_manager_, video_surface_);
    wp_color_management_surface_feedback_v1_add_listener(
        color_feedback_, &s_feedbackListener, this);
    handlePreferredChanged();  // initial query
}

void setHdrImageDescription() {
    // Image description on video_surface_ via color_surface_
    auto* creator = wp_color_manager_v1_create_parametric_creator(color_manager_);
    wp_image_description_creator_params_v1_set_primaries_named(creator,
        primaries_map_[PL_COLOR_PRIM_BT_2020]);
    wp_image_description_creator_params_v1_set_tf_named(creator,
        transfer_map_[PL_COLOR_TRC_PQ]);
    // ... mastering metadata, CLL, etc. (same as current)

    auto* desc = wp_image_description_creator_params_v1_create(creator);
    wp_color_management_surface_v1_set_image_description(
        color_surface_, desc, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wp_image_description_v1_destroy(desc);
}
```

## The preferred_changed Problem

**Critical question**: Does the compositor send `preferred_changed` events to subsurfaces?

### KWin (KDE Plasma 6.x)
KWin sends `preferred_changed` to surfaces that have a `wp_color_management_surface_feedback_v1` attached. This should work for subsurfaces, but KWin's implementation may only track top-level surfaces. **Needs testing.**

### Mutter (GNOME)
Does not support `wp_color_management_v1` yet. Not relevant.

### Wlroots
Supports `preferred_changed` on any surface with feedback. Should work for subsurfaces.

### Mitigation if subsurface feedback doesn't work

If `preferred_changed` doesn't fire on subsurfaces:
1. Get feedback on the **parent surface** (top-level, guaranteed to work)
2. Apply the display profile to the **video subsurface's** image description
3. This is a read-from-parent, write-to-child pattern — the feedback source and image description target can be different surfaces

```cpp
// Feedback on parent (guaranteed to work)
color_feedback_ = wp_color_manager_v1_get_surface_feedback(
    color_manager_, parent_surface_);

// Image description on video surface
color_surface_ = wp_color_manager_v1_get_surface(
    color_manager_, video_surface_);

// handlePreferredChanged reads from feedback → writes to color_surface_
```

This is the safest approach and should be the default implementation.

## Comparison with Standalone mpv's Surface Model

| Surface | Standalone mpv | Option E |
|---|---|---|
| Parent (xdg_toplevel) | Black single-pixel buffer | SDL window (empty or black) |
| Video subsurface | hw-decoded dmabuf, color-managed | libplacebo-rendered dmabuf, color-managed |
| OSD/Overlay subsurface | wl_shm ARGB8888 (subtitles) | CEF dmabuf ARGB8888 (browser UI) |
| Callback surface | Hidden, frame timing | Not needed (mpv timing internal) |
| Color feedback | On callback_surface | On parent surface (safer) |
| Color description | On callback_surface | On video_surface |

## Pros

- **Closest to standalone mpv's architecture**: Same surface hierarchy, same color management pattern
- **Clean separation of concerns**: Each surface has one job (video, UI, coordination)
- **Color management on the correct surface**: Video surface carries the image description
- **Independent update rates**: Video and CEF can commit at different frame rates
- **Compositor handles cross-color-space compositing**: sRGB CEF over PQ/BT.2020 video
- **Parent surface can be empty**: No wasted GPU work on the coordinator surface

## Cons

- **Most invasive refactoring**: Requires full decoupling of CEF from SDL's EGL surface, moving to dmabuf-based CEF presentation, and restructuring the surface hierarchy
- **Three surfaces to manage**: More complex lifecycle, more Wayland roundtrips
- **preferred_changed on subsurface may not work**: Requires the feedback-on-parent, description-on-child pattern as mitigation
- **SDL EGL surface conflict**: SDL creates EGL on the parent. Must either disable SDL rendering or make the parent truly inert (may require SDL patches or `SDL_WINDOW_VULKAN` without actually creating a Vulkan surface)
- **CEF Ozone/X11 constraint**: CEF uses `--ozone-platform=x11` for OSR. CEF's dmabuf output may have X11-specific assumptions about format/stride
- **Input routing on subsurface**: SDL expects input on its surface. CEF subsurface needs custom input region handling.

## When to Choose This Over Option A or D

| Scenario | Best Option |
|---|---|
| Quick win, minimal changes | **Option A** (unified dmabuf, current hierarchy) |
| Color management mismatch causes real bugs | **Option D** (mpv on parent) or **Option E** (triple subsurface) |
| Need exact standalone mpv surface architecture | **Option E** |
| Compositor only sends preferred_changed to top-level | **Option D** (mpv IS the top-level surface) |

## Risk Assessment

| Risk | Severity | Mitigation |
|---|---|---|
| preferred_changed not sent to subsurfaces | Medium | Feedback on parent, description on child |
| SDL conflicts with empty parent | Medium | SDL_WINDOW_VULKAN flag, no actual Vulkan surface |
| CEF dmabuf format varies by driver | Medium | Verify ARGB8888 on target hardware; wl_shm fallback |
| Three-surface commit ordering issues | Low | Desync mode initially; sync mode as optimization |
| Compositor doesn't blend sRGB sub over PQ parent correctly | Low | Test on KDE Plasma 6.x; file compositor bugs |

## Conclusion

Option E is the architecturally purest approach — it mirrors standalone mpv's proven Wayland surface model and puts every layer in its correct place. However, the implementation cost is the highest of all options, requiring full decoupling of CEF rendering from SDL's EGL surface and custom Wayland surface management.

**Recommended as the long-term target architecture**, with Option A as the pragmatic first step. The migration path is:
1. Option A (now): Unify on dmabuf, fix SDR path, validate protocol parity
2. Option D or E (later): Restructure surface hierarchy, move CEF to subsurface, correct color management surface targeting
