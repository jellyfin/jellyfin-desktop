#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JfnInputWayland JfnInputWayland;

// All callbacks fire on the Rust input thread. The C++ side is responsible
// for any cross-thread coordination needed by the dispatch layer (matches
// the previous C++ implementation, which dispatched from its own thread).

typedef void (*JfnMouseMoveFn)(int32_t x, int32_t y, uint32_t modifiers, int leave);
typedef void (*JfnMouseButtonFn)(uint32_t button_code, int pressed,
                                 int32_t x, int32_t y, uint32_t modifiers);
typedef void (*JfnScrollFn)(int32_t x, int32_t y, int32_t dx, int32_t dy, uint32_t modifiers);
typedef void (*JfnHistoryNavFn)(int forward);
typedef void (*JfnKbFocusFn)(int gained);
typedef void (*JfnKeyFn)(uint32_t keysym, uint32_t native_code,
                        uint32_t modifiers, int pressed);
typedef void (*JfnCharFn)(uint32_t codepoint, uint32_t modifiers, uint32_t native_code);

typedef struct {
    JfnMouseMoveFn   mouse_move;
    JfnMouseButtonFn mouse_button;
    JfnScrollFn      scroll;
    JfnHistoryNavFn  history_nav;
    JfnKbFocusFn     kb_focus;
    JfnKeyFn         key;
    JfnCharFn        char_;
} JfnInputCallbacks;

// CEF EVENTFLAG_* values surfaced as constants for callers that build
// modifier masks across the FFI boundary.
#define JFN_EVENTFLAG_SHIFT_DOWN          (1u << 1)
#define JFN_EVENTFLAG_CONTROL_DOWN        (1u << 2)
#define JFN_EVENTFLAG_ALT_DOWN            (1u << 3)
#define JFN_EVENTFLAG_LEFT_MOUSE_BUTTON   (1u << 4)
#define JFN_EVENTFLAG_MIDDLE_MOUSE_BUTTON (1u << 5)
#define JFN_EVENTFLAG_RIGHT_MOUSE_BUTTON  (1u << 6)

// Wraps a foreign-owned wl_display, opens its own EventQueue, binds wl_seat
// and wp_cursor_shape_manager_v1 from its own registry view. `display` must
// outlive the returned handle. Caller retains ownership of `display`.
// The input layer owns its own wake fd internally; callers do not provide one.
// Returns NULL on failure.
JfnInputWayland* jfn_input_wayland_init(void* display,
                                        const JfnInputCallbacks* callbacks);

// Spawn the input thread. Idempotent: only the first call starts it.
void jfn_input_wayland_start(JfnInputWayland* ctx);

// Apply a cursor shape (CEF cef_cursor_type_t value, numeric). Safe from
// any thread.
void jfn_input_wayland_set_cursor(JfnInputWayland* ctx, uint32_t cef_cursor_type);

// Joins the input thread, destroys all Rust-owned Wayland proxies.
void jfn_input_wayland_cleanup(JfnInputWayland* ctx);

#ifdef __cplusplus
}
#endif
