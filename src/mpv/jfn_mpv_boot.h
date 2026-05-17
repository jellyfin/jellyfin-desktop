#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mpv_handle;

#ifdef __cplusplus
extern "C" {
#endif

// Display backend the C++ side reports. Matches the discriminants of
// enum class DisplayBackend so the C ABI need not negotiate names.
//   0 = Wayland, 1 = X11, 2 = Other (macOS/Windows)

typedef struct {
    uint8_t     display_backend;
    const char* hwdec;
    const char* user_agent;
    const char* audio_passthrough;    // may be NULL
    bool        audio_exclusive;
    const char* audio_channels;       // may be NULL
    const char* geometry;             // may be NULL
    bool        force_window_position;
    bool        window_maximized_at_boot;
    // libmpv log-message subscription level: "no", "error", "warn",
    // "info", "v", "debug", "trace". NULL or empty = no subscription.
    const char* mpv_log_level;
} JfnMpvBoot;

// Create + configure + initialize the libmpv handle. On success
// returns the raw mpv_handle* for the C++ MpvHandle wrapper to borrow;
// on failure returns NULL.
struct mpv_handle* jfn_mpv_handle_init(const JfnMpvBoot* boot);

// Drop the Rust-owned handle. Calls mpv_terminate_destroy. Idempotent.
// On macOS the caller must invoke this off the main thread — mpv's VO
// uninit blocks on DispatchQueue.main.sync.
void jfn_mpv_handle_terminate(void);

// Borrow the live raw mpv_handle*. NULL before init and after terminate.
struct mpv_handle* jfn_mpv_handle_get(void);

#ifdef __cplusplus
}
#endif
