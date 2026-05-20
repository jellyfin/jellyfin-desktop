#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Single-plane dmabuf frame info — what the C++ trampoline unpacks
// from CefAcceleratedPaintInfo before calling into the Rust paint path.
typedef struct {
    int32_t  fd;
    uint32_t stride;
    uint64_t modifier;
    int32_t  coded_w, coded_h;
    int32_t  visible_w, visible_h;
} JfnDmabufFrame;

// Bootstrap. `display` is an mpv-owned `*mut wl_display` (never closed
// here); `parent_surface` is an mpv-owned `*mut wl_proxy` referencing a
// `wl_surface`. Globals are bound on a dedicated EventQueue.
bool jfn_wl_core_init(void* display, void* parent_surface);
void jfn_wl_core_set_was_fullscreen(bool fs);

// Whole-Platform lifecycle for the Wayland backend. Replaces the
// orchestration body that used to live in C++ wl_init/wl_cleanup —
// reads mpv's wayland-display/-surface, primes input + clipboard +
// KDE palette + dmabuf probe, installs the xdg_toplevel close-cb
// trampoline. `_init` returns false on the same conditions the C++
// version did (missing mpv handles, core init failure).
bool jfn_wl_lifecycle_init(void);
void jfn_wl_lifecycle_cleanup(void);

// Surface lifecycle. Opaque handle returned by alloc_surface is a
// boxed `PlatformSurface` on the Rust side.
void* jfn_wl_alloc_surface(void);
void  jfn_wl_free_surface(void* surface);
void  jfn_wl_restack(void* const* surfaces, size_t n);
void  jfn_wl_surface_resize(void* surface, int32_t lw, int32_t lh,
                            int32_t pw, int32_t ph);
void  jfn_wl_surface_set_visible(void* surface, bool visible,
                                 uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);

// Paint.
bool jfn_wl_surface_present(void* surface, const JfnDmabufFrame* frame);
bool jfn_wl_surface_present_software(void* surface, const uint8_t* pixels,
                                     int32_t w, int32_t h);
void jfn_wl_popup_show(void* surface, int32_t x, int32_t y,
                       int32_t lw, int32_t lh);
void jfn_wl_popup_hide(void* surface);
void jfn_wl_popup_present(void* surface, const JfnDmabufFrame* frame,
                          int32_t lw, int32_t lh);
void jfn_wl_popup_present_software(void* surface, const uint8_t* pixels,
                                   int32_t pw, int32_t ph,
                                   int32_t lw, int32_t lh);

// Fullscreen / transition.
void jfn_wl_set_fullscreen(bool fullscreen);
void jfn_wl_toggle_fullscreen(void);
void jfn_wl_begin_transition(void);
void jfn_wl_end_transition(void);
bool jfn_wl_in_transition(void);
bool jfn_wl_was_fullscreen(void);

// xdg_toplevel.configure callback — invoked from on_proxy_configure
// (which still lives in C++ because it talks to jfn_playback_post_osd_pixels).
void jfn_wl_on_configure(int32_t width, int32_t height, int32_t fullscreen);

// Per-frame fade callback — wired into jfn_wl_fade_start by the C++
// fade_surface trampoline. Signature matches JfnWlFadeApply.
bool jfn_wl_fade_apply_frame(void* surface, uint32_t alpha);

#ifdef __cplusplus
}
#endif
