#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Apply one frame of alpha to the surface. Implemented on the C++ side
// (wayland.cpp owns the wp_alpha_modifier protocol). Must take whatever
// lock guards the surface stack and return false if the surface is gone.
typedef bool (*JfnWlFadeApply)(void* surface, uint32_t alpha);

// Start a single-surface alpha fade on a dedicated thread.
//
// Behaviour on success (returns true):
//   1. fires on_start(start_ctx) once before the loop
//   2. runs total_frames = fade_sec * fps frames, calling apply() per frame
//      under the implementation's surface lock
//   3. fires on_done(done_ctx) once on natural completion (skipped on abort)
//
// Behaviour when the caller should skip the animation entirely (returns
// false): surface is NULL, or fps <= 0. The Rust side runs start_dtor /
// done_dtor on the contexts it received but does NOT fire start/done — the
// C++ caller does that itself for the early-skip path so the lifecycle
// semantics match the original wl_fade_surface.
//
// Starting a new fade preempts any in-flight fade: the previous thread
// receives the stop flag and is joined before this one starts. The
// preempted fade's on_done is NOT fired (matches the original behaviour).
bool jfn_wl_fade_start(void* surface,
                       float fade_sec,
                       double fps,
                       JfnWlFadeApply apply,
                       void (*on_start)(void*), void* start_ctx, void (*start_dtor)(void*),
                       void (*on_done)(void*), void* done_ctx, void (*done_dtor)(void*));

// Stop any in-flight fade and join its thread. Call from C++ cleanup
// before destroying the alpha-modifier proxy.
void jfn_wl_fade_stop_all(void);

#ifdef __cplusplus
}
#endif
