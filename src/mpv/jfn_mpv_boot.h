#pragma once

#include <stdbool.h>
#include <stddef.h>
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

// Hardware-decode mode helpers — replace the legacy hwdecOptions /
// isValidHwdec / kHwdecDefault inline helpers from mpv/options.h.
// Returned strings have static lifetime.
const char* jfn_mpv_hwdec_default(void);
size_t      jfn_mpv_hwdec_options_count(void);
const char* jfn_mpv_hwdec_options_get(size_t i);
bool        jfn_mpv_is_valid_hwdec(const char* s);

// Decoder + demuxer enumeration — replaces the legacy
// mpv_capabilities::Query from src/mpv/capabilities.cpp.
typedef struct JfnMpvCapabilities JfnMpvCapabilities;

JfnMpvCapabilities* jfn_mpv_capabilities_query(struct mpv_handle* h);
void                jfn_mpv_capabilities_free(JfnMpvCapabilities* p);
size_t              jfn_mpv_capabilities_decoder_count(const JfnMpvCapabilities* p);
const char*         jfn_mpv_capabilities_decoder_name(const JfnMpvCapabilities* p, size_t i);
// Decoder kind: 0 = Video, 1 = Audio, 2 = Subtitle. 0xFF on out-of-range.
uint8_t             jfn_mpv_capabilities_decoder_kind(const JfnMpvCapabilities* p, size_t i);
size_t              jfn_mpv_capabilities_demuxer_count(const JfnMpvCapabilities* p);
const char*         jfn_mpv_capabilities_demuxer_name(const JfnMpvCapabilities* p, size_t i);

#ifdef __cplusplus
}
#endif
