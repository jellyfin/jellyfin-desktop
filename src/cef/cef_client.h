#pragma once

#include "include/cef_base.h"
#include "include/cef_browser.h"
#include "include/cef_menu_model.h"
#include "include/cef_values.h"
#include <cstdint>
#include <functional>
#include <string>

class Browsers;
struct PlatformSurface;

// Opaque handle to the Rust-side layer state (jfn-cef::client). Owns all
// state, the CEF Client + 6 handler impls (wrap_* macros), and the stored
// CefBrowser captured at LifeSpanHandler::on_after_created.
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

typedef void (*JfnCbDtor)(void*);
void         jfn_cef_layer_set_message_handler(const JfnCefLayer*, void* fn, void* ctx, JfnCbDtor);
void         jfn_cef_layer_set_created_callback(const JfnCefLayer*, void* fn, void* ctx, JfnCbDtor);
void         jfn_cef_layer_set_before_close_callback(const JfnCefLayer*, void* fn, void* ctx, JfnCbDtor);
void         jfn_cef_layer_set_context_menu_builder(const JfnCefLayer*, void* fn, void* ctx, JfnCbDtor);
void         jfn_cef_layer_set_context_menu_dispatcher(const JfnCefLayer*, void* fn, void* ctx, JfnCbDtor);
bool         jfn_cef_layer_has_context_menu_builder(const JfnCefLayer*);

void         jfn_cef_layer_set_visible(const JfnCefLayer*, bool visible);
void         jfn_cef_layer_fade(const JfnCefLayer*, float sec,
                                void (*start_fn)(void*), void* start_ctx, JfnCbDtor start_dtor,
                                void (*done_fn)(void*),  void* done_ctx,  JfnCbDtor done_dtor);

// Per-layer injection-profile kind. Built into a DictionaryValue on the Rust
// side at browser-create time.
void         jfn_cef_layer_set_injection_profile_kind(const JfnCefLayer*,
                                                      const char* kind_utf8, size_t len);

// Browser identity + lifecycle for shutdown / active-target compare.
int          jfn_cef_layer_browser_id(const JfnCefLayer*);
void         jfn_cef_layer_close_browser_force(const JfnCefLayer*);

// Browser navigation / focus / input dispatch — all routed through the
// per-layer Rust state.
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

// Callback invoked for IPC messages from the renderer process.
// Returns true if the message was handled.
using MessageHandler = std::function<bool(const std::string& name,
                                         CefRefPtr<CefListValue> args,
                                         CefRefPtr<CefBrowser> browser)>;

// Created/before-close callbacks: the layer owns its browser; consumers
// reference the layer (not the underlying CefBrowser) for input-target etc.
using CreatedCallback = std::function<void()>;
using BeforeCloseCallback = std::function<void()>;
using ContextMenuBuilder = std::function<void(CefRefPtr<CefMenuModel>)>;
using ContextMenuDispatcher = std::function<bool(int command_id)>;

// Thin C++ shim around the Rust-side CefLayer. C++ never sees the underlying
// CefBrowser* — all browser-level operations (input, focus, close, identity)
// route through layer FFI.
class CefLayer : public CefBaseRefCounted {
public:
    CefLayer(Browsers& browsers, PlatformSurface* surface);
    ~CefLayer();

    void setName(std::string name) {
        name_ = std::move(name);
        jfn_cef_layer_set_name(rs_, name_.c_str());
    }
    const std::string& name() const { return name_; }

    void setMessageHandler(MessageHandler handler);
    void setCreatedCallback(CreatedCallback cb);
    void setBeforeCloseCallback(BeforeCloseCallback cb);
    void setContextMenuBuilder(ContextMenuBuilder cb);
    void setContextMenuDispatcher(ContextMenuDispatcher cb);

    void resize(int w, int h, int physical_w, int physical_h);
    void kickInvalidateLoop();
    bool isClosed() const { return jfn_cef_layer_is_closed(rs_); }
    bool isLoaded() const { return jfn_cef_layer_is_loaded(rs_); }
    int  browserId() const { return jfn_cef_layer_browser_id(rs_); }
    bool hasBrowser() const { return browserId() != 0; }
    void closeBrowserForce() { jfn_cef_layer_close_browser_force(rs_); }
    void waitForClose() { jfn_cef_layer_wait_for_close(rs_); }
    void waitForLoad() { jfn_cef_layer_wait_for_load(rs_); }
    void execJs(const std::string& js);
    void setRefreshRate(double hz);
    void setVisible(bool visible);
    void fade(float fade_sec,
              std::function<void()> on_fade_start,
              std::function<void()> on_complete);

    PlatformSurface* surface() const { return surface_; }

    // Native-shim injection-profile kind ("web" / "overlay" / "about"). The
    // Rust side materializes the DictionaryValue lazily at browser-create.
    void setInjectionProfileKind(const char* kind) {
        std::string s = kind ? kind : "";
        jfn_cef_layer_set_injection_profile_kind(rs_, s.data(), s.size());
    }

    void create(const std::string& url);
    void reset();
    void loadUrl(const std::string& url);
    void onDeactivated();

    // Navigation + focus + input dispatch.
    bool canGoBack() const { return jfn_cef_layer_can_go_back(rs_); }
    bool canGoForward() const { return jfn_cef_layer_can_go_forward(rs_); }
    void goBack() { jfn_cef_layer_go_back(rs_); }
    void goForward() { jfn_cef_layer_go_forward(rs_); }
    void setFocus(bool f) { jfn_cef_layer_set_focus(rs_, f); }
    void sendKeyEvent(int type_, uint32_t modifiers, int windows_key_code,
                      int native_key_code, bool is_system_key,
                      uint16_t character, uint16_t unmodified_character) {
        jfn_cef_layer_send_key_event(rs_, type_, modifiers, windows_key_code,
                                     native_key_code, is_system_key,
                                     character, unmodified_character);
    }
    void sendMouseClick(int x, int y, uint32_t modifiers, int button,
                        bool mouse_up, int click_count) {
        jfn_cef_layer_send_mouse_click(rs_, x, y, modifiers, button, mouse_up, click_count);
    }
    void sendMouseMove(int x, int y, uint32_t modifiers, bool leave) {
        jfn_cef_layer_send_mouse_move(rs_, x, y, modifiers, leave);
    }
    void sendMouseWheel(int x, int y, uint32_t modifiers, int dx, int dy) {
        jfn_cef_layer_send_mouse_wheel(rs_, x, y, modifiers, dx, dy);
    }
#if defined(__APPLE__)
    void sendExternalBeginFrame() { jfn_cef_layer_send_external_begin_frame(rs_); }
#endif
    void undo()      { jfn_cef_layer_undo(rs_); }
    void redo()      { jfn_cef_layer_redo(rs_); }
    void cut()       { jfn_cef_layer_cut(rs_); }
    void copy()      { jfn_cef_layer_copy(rs_); }
    void paste()     { jfn_cef_layer_paste(rs_); }
    void selectAll() { jfn_cef_layer_select_all(rs_); }

private:
    Browsers& browsers_;
    PlatformSurface* surface_ = nullptr;
    std::string name_;
    JfnCefLayer* rs_ = nullptr;
    IMPLEMENT_REFCOUNTING(CefLayer);
};
