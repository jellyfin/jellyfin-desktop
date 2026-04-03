# Third-Party mpv gpu-next Rendering Pipeline Analysis

## Overview

The `third_party/mpv/video/out/gpu_next/` directory contains a custom libmpv render backend that replaces mpv's standard `vo_gpu_next` output driver with a simplified, synchronous rendering engine. This backend is the interface between jellyfin-desktop's video stack and libplacebo's GPU rendering capabilities.

The key difference from upstream mpv: **standalone mpv owns the window and swapchain**; the libmpv backend **receives render targets from the host application** (either a pre-wrapped FBO or a surface for libplacebo-managed swapchain).

## File Map

```
third_party/mpv/video/out/gpu_next/
  libmpv_gpu_next.h   — vtable interface for context backends (GL/VK)
  libmpv_gpu_next.c   — render_backend dispatch: init, render, reconfig, resize, reset
  context.h           — gpu_ctx structure (standalone mpv compatibility)
  context.c           — GL and VK context init, FBO wrapping, swapchain creation
  ra.h                — rendering abstraction (pl_gpu + pl_renderer wrapper)
  ra.c                — texture management, frame queue, upload/download
  video.h             — pl_video engine interface
  video.c             — synchronous rendering: frame queue, color management, OSD, scaling
```

## render_backend_gpu_next (libmpv_gpu_next.c)

Entry point for all libmpv rendering. Implements the `render_backend_fns` vtable:

```
init(ctx, params)
  -> find API context backend (GL or VK) by MPV_RENDER_PARAM_API_TYPE
  -> backend->init() creates pl_gpu, optionally pl_swapchain
  -> pl_video_init() creates synchronous rendering engine

render(ctx, params, frame)
  -> if swapchain: acquire, render_to_swapchain, submit, swap
  -> if FBO: wrap_fbo, render, destroy wrapper, done_frame

reconfig(ctx, params) -> pl_video_reconfig (new video params)
resize(ctx, src, dst, osd) -> pl_video_resize (viewport changed)
reset(ctx) -> pl_video_reset (seek/flush)
```

### Two Rendering Paths

**Swapchain path** (lines 142-189): Used when `MPV_RENDER_PARAM_VULKAN_SURFACE` was provided at init. libplacebo owns the swapchain lifecycle.

```c
pl_swapchain_resize()            // match window dimensions
pl_swapchain_colorspace_hint()   // negotiate HDR format (conditional)
pl_swapchain_start_frame()       // acquire VkImage
pl_video_render_to_swapchain()   // render with display_profile
pl_swapchain_submit_frame()      // submit render commands
pl_swapchain_swap_buffers()      // vsync + present
```

**FBO path** (lines 191-206): Used when only `MPV_RENDER_PARAM_VULKAN_FBO` is provided per frame. Application owns buffer management.

```c
wrap_fbo()          // wrap VkImage as pl_tex (temporary)
pl_video_render()   // render to wrapped texture
ra_next_tex_destroy() // destroy wrapper
done_frame()        // GPU sync (pl_gpu_finish on Vulkan)
```

### Swapchain Color Hint Logic

The hint logic (lines 149-169) is nuanced:

```c
const mpv_display_profile *dp = p->context->display_profile;
bool platform_owns_hint = dp && dp->max_luma > 0 && dp->ref_luma > 0;

if (dp && !platform_owns_hint && frame->current) {
    // Only hint when platform didn't provide luminance data
    if (pl_color_transfer_is_hdr(source->transfer)) {
        // Hint: "I have HDR content, pick an HDR-capable format"
        pl_swapchain_colorspace_hint(swapchain, &source_color);
    } else {
        pl_swapchain_colorspace_hint(swapchain, NULL); // SDR: default
    }
}
```

When `platform_owns_hint` is true (dmabuf path provides luminance data), the hint is skipped because the platform (WaylandSubsurface) handles color management directly via `wp_color_management`. The swapchain doesn't need to negotiate color — it's just a presentation mechanism.

## Context Backends (context.c)

### OpenGL Backend

```c
libmpv_gpu_next_init_gl()
  -> get MPV_RENDER_PARAM_OPENGL_INIT_PARAMS (get_proc_address)
  -> pl_opengl_create() with make_current/release_current callbacks
  -> ra_pl_create(gpu) to create rendering abstraction
```

FBO wrapping: `pl_opengl_wrap(gpu, {framebuffer, width, height, iformat})`

### Vulkan Backend

```c
libmpv_gpu_next_init_vk()
  -> get MPV_RENDER_PARAM_VULKAN_INIT_PARAMS (VkInstance, VkDevice, VkQueue, etc.)
  -> pl_vulkan_import() — import user's Vulkan device into libplacebo
  -> if MPV_RENDER_PARAM_VULKAN_SURFACE provided:
       pl_vulkan_create_swapchain() with FIFO present mode
       store display_profile pointer for per-frame reads
       write swapchain_out to display_profile for platform re-hinting
       initial HDR hint if display has luminance data
       probe swapchain format via test frame
  -> if MPV_RENDER_PARAM_DISPLAY_PROFILE (no swapchain):
       store for FBO-path video.c to read content metadata
  -> ra_pl_create(gpu)
```

FBO wrapping: `pl_vulkan_wrap(gpu, {image, width, height, format, usage})` + `pl_vulkan_release_ex()`

### Display Profile Connection

The `mpv_display_profile` struct is the bridge between the platform layer and the rendering engine:

```c
struct mpv_display_profile {
    float max_luma;        // Display peak (cd/m², raw from compositor)
    float min_luma;        // Display black level (raw)
    float ref_luma;        // Reference white (raw, e.g. 252 nits)

    // Written by video.c per-frame (HDR passthrough):
    float content_peak;       // Source max_luma (nits)
    float content_min_luma;   // Source min_luma
    float content_max_cll;    // MaxCLL from mastering metadata
    float content_max_fall;   // MaxFALL from mastering metadata

    void* swapchain_out;   // Written by context.c: pl_swapchain* for re-hinting
};
```

**Read by video.c**: `max_luma`, `ref_luma`, `min_luma` — used to compute target HDR metadata with the scaling formula.
**Written by video.c**: `content_peak`, `content_min_luma`, `content_max_cll`, `content_max_fall` — used by the platform to update the surface's image description.

## Video Rendering Engine (video.c)

### pl_video Structure

```c
struct pl_video {
    struct ra_next *ra;         // libplacebo rendering abstraction
    ra_queue queue;             // frame interpolation queue
    uint64_t last_frame_id;     // dedup frame pushes
    double last_pts;            // for redraws without new frame

    struct m_config_cache *opts_cache;  // target-trc, target-prim, scalers
    struct mp_image_params current_params;
    struct mp_rect current_src, current_dst;
    struct osd_state *current_osd_state;
    struct mp_osd_res osd_res;

    struct pl_video_osd_state osd_state_storage;
    struct mp_csp_equalizer_state *video_eq;     // brightness/contrast/etc.
    struct pl_filter_config scalers[SCALER_COUNT];
    bool flipped;  // OpenGL FBO orientation
};
```

### Frame Upload Pipeline

```
mp_image (CPU) → map_frame callback → pl_frame (GPU)
  1. mp_image_params_guess_csp() — fill in missing colorspace metadata
  2. ra_upload_mp_image() — upload YUV/RGB planes to pl_tex
  3. Override frame->color with guessed values
  4. If sRGB source → treat as gamma 2.2 (IEC 61966-2-1)
  5. Apply ICC profile if present
  6. Set chroma location for 4:2:0
  7. Apply film grain from AV1/VP9 metadata
```

### Rendering Pipeline (render_to_target)

```
render_to_target(p, frame, target_frame):
  1. Set target crop from current_dst (with flip for GL)
  2. Set output levels to PL_COLOR_LEVELS_FULL
  3. If target sRGB → treat as gamma 2.2 (match source behavior)
  4. sdr_adjust_gamma: avoid unnecessary gamma roundtrip
  5. Push frame to queue (if new frame_id)
  6. Queue update → get frame mix for interpolation
  7. Apply source crop to each mix frame
  8. Generate OSD overlays (subtitles, UI elements)
  9. Call ra_next_render_image_mix() with render params:
     - upscaler, downscaler, plane_upscaler (from mpv options)
     - peak detection, dithering, sigmoid
     - color adjustment (brightness, contrast, hue, saturation, gamma)
```

### Color Management in pl_video_render (FBO path)

```c
pl_video_render(p, frame, target_tex, display_profile):
  1. Read mpv options (target_trc, target_prim, target_peak)
  2. Default: target = pl_color_space_srgb

  3. If options override target colorspace:
     target = {prim, trc, peak, min_luma, max_cll, max_fall}

  4. HDR passthrough check:
     if target is PQ && no explicit target-peak:
       → copy source HDR metadata to target
       → write content metadata to display_profile
       → libplacebo sees matching peaks → no tone mapping

  5. If display_profile has luminance data (and not passthrough):
     → apply mpv's luminance scaling formula:
       a = min_luma
       b = (PL_COLOR_SDR_WHITE - PL_COLOR_HDR_BLACK) / (ref_luma - a)
       target.hdr.max_luma = (max_luma - a) * b + PL_COLOR_HDR_BLACK
     → libplacebo tone maps source → display capabilities

  6. Build pl_frame target with target_color
  7. Call render_to_target()
```

### Color Management in pl_video_render_to_swapchain

```c
pl_video_render_to_swapchain(p, frame, sw_frame, display_profile):
  1. pl_frame_from_swapchain() → get target color from swapchain format
     (libplacebo reads VkSurfaceFormatKHR → pl_color_space)

  2. If display_profile has luminance data:
     → override swapchain's default HDR metadata
     → same scaling formula as FBO path
     → matches standalone mpv's target.color = hint

  3. Call render_to_target()
```

### The Luminance Scaling Formula

This formula appears in three places (video.c, context.c, wayland_subsurface.cpp) and must be identical everywhere:

```
a = display_profile->min_luma    // display black level (raw from compositor)
b = (PL_COLOR_SDR_WHITE - PL_COLOR_HDR_BLACK) / (display_profile->ref_luma - a)

target.hdr.min_luma = (min_luma - a) * b + PL_COLOR_HDR_BLACK
target.hdr.max_luma = (max_luma - a) * b + PL_COLOR_HDR_BLACK
```

Origin: `wayland_common.c` `info_done()` in standalone mpv. Converts compositor's luminance values (in cd/m²) to libplacebo's internal reference scale (where SDR white = `PL_COLOR_SDR_WHITE` = 203 nits).

## Scaler Configuration (map_scaler)

Maps mpv's scaler options to libplacebo filter configs:

```
Fixed scalers: bilinear, bicubic_fast, nearest, oversample
Named presets: pl_find_filter_preset() (lanczos, spline36, etc.)
Raw functions: pl_find_filter_function_preset()
```

Supports:
- Kernel + window function
- Per-parameter overrides (params[0], params[1])
- Clamping, antiring, blur, taper, radius

mpv options: `--scale`, `--dscale`, `--cscale` with `SCALER_SCALE`, `SCALER_DSCALE`, `SCALER_CSCALE` units.

## OSD/Subtitle Rendering

```
update_overlays():
  1. osd_render() → sub_bitmap_list (CPU-rendered subtitles)
  2. For each bitmap:
     a. Allocate/reuse pl_tex from pool
     b. Upload bitmap to GPU texture
     c. Build pl_overlay_part[] with src/dst rects + color
     d. Create pl_overlay with blending mode:
        - SUBBITMAP_BGRA → PL_OVERLAY_NORMAL, premultiplied alpha
        - SUBBITMAP_LIBASS → PL_OVERLAY_MONOCHROME, independent alpha
  3. Attach overlays to target_frame → rendered during mix
```

## Rendering Abstraction (ra.c)

Wraps libplacebo's `pl_gpu` + `pl_renderer` for mpv compatibility:

```c
struct ra_next {
    pl_gpu gpu;
    pl_renderer renderer;   // libplacebo's main rendering engine
    pl_log pl_log;
};
```

Key functions:
- `ra_upload_mp_image()` — converts mp_image planes to pl_tex array
- `ra_cleanup_pl_frame()` — destroys temporary textures
- `ra_next_render_image_mix()` — calls `pl_render_image_mix()` (the core libplacebo render call)
- `ra_next_queue_*()` — frame queue for temporal interpolation
- `ra_next_find_fmt()` — find texture format by capabilities
- `ra_next_tex_*()` — texture create/destroy/recreate/upload/download

## Key Differences from Upstream mpv's vo_gpu_next

| Aspect | Upstream vo_gpu_next | Custom libmpv backend |
|---|---|---|
| Window ownership | mpv owns (via ra_ctx) | Host app owns |
| Swapchain | Always via ra_vk_ctx | Optional (user provides surface or FBO) |
| Rendering API | pl_renderer_render_frame() | pl_video_render() / pl_video_render_to_swapchain() |
| Frame queue | Built-in (pl_queue) | Same (wrapped in ra_queue) |
| Color management | Wayland: wp_color_management | Display profile struct (bidirectional) |
| OSD rendering | Same pipeline | Same pipeline |
| Scaler config | Same options | Same options (via m_config_cache) |
| Hardware decoding | Full hwdec support | hwdec_devices_create() but no hwdec interop |
| Direct rendering | Supported (vd-lavc-dr) | Not implemented (get_image returns NULL) |

## Data Flow Summary

```
Host App                         mpv core                    gpu-next backend
---------                        --------                    ----------------
                                 Decode frame
                                 → mp_image
VideoRenderController
  ::render()
    → mpv_render_context_render()
                                 → render_backend.render()
                                                             → [swapchain or FBO path]
                                                             → pl_video_render*()
                                                               → map_frame (upload)
                                                               → pl_render_image_mix
                                                               → OSD overlays
                                                             → [present or done_frame]
    ← return
  [dmabuf: presentBuffer()]
  [swapchain: implicit via swap_buffers]
```
