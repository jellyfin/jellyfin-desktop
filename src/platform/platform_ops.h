#pragma once

// C-ABI mirror of the subset of `g_platform` (src/platform/platform.h) needed
// by the Rust-side CefLayer port (src/jfn_cef/src/client.rs). Each function
// pointer here is a thunk that translates the repr(C) argument shapes into
// the `g_platform`-native C++ types and forwards. Layout must match
// `JfnPlatformOps` in src/jfn_cef/src/platform_ops.rs.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JfnRect {
    int x;
    int y;
    int w;
    int h;
} JfnRect;

// On-selected callback uses a (fn, ctx, dtor) triple; `dtor(ctx)` runs once
// when the C++ side drops its last reference to the std::function wrapper
// (allowing the Rust side to free any boxed state).
typedef struct JfnPopupRequest {
    int x;
    int y;
    int lw;
    int lh;
    const char* const* options;   // each entry NUL-terminated UTF-8
    size_t options_len;
    int initial_highlight;        // -1 = none
    void (*on_selected)(void* ctx, int index);  // index = -1 means dismissed
    void* on_selected_ctx;
    void (*on_selected_dtor)(void* ctx);
} JfnPopupRequest;

typedef struct JfnPlatformOps {
    bool (*surface_present)(void* surface, const void* accel_paint_info);
    bool (*surface_present_software)(void* surface,
                                     const JfnRect* dirty, size_t dirty_len,
                                     const void* buffer, int w, int h);
    void (*surface_resize)(void* surface, int lw, int lh, int pw, int ph);
    void (*surface_set_visible)(void* surface, bool visible);

    void (*fade_surface)(void* surface, float fade_sec,
                         void (*on_start)(void*), void* start_ctx,
                         void (*start_dtor)(void*),
                         void (*on_done)(void*), void* done_ctx,
                         void (*done_dtor)(void*));

    void (*popup_show)(void* surface, const JfnPopupRequest* req);
    void (*popup_hide)(void* surface);
    void (*popup_present)(void* surface, const void* accel_paint_info,
                          int lw, int lh);
    void (*popup_present_software)(void* surface, const void* buffer,
                                   int pw, int ph, int lw, int lh);

    void (*set_fullscreen)(bool fullscreen);
    void (*set_cursor)(int cef_cursor_type);
    void (*clipboard_read_text_async)(
        void (*cb)(void* ctx, const char* utf8, size_t len),
        void* ctx,
        void (*dtor)(void* ctx));
    void (*open_external_url)(const char* utf8, size_t len);
} JfnPlatformOps;

// Process-static vtable. Each thunk inside forwards to `g_platform.<field>`.
const JfnPlatformOps* jfn_platform_ops(void);

// Rust setter — implemented in jfn-cef crate. Stores the supplied pointer so
// subsequent Rust calls (slices 3+) can dispatch through it.
void jfn_cef_set_platform_ops(const JfnPlatformOps* ops);

#ifdef __cplusplus
}
#endif
