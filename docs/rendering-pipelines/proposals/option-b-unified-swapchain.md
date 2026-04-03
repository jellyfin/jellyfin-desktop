# Option B: Unified libplacebo Swapchain

## Summary

Unify both HDR and SDR rendering paths to use a libplacebo-managed Vulkan swapchain on the mpv subsurface. Color management is handled by libplacebo's swapchain color hints, which flow through Mesa's WSI layer to the Wayland compositor. The app provides a `display_profile` with compositor capabilities; libplacebo and Mesa handle the rest.

## Architecture

```
SDL Window (parent wl_surface)
  |
  +-- mpv wl_subsurface (place_below, desync, empty input region)
  |     |
  |     +-- VkSurfaceKHR -> libplacebo pl_swapchain
  |     |     +-- Format negotiation (RGBA16F for HDR, BGRA8 for SDR)
  |     |     +-- pl_swapchain_colorspace_hint() per frame
  |     |     +-- VkSwapchainKHR created/managed by Mesa + libplacebo
  |     +-- wp_viewporter for HiDPI logical size mapping
  |
  +-- CEF on parent surface (transparent overlay)
```

### Rendering Flow

```
Per frame:
  1. dispatchColorEvents()              — drain preferred_changed events
  2. pl_swapchain_resize(w, h)          — resize swapchain if needed
  3. pl_swapchain_colorspace_hint(csp)  — tell libplacebo what color space we want
     -> libplacebo passes to Mesa WSI -> Mesa sets wp_image_description
  4. pl_swapchain_start_frame()         — acquire VkImage from swapchain
  5. pl_video_render_to_swapchain()     — render frame with display_profile
  6. pl_swapchain_submit_frame()        — submit GPU commands
  7. pl_swapchain_swap_buffers()        — vsync wait + present
```

### Color Management Flow

```
Compositor preferred_changed event
  -> handlePreferredChanged()
     -> query preferred description (same as Option A)
     -> scale luminances to PL_COLOR_SDR_WHITE reference
     -> store in display_profile_ (max_luma, ref_luma, min_luma)
     -> store in preferred_csp_ for re-hinting

Per frame:
  libmpv_gpu_next.c reads display_profile_:
    if platform_owns_hint (dp has luminance data):
      -> skip hint (platform already set color on surface)  // PROBLEM
    else:
      -> hint with source content color space

  video.c pl_video_render_to_swapchain():
    if display_profile has luminance data:
      -> scale target HDR metadata
    -> render with tone mapping if needed
```

## What Changes from Current Implementation

### Currently

- SDR: swapchain path, Mesa WSI handles color
- HDR: dmabuf FBO path, app handles color via `wp_color_management`
- `output_is_hdr_` determines which path at init time

### Proposed

1. **Always create VkSurface + libplacebo swapchain** (already done for SDR).
2. **Never create dmabuf pool** — remove all dmabuf code from Wayland path.
3. **Use `pl_swapchain_colorspace_hint()`** for HDR format negotiation:
   - HDR source: hint PQ/BT.2020 with content peak → Mesa picks RGBA16F or RGB10A2 + PQ color space
   - SDR source: hint NULL or sRGB → Mesa picks BGRA8 + sRGB
4. **Provide display_profile** to context.c for per-frame swapchain hints and video.c for tone mapping target.

## The Core Problem: Mesa WSI as Intermediary

The fundamental challenge with this option is that **color management goes through Mesa's VkSwapchainKHR implementation**, not directly through `wp_color_management_v1`. The protocol flow is:

```
libplacebo hint → Mesa WSI → Wayland protocol → Compositor
```

vs. standalone mpv (dmabuf-wayland VO):
```
mpv → wp_color_management_v1 → Wayland protocol → Compositor
```

vs. standalone mpv (gpu-next with Vulkan-wayland context):
```
mpv → libplacebo swapchain → Mesa WSI → Wayland protocol → Compositor
```

Standalone mpv's Vulkan-Wayland path (`context_wayland.c`) *also* uses libplacebo's swapchain and Mesa WSI. However, standalone mpv's **default VO for Wayland is `dmabuf-wayland`**, which uses explicit `wp_color_management` — the same approach as our Option A. The `gpu-next` Vulkan-Wayland path is the fallback.

This means:
- If we want to match `dmabuf-wayland` (mpv's default/preferred): Option A is the direct match
- If we want to match `gpu-next` Vulkan-Wayland: Option B would match, but this is mpv's non-default path

### Mesa WSI Color Space Negotiation

Mesa's Vulkan WSI layer negotiates color space via:
1. `vkGetPhysicalDeviceSurfaceFormatsKHR()` → available format/colorspace pairs
2. `vkCreateSwapchainKHR()` with chosen `VkSurfaceFormatKHR`
3. Mesa sets `wp_image_description` based on the `VkColorSpaceKHR`

The mapping from `VkColorSpaceKHR` to `wp_image_description` parameters is **Mesa-internal**. It may not exactly match standalone mpv's explicit protocol values because:
- Mesa may set different luminance ranges
- Mesa may omit mastering display metadata
- Mesa's format support depends on driver version and GPU
- Mesa may not support all VkColorSpaceKHR values needed for PQ/BT.2020

### Verified: Mesa Wayland WSI Behavior (as of Mesa 24.x)

Mesa's `wsi_wl_surface_create_swapchain()` sends:
- `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` → sRGB TF, sRGB primaries (matches standalone mpv SDR)
- `VK_COLOR_SPACE_HDR10_ST2084_EXT` → PQ TF, BT.2020 primaries, but **no mastering metadata or CLL**
- `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` → scRGB linear (macOS/Windows style, not used on Wayland typically)

The **missing mastering metadata and CLL in Mesa's HDR path** is the critical difference. Standalone mpv's `set_color_management()` sends:
```
wp_image_description_creator_params_v1_set_max_cll(lrintf(max_cll))
wp_image_description_creator_params_v1_set_max_fall(0)
wp_image_description_creator_params_v1_set_mastering_display_primaries(...)
wp_image_description_creator_params_v1_set_mastering_luminance(min, max)
```

Mesa's WSI doesn't send any of these. This means:
- The compositor doesn't know the content's actual brightness range
- Tone mapping decisions by the compositor may differ
- `WAYLAND_DEBUG=1` comparison will show different protocol output

## Swapchain Format Selection

| Content | libplacebo hint | Mesa VkColorSpaceKHR | Swapchain VkFormat |
|---|---|---|---|
| SDR | NULL / sRGB | `SRGB_NONLINEAR_KHR` | `B8G8R8A8_SRGB` or `B8G8R8A8_UNORM` |
| HDR PQ | PQ/BT.2020 + peak | `HDR10_ST2084_EXT` | `A2B10G10R10_UNORM_PACK32` |
| HDR HLG | HLG/BT.2020 | `HDR10_HLG_EXT` (if available) | `A2B10G10R10_UNORM_PACK32` |

Format availability depends on Mesa version and GPU driver. If `HDR10_ST2084_EXT` is not available, libplacebo falls back to `SRGB_NONLINEAR` and does tone mapping internally — resulting in visually correct but non-HDR output.

## Resize Behavior

Swapchain resize is handled by `pl_swapchain_resize()`:
1. Internally calls `vkCreateSwapchainKHR()` with new extent
2. Old swapchain images are retired
3. New images are allocated at new size
4. No gap between old and new — swapchain recycle is atomic

This is **slightly better** than dmabuf pool recreation because the swapchain old→new transition is handled by the driver, not application code. However, `pl_swapchain_start_frame()` may block briefly during recreation.

During resize, the compositor may display the stale frame at the old size. This is the same behavior as dmabuf — the compositor stretches or positions the old buffer until a new frame arrives. **No application-side stretching** in either case.

## Pros

- **Simpler code**: No dmabuf pool management, buffer lifecycle, or `wl_buffer` creation. libplacebo and Mesa handle everything.
- **Optimal tiling**: Swapchain images use driver-optimal tiling (not forced linear), potentially better GPU write performance.
- **Automatic format negotiation**: libplacebo queries available formats via `vkGetPhysicalDeviceSurfaceFormatsKHR` and picks the best match. Adapts to GPU capabilities automatically.
- **Standard Vulkan path**: Well-tested across drivers and compositors. This is the path every Vulkan application uses.
- **No VkImage export complexity**: No `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`, no `vkGetMemoryFdKHR`, no linear tiling requirement.
- **Automatic vsync**: `pl_swapchain_swap_buffers()` provides vsync pacing built-in.

## Cons

- **Cannot match standalone mpv's HDR protocol output**: Mesa's WSI sends different (less detailed) `wp_image_description` parameters than standalone mpv's explicit `wp_color_management` usage. Missing mastering display metadata and CLL.
- **Indirect color management**: Color identity flows through Mesa's WSI layer, adding an opaque intermediary. Debugging requires understanding Mesa's internal mapping.
- **Mesa version dependency**: HDR swapchain support requires Mesa 24.x+ with `VK_EXT_swapchain_colorspace`. Older Mesa versions silently fall back to SDR.
- **No per-frame content metadata updates**: Mesa sets the image description at swapchain creation time, not per-frame. Content with varying mastering metadata (common in streaming) won't update the compositor's understanding of the content range.
- **Format availability varies**: `VK_COLOR_SPACE_HDR10_ST2084_EXT` support depends on driver. Intel, AMD, and NVIDIA have different levels of support.
- **Cannot verify parity with standalone mpv via WAYLAND_DEBUG**: Different protocol output means the `color-mgmt-compare.sh` validation script fails.

## When This Option Makes Sense

Option B is appropriate if:
1. Exact standalone mpv protocol parity is **not required** (e.g., "visually similar" is sufficient)
2. The project wants to minimize custom Wayland protocol handling code
3. Mesa WSI's HDR support matures to include mastering metadata (future Mesa versions may close this gap)
4. The target compositors handle missing mastering metadata gracefully (KDE Plasma 6.x does, but with less optimal tone mapping)

## Risk Assessment

| Risk | Severity | Mitigation |
|---|---|---|
| Mesa WSI doesn't send mastering metadata | High | None — requires Mesa changes or fallback to Option A |
| Mesa picks wrong swapchain format for HDR | Medium | Verify with `WAYLAND_DEBUG=1` per driver |
| Swapchain recreation fails during resize | Low | libplacebo handles gracefully (retries) |
| VkColorSpaceKHR not available for PQ | Medium | libplacebo falls back to SDR; log warning |
| Future Mesa changes break protocol output | Medium | Pin Mesa version in CI; add regression test |

## Conclusion

Option B provides the simplest implementation but **cannot currently meet the requirement of byte-identical protocol output** with standalone mpv for HDR content. It is viable for SDR-only deployments or as a fallback when `wp_color_management_v1` is not available, but should not be the primary HDR rendering path.
