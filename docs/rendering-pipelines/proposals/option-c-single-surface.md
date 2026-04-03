# Option C: Single-Surface Vulkan Compositing

## Summary

Both mpv and CEF render to offscreen Vulkan textures. A final Vulkan render pass composites the video layer and the CEF overlay into a single output, which is presented on one Wayland surface via either a libplacebo swapchain or dmabuf export. Color management is controlled by the application on this single surface.

## Architecture

```
SDL Window (single wl_surface)
  |
  +-- Vulkan compositor (custom render pass)
  |     |
  |     +-- Video layer: mpv renders to offscreen VkImage (FBO)
  |     |     +-- libplacebo gpu-next rendering
  |     |     +-- Full color management (same as standalone mpv)
  |     |
  |     +-- UI layer: CEF renders to shared texture
  |     |     +-- OnAcceleratedPaint → shared texture handle (dmabuf or GPU memory)
  |     |     +-- Premultiplied alpha, sRGB color space
  |     |
  |     +-- Composite: video under UI with alpha blending
  |           +-- Single fullscreen quad (video), overlaid with UI quad
  |           +-- Output to presentation target
  |
  +-- Presentation: dmabuf wl_buffer OR libplacebo swapchain
  +-- wp_color_management_v1 image description (one surface, one description)
```

### Rendering Flow

```
Per frame:
  1. mpv renders to offscreen VkImage (FBO path, same as Option A)
     -> pl_video_render() with target_color = PQ/BT.2020 (HDR) or linear sRGB
     -> content mastering metadata written to display_profile

  2. CEF provides UI texture
     -> OnAcceleratedPaint callback provides dmabuf fd + metadata
     -> Import into Vulkan as VkImage (external memory)
     -> Or: CEF paints to shared VkImage via GL/VK interop

  3. Vulkan compositor pass
     -> Bind video texture as sampler (layer 0)
     -> Bind CEF texture as sampler (layer 1)
     -> Fragment shader: alpha-blend CEF over video
     -> Render to output VkImage (framebuffer)

  4. Present output
     -> dmabuf path: export VkImage, wl_surface_attach
     -> or swapchain: pl_swapchain_submit_frame

  5. Set wp_image_description based on video color space
```

### Compositing Shader

```glsl
// Fragment shader for video + UI compositing
#version 450

layout(set = 0, binding = 0) uniform sampler2D video_tex;
layout(set = 0, binding = 1) uniform sampler2D cef_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag_color;

void main() {
    vec4 video = texture(video_tex, uv);
    vec4 ui = texture(cef_tex, uv);

    // CEF provides premultiplied alpha in sRGB.
    // Video is in output color space (PQ/BT.2020 for HDR, sRGB for SDR).
    //
    // For correct blending, both must be in the same color space.
    // Convert CEF sRGB to video color space before blending.
    // (Implementation depends on output color space)

    // Alpha-over compositing (premultiplied alpha)
    frag_color = ui + video * (1.0 - ui.a);
}
```

## CEF Texture Sharing

This is the primary technical challenge. CEF must provide its rendered UI as a Vulkan-accessible texture.

### Option C.1: CEF Offscreen Rendering + CPU Copy

```
CEF OnPaint() callback → BGRA buffer in system memory
  → vkMapMemory + memcpy → VkImage (host-visible)
  → Use as sampler input
```

**Pros**: Simple, no GPU interop needed
**Cons**: CPU copy per frame (~4ms at 1080p, ~16ms at 4K), unacceptable latency

### Option C.2: CEF OnAcceleratedPaint + dmabuf Import

```
CEF OnAcceleratedPaint() → dmabuf fd + stride + modifier
  → vkCreateImage with VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
  → Import dmabuf fd as VkDeviceMemory
  → Bind to VkImage → use as sampler input
```

**Pros**: Zero-copy, GPU-native
**Cons**: Requires CEF's shared texture support on Linux (available with `--enable-features=UseOzonePlatform --ozone-platform=wayland`), format negotiation complexity, synchronization between CEF's GPU process and our Vulkan compositor

### Option C.3: CEF to GL Texture + Vulkan Interop

```
CEF renders to GL texture (via OnAcceleratedPaint or custom compositor)
  → Export GL texture as dmabuf (EGL_MESA_image_dma_buf_export)
  → Import dmabuf into Vulkan (same as C.2)
```

**Pros**: Works with CEF's existing GL-based rendering
**Cons**: GL↔VK interop overhead, synchronization (GL flush + VK semaphore)

### Option C.4: CEF on Separate Subsurface (Hybrid)

```
mpv renders to single surface via compositor pass
CEF renders on a separate Wayland subsurface above
  → Standard CEF overlay (same as current architecture)
  → No texture sharing needed
```

**Pros**: No CEF texture interop at all
**Cons**: Back to subsurface architecture for CEF (two surfaces again, just inverted)

## Color Space Compositing Challenges

### The Alpha Blending Problem

CEF renders in sRGB with premultiplied alpha. mpv renders in the video's output color space (PQ/BT.2020 for HDR, sRGB/gamma 2.2 for SDR).

**For SDR**: Both are sRGB — alpha blending is straightforward.

**For HDR**: The UI overlay is in sRGB but the video is in PQ/BT.2020. Correct compositing requires:
1. Convert CEF's sRGB premultiplied alpha content to PQ/BT.2020
2. Alpha-blend in linear light (not in PQ transfer function, which is perceptual)
3. Re-encode to PQ for output

This is non-trivial. The naive approach (blend in PQ space) produces incorrect luminance for semi-transparent UI elements — text with drop shadows, progress bar overlays, etc. would appear too bright or too dark.

The correct approach:
```
CEF sRGB → linearize (sRGB EOTF) → gamut map BT.709 → BT.2020 → scale to PQ luminance range
Video PQ → linearize (PQ EOTF) → already BT.2020
Blend in linear light
Output → PQ OETF → output buffer
```

This requires implementing color space conversion in the compositing shader. libplacebo can do this, but we'd be doing our own compositing pass outside of mpv's render pipeline.

### Alternative: Compositor Handles the Blend

With the subsurface architecture (Options A/B), the **Wayland compositor** handles the alpha blend. The compositor knows the color space of each surface (from `wp_image_description`) and performs color-correct compositing. This is the intended Wayland design — applications don't composite; the compositor does.

By moving compositing into our application (Option C), we take on responsibility for color-correct blending that the compositor would otherwise handle for us.

## Resize Behavior

**This is Option C's strongest advantage.**

With subsurfaces (Options A/B), resize involves:
1. SDL resizes parent surface → immediate
2. mpv subsurface gets new size → dmabuf pool rebuild or swapchain recreate → 1-3 frame delay
3. During the gap, compositor stretches old buffer or shows black

With single-surface compositing:
1. SDL resizes surface → swapchain/dmabuf recreation
2. Next frame renders mpv + CEF at new size → immediate
3. No gap — both layers resize atomically

However, this advantage only matters if resize quality is a problem in practice. On Wayland with `wl_subsurface_set_desync()`, the gap is typically 1-2 frames, and most users won't notice at 60+ fps.

## Performance

### Additional GPU Cost

| Component | Time (1080p, ~estimate) | Time (4K) |
|---|---|---|
| mpv FBO render | Same as Options A/B | Same |
| CEF texture import | ~0.1ms (zero-copy dmabuf) | ~0.1ms |
| Compositing pass | ~0.2ms (single fullscreen quad) | ~0.5ms |
| Color space conversion (HDR) | ~0.3ms (PQ ↔ linear) | ~0.8ms |
| **Total overhead vs Option A** | **~0.6ms** | **~1.4ms** |

The overhead is small in absolute terms but adds latency to every frame. At 4K 120Hz (8.3ms budget), 1.4ms is 17% of the frame budget.

### Memory Cost

- 1 additional VkImage for compositing output (same size as video)
- CEF texture import (shared, no additional allocation)
- Compositing pipeline resources (shader, descriptors, render pass)

## Pros

- **Atomic resize**: Both video and UI resize in the same frame. No inter-surface synchronization issues.
- **Single color management point**: One surface, one `wp_image_description`. No risk of mismatched color spaces between surfaces.
- **Full control**: Application controls all compositing, can implement custom transitions, effects, or blending modes.
- **No subsurface edge cases**: Avoids compositor-specific subsurface behavior differences (z-ordering, synchronization, input passthrough).

## Cons

- **Significant complexity**: Requires Vulkan compositing pipeline, CEF texture sharing, color-correct alpha blending in multiple color spaces.
- **CEF texture sharing is hard**: On Linux, CEF's accelerated paint path requires specific build flags and has limited driver support. The CPU-copy fallback has unacceptable performance.
- **Color space compositing is error-prone**: Blending sRGB UI over PQ video in linear light requires careful implementation. Getting this wrong produces visible artifacts (wrong brightness for semi-transparent elements).
- **Additional latency**: Compositing pass adds ~0.5-1.5ms per frame depending on resolution.
- **Reinventing the compositor**: Wayland's design delegates compositing to the compositor, which already handles color-correct blending between surfaces. Doing it ourselves means maintaining our own compositor.
- **Testing surface doubles**: Every change to the compositing pipeline needs testing across GPU vendors, driver versions, and compositors.
- **Not what standalone mpv does**: Standalone mpv doesn't composite anything — it renders to its own surface and lets the compositor handle overlays. Matching standalone mpv's protocol output is possible (same dmabuf/image description), but the rendering path is fundamentally different.

## When This Option Makes Sense

Option C is the right choice if:
1. Subsurface resize quality is **unacceptable** and cannot be fixed (e.g., for a kiosk/signage use case where resize jank during window management is visible)
2. The project needs custom compositing effects between video and UI (e.g., video blur behind semi-transparent UI panels)
3. The project is willing to invest in a Vulkan compositing pipeline and maintain it long-term
4. CEF's accelerated paint path is stable and well-supported on target hardware

## Risk Assessment

| Risk | Severity | Mitigation |
|---|---|---|
| CEF texture sharing doesn't work reliably on Linux | High | CPU-copy fallback (poor performance) |
| Color-space compositing artifacts | High | Extensive testing with HDR test patterns |
| Additional latency pushes past frame budget at 4K 120Hz | Medium | Profile and optimize; consider async compositing |
| Maintenance burden of custom compositor | High | Abstract behind clean interface; test matrix |
| Mesa/driver bugs in VK_EXTERNAL_MEMORY import | Medium | Test across vendors (Intel, AMD, NVIDIA) |

## Conclusion

Option C is the most architecturally clean solution — one surface, one color identity, atomic resize — but the implementation complexity is substantially higher than Options A or B. The primary blocker is reliable CEF texture sharing on Linux, which is still maturing. The color-space compositing challenge for HDR adds further risk.

**Recommendation**: Consider Option C as a future evolution after Option A is stable and validated. The subsurface approach (Option A) provides exact standalone mpv parity with much less complexity. If resize quality becomes a user-visible problem, Option C can be pursued incrementally by first solving CEF texture sharing in isolation, then building the compositing pipeline.
