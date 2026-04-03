# Option A: Unified Dmabuf FBO with Explicit Color Management

## Summary

Unify both HDR and SDR rendering paths to use the dmabuf FBO approach. mpv always renders to VkImage framebuffer objects exported as Linux dmabufs, presented via `wl_surface_attach()` on a Wayland subsurface. The application owns color management for the video surface via the `wp_color_management_v1` protocol, setting explicit image descriptions that match standalone mpv's output exactly.

## Architecture

```
SDL Window (parent wl_surface)
  |
  +-- mpv wl_subsurface (place_below, desync, empty input region)
  |     |
  |     +-- Dmabuf FBO pool (4 buffers, VkImage -> dmabuf fd -> wl_buffer)
  |     +-- wp_color_management_v1 image description (HDR: PQ/BT.2020, SDR: sRGB)
  |     +-- wp_viewporter for HiDPI logical size mapping
  |
  +-- CEF on parent surface (transparent overlay)
```

### Rendering Flow

```
Per frame:
  1. dispatchColorEvents()          — drain any pending preferred_changed
  2. acquireBuffer()                — get a free DmabufBuffer from pool
  3. mpv_render_context_render()    — render to VkImage via MPV_RENDER_PARAM_VULKAN_FBO
     -> video.c: pl_video_render() — libplacebo renders with target color from mpv options
     -> writes content_peak to display_profile (HDR passthrough)
  4. updateContentPeak()            — if content metadata changed, update image description
  5. presentBuffer()                — wl_surface_attach + damage + commit
  6. buffer_release callback        — compositor returns buffer when done
```

### Color Management Flow

```
Compositor preferred_changed event
  -> handlePreferredChanged()
     -> query preferred image description (synchronous, temp event queue)
     -> apply info_done scaling (mpv's exact formula)
     -> store in preferred_csp_ and display_profile_
     -> if changed: setHdrImageDescription()

Per-frame (HDR):
  video.c writes content_peak/content_min_luma/content_max_cll/content_max_fall
  -> updateContentPeak() reads display_profile_.content_peak
  -> if max_cll changed: setHdrImageDescription() with content mastering metadata

Per-frame (SDR):
  No content-dependent updates needed (sRGB is static)
```

## What Changes from Current Implementation

### Currently: Two branches at `output_is_hdr_`
```cpp
// WaylandSubsurface::init() — HDR detection
output_is_hdr_ = preferred_csp_valid_ && preferred_csp_.hdr.max_luma > PL_COLOR_SDR_WHITE;

// VulkanSubsurfaceRenderer::render() — runtime branch
if (wl && wl->hasDmabufPool()) {
    // FBO path: dmabuf + explicit color management
} else {
    // Swapchain path: libplacebo swapchain + Mesa WSI color
}
```

### Proposed: Always dmabuf FBO

1. **Always initialize dmabuf pool** in `WaylandSubsurface::init()` regardless of HDR/SDR. The pool creation is identical — only the image description differs.

2. **Set SDR image description** when `output_is_hdr_` is false:
   ```
   wp_image_description_creator_params_v1_set_primaries_named(creator, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
   wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB);
   ```
   This matches standalone mpv's SDR path (`wayland_common.c` `set_color_management` with sRGB TF and BT.709 primaries).

3. **Remove `VkSurfaceKHR` creation and swapchain fallback** from the Wayland path. The VkSurface is currently only used for the swapchain — without it, we don't need `vkCreateWaylandSurfaceKHR` or `MPV_RENDER_PARAM_VULKAN_SURFACE`.

4. **Remove swapchain-related code** from `VulkanSubsurfaceRenderer::render()` — the `else` branch goes away entirely.

5. **Keep `preferred_changed` handling** — it already works for both paths. For SDR, the display profile luminance values inform whether we're on an HDR display; for HDR, they provide the compositor's target capabilities.

### mpv-side changes

**video.c** already handles both paths:
- FBO path: `pl_video_render()` reads mpv options (`target-trc`, `target-prim`, `target-peak`) to determine target color space. For SDR, these default to sRGB/BT.709 with no peak, which produces correct SDR output.
- For HDR, the PQ passthrough logic copies source HDR metadata to target and writes content_peak to display_profile. This is unchanged.

**context.c**: No swapchain creation needed. The `MPV_RENDER_PARAM_VULKAN_SURFACE` parameter is omitted entirely. The display_profile pointer is still passed via `MPV_RENDER_PARAM_DISPLAY_PROFILE`.

**libmpv_gpu_next.c**: Only the FBO path is used (`wrap_fbo` → `pl_video_render` → `done_frame`). The swapchain path code becomes dead code on Wayland (still used on macOS/Windows).

## Dmabuf Buffer Details

### Format Selection

| Mode | VkFormat | DRM Format | Rationale |
|---|---|---|---|
| HDR | `VK_FORMAT_A2B10G10R10_UNORM_PACK32` | `DRM_FORMAT_ABGR2101010` | 10-bit per channel, matches PQ range |
| SDR | `VK_FORMAT_B8G8R8A8_UNORM` | `DRM_FORMAT_ARGB8888` | 8-bit sRGB, universally supported, matches standalone mpv SDR |

The format should match what standalone mpv uses for dmabuf-wayland. Check with `WAYLAND_DEBUG=1` — standalone mpv's `zwp_linux_buffer_params_v1_add` will show the DRM format code.

### Tiling: Linear vs Optimal with Modifiers

Current implementation uses `VK_IMAGE_TILING_LINEAR` for dmabuf export. This is correct and required because:
- `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT` with no DRM format modifier implies linear
- Universal compositor support (every Wayland compositor handles linear dmabufs)
- No driver-specific modifier negotiation needed

Performance impact: Linear tiling may be ~5-15% slower for GPU writes vs optimal tiling, but for video frame output at typical resolutions (1080p-4K), this is well within budget. The GPU is doing the heavy lifting in libplacebo's render pass, not the final write to the output buffer.

**Future optimization**: Use `zwp_linux_dmabuf_feedback_v1` to learn preferred modifiers and switch to `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`. This is an incremental improvement, not a requirement for correctness.

### Buffer Pool Size

4 buffers (current) is correct:
- 1 being displayed (held by compositor)
- 1 being rendered to
- 2 spare (handles compositor latency and variable frame timing)

## Color Management Protocol Details

### SDR Image Description

```
wp_image_description_creator_params_v1_set_primaries_named(creator, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB)
wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB)
// No luminances, no mastering metadata, no CLL
```

This tells the compositor "this surface contains sRGB content" and lets it handle any display-specific adjustments (e.g., mapping sRGB to the panel's native color space).

### HDR Image Description

```
wp_image_description_creator_params_v1_set_primaries_named(creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020)
wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ)
wp_image_description_creator_params_v1_set_max_cll(creator, lrintf(hdr.max_cll))
wp_image_description_creator_params_v1_set_max_fall(creator, 0)  // matches mpv
wp_image_description_creator_params_v1_set_mastering_display_primaries(creator, ...)
wp_image_description_creator_params_v1_set_mastering_luminance(creator, min, max)
```

All values must match standalone mpv's `set_color_management()` in `wayland_common.c` exactly.

### Handling preferred_changed During SDR Playback

When the compositor sends `preferred_changed` during SDR playback:
1. Query the new preferred description (same as now)
2. Update `preferred_csp_` (same as now)
3. Check if the display is now HDR capable (`max_luma > PL_COLOR_SDR_WHITE`)
4. If transitioning SDR→HDR: rebuild dmabuf pool with 10-bit format, update image description
5. If transitioning HDR→SDR: rebuild dmabuf pool with 8-bit format, update image description
6. If staying SDR: no change needed (sRGB image description is static)

This handles the case where a user toggles HDR/EDR on their display while playing content.

## Resize Behavior

When the window resizes:
1. `setDestinationSize(w, h)` sets `wp_viewport` destination (logical size for HiDPI)
2. `initDmabufPool(physical_w, physical_h)` recreates buffers at new physical dimensions
3. Between pool recreation and first new frame, the old buffer may be stretched by the compositor

This is acceptable — the gap is typically 1-2 frames and matches standalone mpv's behavior. **No artificial stretching** — we let the compositor's default behavior handle the transition rather than scaling a stale texture ourselves.

## Pros

- **Exact standalone mpv parity by construction**: Uses the same `wp_color_management` protocol path, same scaling formulas (ported from `wayland_common.c`), same image description parameters
- **Single rendering path**: Eliminates the `output_is_hdr_` branch in the renderer. One path for all content on all displays.
- **Proven**: The HDR path already works and passes `dev/linux/color-mgmt-compare.sh`
- **Independent of Mesa WSI**: Color management doesn't depend on Mesa's swapchain color space negotiation, which may differ from standalone mpv's explicit protocol usage
- **Clean separation**: App owns the video surface's color identity; compositor handles display mapping
- **Verifiable**: `WAYLAND_DEBUG=1` shows exact protocol messages for both app and standalone mpv

## Cons

- **Linear tiling performance**: ~5-15% slower GPU writes vs optimal-tiled swapchain. Negligible for video output at typical resolutions.
- **Manual buffer management**: Pool allocation, busy tracking, release callbacks. This code already exists and is tested.
- **Dmabuf pool rebuild on format change**: SDR→HDR transition requires destroying and recreating the pool with a different pixel format. This is a brief stall (< 1 frame at 60fps).
- **No automatic format negotiation**: We choose the format rather than letting the compositor suggest one. This is actually a pro for reproducibility.

## Validation

1. `WAYLAND_DEBUG=1 jellyfin-desktop --player test.mkv 2>jf.log`
2. `WAYLAND_DEBUG=1 third_party/mpv/build/mpv test.mkv 2>mpv.log`
3. Compare `wp_image_description_creator_params_v1_set_*` calls
4. Compare `zwp_linux_buffer_params_v1_add` DRM format and modifier
5. Compare `wl_surface_attach` / `wl_surface_commit` patterns
6. Automated: `dev/linux/color-mgmt-compare.sh --hdr test_hdr.mkv --sdr test_sdr.mkv`

## Implementation Estimate

This is an incremental change from the current codebase:

1. **WaylandSubsurface**: Always init dmabuf pool (move from HDR-conditional to unconditional). Add `setSdrImageDescription()`. Possibly adjust dmabuf format based on `output_is_hdr_`.
2. **VulkanSubsurfaceRenderer**: Remove swapchain else-branch.
3. **VideoStack/main.cpp**: Remove swapchain setup code for Wayland (no `VkSurfaceKHR` needed).
4. **mpv context.c**: No changes (FBO path already works).
5. **Tests**: Extend `color-mgmt-compare.sh` to verify SDR path.
