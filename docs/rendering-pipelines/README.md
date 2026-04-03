# Wayland Rendering Pipeline: mpv + CEF Overlay

## The Problem

jellyfin-desktop needs to render video (via mpv/libplacebo) with a transparent browser UI (via CEF) composited on top, on Wayland, matching standalone mpv's HDR and SDR output exactly. "Exactly" means the `wp_image_description` protocol messages are byte-identical to standalone mpv when compared via `WAYLAND_DEBUG=1`.

This is harder than it sounds. The difficulty isn't rendering video — libplacebo handles that. The difficulty is that Wayland's color management protocol operates on **surfaces**, and our application has multiple surfaces for different content types (video in PQ/BT.2020, UI in sRGB). Where the image description lands, which surface owns color management, and how the compositor blends across color space boundaries — these are the real design decisions.

## What Standalone mpv Does

Standalone mpv's preferred Wayland VO (`dmabuf-wayland`) uses a clean three-surface model:

```
main surface (xdg_toplevel) — black, empty, coordinates commits
  +-- video subsurface — hw-decoded dmabuf, color-managed via wp_color_management
  +-- osd subsurface — wl_shm ARGB8888 subtitles (implicit sRGB)
```

The video surface carries the `wp_image_description`. The OSD surface has no description (implicit sRGB). The compositor handles cross-color-space alpha blending between them. This is the gold standard we're targeting.

## What We Have Now

```
SDL parent surface — EGL/CEF renders here, wp_image_description HERE
  +-- mpv subsurface (below) — Vulkan dmabuf [HDR] or swapchain [SDR]
```

Two problems:
1. **Two rendering paths**: HDR uses dmabuf FBO with explicit color management; SDR uses a libplacebo swapchain with Mesa WSI. Different code paths for the same goal.
2. **Color management surface mismatch**: The `wp_image_description` is set on the parent surface (which hosts CEF's sRGB content), not on the mpv subsurface (which has the actual HDR video). The compositor sees PQ/BT.2020 metadata on a surface whose primary rendered content is sRGB.

## My Assessment

I see five viable architectures, ranging from "fix what we have" to "rebuild the surface stack." Here's how I think about them:

### The pragmatic path: Option A

[Option A (Unified Dmabuf FBO)](proposals/option-a-unified-dmabuf.md) is the smallest change that solves problem #1. Extend the existing HDR dmabuf path to also handle SDR — just set an sRGB image description instead of PQ/BT.2020. One rendering path, no `output_is_hdr_` branch. The protocol values match standalone mpv because we're using the same `wp_color_management` calls with the same scaling formulas ported from `wayland_common.c`.

It doesn't fix the surface mismatch (problem #2). The image description still goes on the parent surface. In practice this might not matter — KDE Plasma 6.x applies the image description to the entire surface tree, and the compositor blends correctly regardless. But it's architecturally wrong and could break on a compositor that applies the description only to the parent's own content.

**This is where I'd start.** Low risk, fast to ship, solves the immediate problem of HDR/SDR protocol parity.

### The architecturally correct paths: Options D and E

If the surface mismatch matters in practice, [Option D (mpv Primary + CEF Subsurface)](proposals/option-d-mpv-primary-cef-subsurface.md) inverts the hierarchy — mpv renders to the parent surface, CEF goes on a subsurface above. The image description is now on the surface that carries video. Clean.

[Option E (Triple Subsurface)](proposals/option-e-triple-subsurface.md) mirrors standalone mpv's architecture exactly — empty parent, video subsurface, CEF subsurface. Each surface has one job. Color management on the video surface. This is the "purest" design.

Both D and E require the same big prerequisite: **decoupling CEF from SDL's EGL surface.** CEF currently renders via OpenGL on the parent surface. Moving CEF to a subsurface means building a new presentation path: CEF's `OnAcceleratedPaint` provides dmabufs, those dmabufs become `wl_buffer` objects attached to a subsurface. The OpenGL compositor goes away. This is significant refactoring but the pieces exist — CEF already exports dmabufs on Linux, and the dmabuf-to-`wl_buffer` path is already implemented for mpv's video buffers.

The `preferred_changed` question matters here: KWin may only send color feedback to top-level surfaces, not subsurfaces. Option D avoids this (mpv IS the top-level). Option E needs the read-from-parent, write-to-child mitigation.

### What I'd skip

[Option B (Unified Swapchain)](proposals/option-b-unified-swapchain.md) can't meet the protocol parity requirement for HDR. Mesa's WSI sends different `wp_image_description` parameters than standalone mpv — missing mastering display metadata and CLL. This is a Mesa limitation, not something we can fix. Fine for SDR-only.

[Option C (Single-Surface Compositing)](proposals/option-c-single-surface.md) is the most technically interesting but the cost/benefit is wrong. Compositing sRGB CEF over PQ video in a Vulkan shader requires color space conversion in linear light, which is exactly the kind of hand-rolled color math that libplacebo exists to avoid. And the main benefit — atomic resize — is solving a problem that barely exists (subsurface resize gaps are 1-2 frames).

Three alternatives were researched and eliminated entirely: dual Vulkan swapchain sync (frame callback deadlock), vo_dmabuf_wayland direct presentation (hardware-decode only, no software fallback), and Wayland layer shell (wrong protocol, no GNOME, no input passthrough).

## Recommended Sequence

1. **Now**: Option A. Unify rendering on dmabuf FBO. One path for HDR and SDR. Validate protocol parity via `dev/linux/color-mgmt-compare.sh`.
2. **Test**: Verify on KDE Plasma 6.x and wlroots compositors that the surface mismatch doesn't cause visible issues.
3. **If needed**: Option D. Invert the surface hierarchy. Build the CEF dmabuf-to-subsurface path. Move `wp_image_description` to the correct surface.
4. **Long term**: Option E if we want to match standalone mpv's surface architecture exactly, or if Option D reveals compositor quirks with image descriptions on the parent while subsurfaces below carry the actual content.

## Documents

### Proposals

| Document | Summary |
|---|---|
| [Option A: Unified Dmabuf FBO](proposals/option-a-unified-dmabuf.md) | Extend HDR dmabuf path to SDR. Single rendering path. Small delta. |
| [Option B: Unified Swapchain](proposals/option-b-unified-swapchain.md) | libplacebo swapchain for everything. Can't match HDR protocol. |
| [Option C: Single-Surface Compositing](proposals/option-c-single-surface.md) | Vulkan compositor pass. Atomic resize. Very complex. |
| [Option D: mpv Primary + CEF Subsurface](proposals/option-d-mpv-primary-cef-subsurface.md) | Invert hierarchy. Correct color management surface. CEF as subsurface. |
| [Option E: Triple Subsurface](proposals/option-e-triple-subsurface.md) | Mirror standalone mpv exactly. Empty parent, video sub, CEF sub. |

### Analysis

| Document | Summary |
|---|---|
| [mpv gpu-next Analysis](proposals/mpv-gpu-next-analysis.md) | Custom libmpv rendering backend internals: render paths, color management, luminance scaling, FBO wrapping, OSD |

## Validation

All options must pass `dev/linux/color-mgmt-compare.sh`, which compares `WAYLAND_DEBUG=1` output between `jellyfin-desktop --player` and standalone `third_party/mpv/build/mpv` for both HDR and SDR content. Key verification: `wp_image_description_creator_params_v1_set_*` calls, `zwp_linux_buffer_params_v1_add` DRM format/modifier, mastering metadata values, and `lrintf` rounding of luminance scaling.
