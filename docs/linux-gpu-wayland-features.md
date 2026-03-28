# Linux GPU and Wayland Features

Comprehensive listing of all Wayland protocols, Vulkan extensions, EGL features, DRM/KMS capabilities, and libplacebo features used by Jellyfin Desktop on Linux.

## 1. Wayland Protocols

### Core Wayland

| Protocol | Version | Purpose | Source |
|----------|---------|---------|--------|
| `wl_display` | Core | Wayland display connection | `src/platform/wayland_subsurface.cpp:93-94` |
| `wl_registry` | Core | Global object discovery | `src/platform/wayland_subsurface.cpp:104` |
| `wl_compositor` | v4 | Surface creation | `src/platform/wayland_subsurface.cpp:66-68` |
| `wl_subcompositor` | v1 | Subsurface creation (video layer below CEF) | `src/platform/wayland_subsurface.cpp:69-71` |
| `wl_surface` | Core | Surface abstraction for rendering | `src/platform/wayland_subsurface.cpp:117-148` |
| `wl_subsurface` | Core | Subsurface positioning/z-ordering | `src/platform/wayland_subsurface.cpp:124-133` |
| `wl_region` | Core | Input region masking (pass-through to parent) | `src/platform/wayland_subsurface.cpp:136-138` |

### Stable Extensions

| Protocol | Version | Purpose | Source |
|----------|---------|---------|--------|
| `wp_viewporter` | v1 | HiDPI viewport scaling (render at physical, display at logical) | `src/platform/wayland_subsurface.cpp:76-79` |
| `wp_viewport` | v1 | Viewport destination sizing | `src/platform/wayland_subsurface.cpp:140-145, 561` |

### Staging Extensions

| Protocol | Version | Purpose | Source |
|----------|---------|---------|--------|
| `wp_color_manager_v1` | v1 | HDR color management (PQ/BT.2020) | `src/platform/wayland_subsurface.cpp:72-75` |
| `wp_color_management_surface_v1` | v1 | Per-surface color management | `src/platform/wayland_subsurface.cpp:83-85, 426-427` |
| `wp_image_description_v1` | v1 | HDR image description (primaries, TF, luminance) | `src/platform/wayland_subsurface.cpp:85, 446-462` |
| `wp_image_description_creator_params_v1` | v1 | Parametric HDR creation (ST2084 PQ, BT.2020) | `src/platform/wayland_subsurface.cpp:446-462` |
| `xdg_activation_v1` | v1 | Window activation via app token | `src/window_activation.cpp:7, 32-34` |

### KDE/Plasma Extensions

| Protocol | Version | Purpose | Source |
|----------|---------|---------|--------|
| `org_kde_kwin_server_decoration_palette_manager` | v1 | Per-window titlebar color | `src/platform/kde_decoration_palette.cpp:10, 242-245` |
| `org_kde_kwin_server_decoration_palette` | v1 | Titlebar palette application | `src/platform/kde_decoration_palette.cpp:262, 298` |

### Display Event Loop

| Interface | Purpose | Source |
|-----------|---------|--------|
| `wl_display_get_fd()` | Obtain FD for poll integration | `src/platform/event_loop_linux.cpp:22` |
| `wl_display_roundtrip()` | Synchronous protocol round-trip | `src/platform/wayland_subsurface.cpp:106, 149, 254, 472` |
| `wl_display_flush()` | Flush pending requests | `src/platform/event_loop_linux.cpp:55` |

## 2. Vulkan Extensions

### Instance Extensions

| Extension | Purpose | Source |
|-----------|---------|--------|
| `VK_KHR_SURFACE` | Base surface abstraction | `src/platform/wayland_subsurface.cpp:169` |
| `VK_KHR_WAYLAND_SURFACE` | Wayland surface creation | `src/platform/wayland_subsurface.cpp:170` |
| `VK_EXT_SWAPCHAIN_COLOR_SPACE` | HDR color space (PASS_THROUGH) | `src/platform/wayland_subsurface.cpp:171` |
| `VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2` | Extended device properties | `src/platform/wayland_subsurface.cpp:172` |
| `VK_KHR_EXTERNAL_MEMORY_CAPABILITIES` | External memory capability queries | `src/platform/wayland_subsurface.cpp:173` |

### Device Extensions (Required)

| Extension | Purpose | Source |
|-----------|---------|--------|
| `VK_KHR_SWAPCHAIN` | Swapchain creation and presentation | `src/platform/wayland_subsurface.cpp:9` |
| `VK_KHR_TIMELINE_SEMAPHORE` | GPU synchronization (libplacebo) | `src/platform/wayland_subsurface.cpp:10` |
| `VK_KHR_EXTERNAL_MEMORY` | External memory interop (dmabuf) | `src/platform/wayland_subsurface.cpp:11` |
| `VK_KHR_EXTERNAL_MEMORY_FD` | FD-based memory export for dmabuf | `src/platform/wayland_subsurface.cpp:12` |
| `VK_KHR_IMAGE_FORMAT_LIST` | Format list for image views | `src/platform/wayland_subsurface.cpp:13` |
| `VK_KHR_SAMPLER_YCBCR_CONVERSION` | YCbCr sampling for video | `src/platform/wayland_subsurface.cpp:14` |
| `VK_KHR_BIND_MEMORY_2` | Advanced memory binding | `src/platform/wayland_subsurface.cpp:15` |
| `VK_KHR_GET_MEMORY_REQUIREMENTS_2` | Enhanced memory requirements query | `src/platform/wayland_subsurface.cpp:16` |
| `VK_KHR_MAINTENANCE_1` | Maintenance fixes | `src/platform/wayland_subsurface.cpp:17` |

### Device Extensions (Optional)

| Extension | Purpose | Source |
|-----------|---------|--------|
| `VK_EXT_EXTERNAL_MEMORY_DMA_BUF` | DMA-BUF memory import/export | `src/platform/wayland_subsurface.cpp:22` |
| `VK_EXT_IMAGE_DRM_FORMAT_MODIFIER` | DRM format modifier support | `src/platform/wayland_subsurface.cpp:23` |
| `VK_EXT_HDR_METADATA` | HDR metadata for displays | `src/platform/wayland_subsurface.cpp:24` |

## 3. Vulkan Features & Capabilities

### API Version

Target: Vulkan 1.3 (`VK_API_VERSION_1_3`). Requires 1.1 (samplerYcbcrConversion) and 1.2 (timelineSemaphore).

### Enabled Device Features

| Feature | Purpose | Source |
|---------|---------|--------|
| `samplerYcbcrConversion` (Vulkan 1.1) | YCbCr color space sampling | `src/platform/wayland_subsurface.cpp:264` |
| `timelineSemaphore` (Vulkan 1.2) | Timeline semaphore sync | `src/platform/wayland_subsurface.cpp:269` |
| `hostQueryReset` (Vulkan 1.2) | Host-side query reset | `src/platform/wayland_subsurface.cpp:270` |
| `shaderStorageImageReadWithoutFormat` | Unformatted image reads (libplacebo) | `src/context/vulkan_context.cpp:134` |
| `shaderStorageImageWriteWithoutFormat` | Unformatted image writes (libplacebo) | `src/context/vulkan_context.cpp:135` |

### Swapchain Formats (HDR)

| Format | Color Space | Use Case | Source |
|--------|-------------|----------|--------|
| `VK_FORMAT_R16G16B16A16_UNORM` | `PASS_THROUGH_EXT` | 16-bit HDR (preferred) | `src/platform/wayland_subsurface.cpp:322-331` |
| `VK_FORMAT_A2B10G10R10_UNORM_PACK32` | `PASS_THROUGH_EXT` | 10-bit HDR fallback | `src/platform/wayland_subsurface.cpp:338-340` |
| `VK_FORMAT_A2R10G10B10_UNORM_PACK32` | `PASS_THROUGH_EXT` | 10-bit HDR fallback | `src/platform/wayland_subsurface.cpp:338-340` |
| `VK_FORMAT_B8G8R8A8_UNORM` | `SRGB_NONLINEAR_KHR` | SDR fallback | `src/platform/wayland_subsurface.cpp:318-319` |

### HDR Metadata

| Parameter | Value | Source |
|-----------|-------|--------|
| Display primaries | BT.2020 (R: 0.708/0.292, G: 0.170/0.797, B: 0.131/0.046) | `src/platform/wayland_subsurface.cpp:453-456` |
| White point | D65 (0.3127/0.3290) | `src/platform/wayland_subsurface.cpp:453-456` |
| Transfer function | ST2084 (PQ) | `src/platform/wayland_subsurface.cpp:456` |
| Max luminance | 1000 nits | `src/platform/wayland_subsurface.cpp:461` |
| Min luminance | 1 nit | `src/platform/wayland_subsurface.cpp:461` |
| Max content light level | 1000 nits | `src/platform/wayland_subsurface.cpp:461` |
| Max frame average light level | 200 nits | `src/platform/wayland_subsurface.cpp:462` |
| Render intent | Perceptual | `src/platform/wayland_subsurface.cpp:483` |

## 4. EGL Extensions & Features

### Platforms

| Platform | Purpose | Source |
|----------|---------|--------|
| `EGL_PLATFORM_WAYLAND_KHR` | Wayland display | `src/context/egl_context.cpp:39` |
| `EGL_PLATFORM_X11_KHR` | X11 display | `src/context/egl_context.cpp:118` |

### Core Configuration

| Feature | Value | Source |
|---------|-------|--------|
| OpenGL ES version | 3.0 | `src/context/egl_context.cpp:78-79` |
| Surface format | 8-bit RGBA | `src/context/egl_context.cpp:62-65` |
| Surface type | `EGL_WINDOW_BIT` | `src/context/egl_context.cpp:61` |
| Renderable type | `EGL_OPENGL_ES3_BIT` | `src/context/egl_context.cpp:66` |
| Swap interval | 1 (vsync) | `src/context/egl_context.cpp:190` |

### DMA-BUF Extensions

| Extension | Purpose | Source |
|-----------|---------|--------|
| `EGL_LINUX_DMA_BUF_EXT` | DMA-BUF image creation | `src/context/egl_context.cpp:396` |
| `EGL_LINUX_DRM_FOURCC_EXT` | DRM fourcc format specification | `src/context/egl_context.cpp:389` |
| `EGL_DMA_BUF_PLANE0_FD_EXT` | DMA-BUF file descriptor | `src/context/egl_context.cpp:390` |
| `EGL_DMA_BUF_PLANE0_OFFSET_EXT` | Plane offset | `src/context/egl_context.cpp:391` |
| `EGL_DMA_BUF_PLANE0_PITCH_EXT` | Plane pitch/stride | `src/context/egl_context.cpp:392` |

### EGL Image Extensions

| Function | Purpose | Source |
|----------|---------|--------|
| `eglCreateImageKHR` | Create EGL image from dmabuf | `src/context/egl_context.cpp:310-311` |
| `eglDestroyImageKHR` | Destroy EGL image | `src/context/egl_context.cpp:312-313` |
| `glEGLImageTargetTexture2DOES` | Bind EGL image to GL texture | `src/context/egl_context.cpp:314-315` |

### Device Extensions

| Extension | Purpose | Source |
|-----------|---------|--------|
| `EGL_DEVICE_EXT` | Query device from display | `src/context/egl_context.cpp:330` |
| `EGL_DRM_RENDER_NODE_FILE_EXT` | Get DRM render node path | `src/context/egl_context.cpp:332` |
| `eglQueryDisplayAttribEXT` | Query display device attribute | `src/context/egl_context.cpp:324-325` |
| `eglQueryDeviceStringEXT` | Query device string (DRM node) | `src/context/egl_context.cpp:326-327` |

### Wayland-EGL Integration

| Interface | Purpose | Source |
|-----------|---------|--------|
| `wl_egl_window_create()` | Create EGL window at pixel size | `src/context/egl_context.cpp:93` |
| `wl_egl_window_resize()` | Resize EGL window | `src/context/egl_context.cpp:230` |
| `wl_egl_window_destroy()` | Cleanup EGL window | `src/context/egl_context.cpp:214` |

## 5. DRM/KMS & GBM

### GBM Functions

| Function | Purpose | Source |
|----------|---------|--------|
| `gbm_create_device()` | Create GBM device from DRM FD | `src/context/egl_context.cpp:357` |
| `gbm_bo_create()` | Create buffer object | `src/context/egl_context.cpp:366` |
| `gbm_bo_get_fd()` | Export buffer as DMA-BUF FD | `src/context/egl_context.cpp:375` |
| `gbm_bo_get_stride()` | Get buffer pitch/stride | `src/context/egl_context.cpp:376` |
| `gbm_bo_destroy()` | Free buffer object | `src/context/egl_context.cpp:415` |
| `gbm_device_destroy()` | Cleanup GBM device | `src/context/egl_context.cpp:416` |

### DRM Formats

| Format | Purpose | Source |
|--------|---------|--------|
| `DRM_FORMAT_ARGB8888` | 32-bit color with alpha (dmabuf probe) | `src/context/egl_context.cpp:366, 389` |

### Render Node Access

| Feature | Purpose | Source |
|---------|---------|--------|
| DRM render node via EGL | Query `EGL_DRM_RENDER_NODE_FILE_EXT` | `src/context/egl_context.cpp:332` |
| Render node fallback scan | Scan `/dev/dri/renderD128-135` | `src/context/egl_context.cpp:343-348` |

## 6. libplacebo / gpu-next

| Component | Purpose | Source |
|-----------|---------|--------|
| libplacebo rendering | Video rendering via mpv gpu-next backend | `third_party/mpv/video/out/gpu_next/` |
| Custom gpu-next path | Vulkan FBO wrapping for libmpv | `third_party/mpv/video/out/gpu_next/context.c` |
| YCbCr sampling | Hardware color space conversion | `src/platform/wayland_subsurface.cpp:264` |
| Timeline semaphores | GPU task synchronization | `src/platform/wayland_subsurface.cpp:269` |
| Unformatted image I/O | Required by libplacebo shaders | `src/context/vulkan_context.cpp:134-135` |

## 7. Linux Event Loop

| Feature | Purpose | Source |
|---------|---------|--------|
| `eventfd(EFD_NONBLOCK \| EFD_CLOEXEC)` | Cross-thread event loop wakeup | `src/platform/event_loop_linux.cpp:12` |
| `poll()` | Monitor Wayland/X11 FD + eventfd | `src/platform/event_loop_linux.cpp:44-56` |

## 8. Platform Detection

| Check | Purpose | Source |
|-------|---------|--------|
| `SDL_GetCurrentVideoDriver()` | Detect Wayland vs X11 | `src/context/egl_context.cpp:22` |
| `SDL_PROP_WINDOW_WAYLAND_*` | Wayland-specific properties | `src/context/egl_context.cpp:29-32` |
| `SDL_PROP_WINDOW_X11_*` | X11-specific properties | `src/context/egl_context.cpp:109-111` |

## Summary

| Category | Count |
|----------|-------|
| Wayland protocols (core) | 7 |
| Wayland protocols (extensions) | 7 |
| KDE protocols | 2 |
| Vulkan instance extensions | 5 |
| Vulkan device extensions (required) | 9 |
| Vulkan device extensions (optional) | 3 |
| Vulkan device features | 5 |
| EGL extensions | 10+ |
| GBM/DRM features | 8 |
