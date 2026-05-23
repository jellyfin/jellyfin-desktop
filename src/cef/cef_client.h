#pragma once

#include <cstddef>
#include <cstdint>

// Opaque handle to the Rust-side layer state (jfn-cef::client). All
// browser-level operations route through the FFI below; the C++ side
// never wraps this in a refcounted class — Browsers (Rust) is the
// sole owner of each handle's lifetime.
struct JfnCefLayer;

extern "C" {
JfnCefLayer* jfn_cef_layer_new();
void         jfn_cef_layer_free(JfnCefLayer*);
void         jfn_cef_layer_set_name(const JfnCefLayer*, const char* utf8);
bool         jfn_cef_layer_is_closed(const JfnCefLayer*);
bool         jfn_cef_layer_is_loaded(const JfnCefLayer*);
void         jfn_cef_layer_wait_for_close(const JfnCefLayer*);
void         jfn_cef_layer_wait_for_load(const JfnCefLayer*);

void         jfn_cef_layer_set_surface(const JfnCefLayer*, void* surface);
void*        jfn_cef_layer_get_surface(const JfnCefLayer*);
void         jfn_cef_layer_resize(const JfnCefLayer*, int w, int h, int pw, int ph);
void         jfn_cef_layer_set_refresh_rate(const JfnCefLayer*, double hz);
void         jfn_cef_layer_kick_invalidate_loop(const JfnCefLayer*);
int          jfn_cef_layer_frame_rate(const JfnCefLayer*);
void         jfn_cef_layer_on_deactivated(const JfnCefLayer*);
void         jfn_cef_layer_create(const JfnCefLayer*, const char* url_utf8, size_t len);
void         jfn_cef_layer_reset(const JfnCefLayer*);
void         jfn_cef_layer_load_url(const JfnCefLayer*, const char* url_utf8, size_t len);
void         jfn_cef_layer_exec_js(const JfnCefLayer*, const char* js_utf8, size_t len);
#if defined(__APPLE__)
void         jfn_cef_layer_send_external_begin_frame(const JfnCefLayer*);
#endif
void         jfn_cef_layer_undo(const JfnCefLayer*);
void         jfn_cef_layer_redo(const JfnCefLayer*);
void         jfn_cef_layer_cut(const JfnCefLayer*);
void         jfn_cef_layer_copy(const JfnCefLayer*);
void         jfn_cef_layer_paste(const JfnCefLayer*);
void         jfn_cef_layer_select_all(const JfnCefLayer*);
void         jfn_cef_layer_free_string(char*);

void         jfn_cef_layer_set_visible(const JfnCefLayer*, bool visible);
void         jfn_cef_layer_fade(const JfnCefLayer*, float sec,
                                void (*start_fn)(void*), void* start_ctx, void (*start_dtor)(void*),
                                void (*done_fn)(void*),  void* done_ctx,  void (*done_dtor)(void*));

// Per-layer injection-profile kind. Built into a DictionaryValue on the Rust
// side at browser-create time.
void         jfn_cef_layer_set_injection_profile_kind(const JfnCefLayer*,
                                                      const char* kind_utf8, size_t len);

// Browser identity + lifecycle for shutdown / active-target compare.
int          jfn_cef_layer_browser_id(const JfnCefLayer*);
void         jfn_cef_layer_close_browser_force(const JfnCefLayer*);

// Browser navigation / focus / input dispatch.
bool         jfn_cef_layer_can_go_back(const JfnCefLayer*);
bool         jfn_cef_layer_can_go_forward(const JfnCefLayer*);
void         jfn_cef_layer_go_back(const JfnCefLayer*);
void         jfn_cef_layer_go_forward(const JfnCefLayer*);
void         jfn_cef_layer_set_focus(const JfnCefLayer*, bool focus);
void         jfn_cef_layer_send_key_event(const JfnCefLayer*, int type_, uint32_t modifiers,
                                          int windows_key_code, int native_key_code,
                                          bool is_system_key, uint16_t character,
                                          uint16_t unmodified_character);
void         jfn_cef_layer_send_mouse_click(const JfnCefLayer*, int x, int y, uint32_t modifiers,
                                            int button, bool mouse_up, int click_count);
void         jfn_cef_layer_send_mouse_move(const JfnCefLayer*, int x, int y, uint32_t modifiers,
                                           bool leave);
void         jfn_cef_layer_send_mouse_wheel(const JfnCefLayer*, int x, int y, uint32_t modifiers,
                                            int dx, int dy);

// Process-wide defaults consumed at browser-create time.
void         jfn_cef_set_default_frame_rate(int hz);
void         jfn_cef_set_use_shared_textures(bool enable);
void         jfn_cef_set_device_profile_json(const char* json_utf8, size_t len);
}
