#pragma once

#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_keyboard_handler.h"
#include "../platform/platform_ops.h"
#include <functional>
#include <string>
#include <vector>

class Browsers;
struct PlatformSurface;

// Opaque handle to the Rust-side state in jfn-cef (src/jfn_cef/src/client.rs).
// Owns name/closed/loaded/condvars, the resize-debounce + invalidate-loop
// state machine, and a per-layer surface pointer. Subsequent porting slices
// migrate more behavior in; for now C++ holds the CEF handler impls and the
// CefRefPtr<CefBrowser>.
struct JfnCefLayer;

// Per-layer CEF browser op vtable. C++ supplies fn-pointer thunks during
// CefLayer ctor; the Rust side invokes them when scheduling work on TID_UI.
// ctx is the CefLayer*; thunks must null-check the browser ref themselves.
struct JfnCefBrowserOps {
    void* ctx;
    void (*notify_screen_info_changed)(void* ctx);
    void (*was_resized)(void* ctx);
    void (*invalidate)(void* ctx);
    void (*send_external_begin_frame)(void* ctx);
    void (*set_windowless_frame_rate)(void* ctx, int hz);
    void (*exec_js)(void* ctx, const char* js_utf8, size_t len);
    // Send a CefProcessMessage with the given name (no args) to PID_RENDERER.
    void (*send_process_message_named)(void* ctx, const char* name_utf8, size_t len);
    // "applyPopupSelection" IPC + mouse-wheel dismiss at (-1,-1). Used by
    // native-menu popup backends (macOS) when the user picks an option.
    void (*dispatch_popup_selection)(void* ctx, int index);
    // CefBrowserHost::CreateBrowser with the layer as client. extra_info,
    // CefWindowInfo + CefBrowserSettings prepared in the thunk.
    void (*create_browser)(void* ctx, const char* url_utf8, size_t len);
    void (*close_browser)(void* ctx);
    void (*load_url)(void* ctx, const char* url_utf8, size_t len);
};

extern "C" {
JfnCefLayer* jfn_cef_layer_new();
void         jfn_cef_layer_free(JfnCefLayer*);
void         jfn_cef_layer_set_name(const JfnCefLayer*, const char* utf8);
bool         jfn_cef_layer_is_closed(const JfnCefLayer*);
bool         jfn_cef_layer_is_loaded(const JfnCefLayer*);
void         jfn_cef_layer_set_closed(const JfnCefLayer*, bool);
void         jfn_cef_layer_set_loaded(const JfnCefLayer*, bool);
void         jfn_cef_layer_wait_for_close(const JfnCefLayer*);
void         jfn_cef_layer_wait_for_load(const JfnCefLayer*);

void         jfn_cef_layer_set_browser_ops(const JfnCefLayer*, const JfnCefBrowserOps*);
void         jfn_cef_layer_clear_browser_ops(const JfnCefLayer*);
void         jfn_cef_layer_set_surface(const JfnCefLayer*, void* surface);
void         jfn_cef_layer_resize(const JfnCefLayer*, int w, int h, int pw, int ph);
void         jfn_cef_layer_set_refresh_rate(const JfnCefLayer*, double hz);
void         jfn_cef_layer_kick_invalidate_loop(const JfnCefLayer*);
bool         jfn_cef_layer_should_present_paint(const JfnCefLayer*);
void         jfn_cef_layer_get_view_rect(const JfnCefLayer*, int* w, int* h);
void         jfn_cef_layer_get_screen_info(const JfnCefLayer*, float* scale, int* w, int* h);
void         jfn_cef_layer_stop_invalidate(const JfnCefLayer*);
int          jfn_cef_layer_frame_rate(const JfnCefLayer*);
void         jfn_cef_layer_bump_resize_gen(const JfnCefLayer*);
void         jfn_cef_layer_on_popup_show(const JfnCefLayer*, bool show);
void         jfn_cef_layer_on_popup_size(const JfnCefLayer*, int x, int y, int w, int h);
void         jfn_cef_layer_set_popup_options(const JfnCefLayer*,
                                             const char* const* options, size_t len,
                                             int selected_idx);
void         jfn_cef_layer_on_deactivated(const JfnCefLayer*);
void         jfn_cef_layer_on_paint(const JfnCefLayer*, bool is_popup,
                                    const JfnRect* dirty, size_t n,
                                    const void* buffer, int w, int h);
void         jfn_cef_layer_on_accelerated_paint(const JfnCefLayer*, bool is_popup,
                                                const void* accel_paint_info);
void         jfn_cef_layer_create(const JfnCefLayer*, const char* url_utf8, size_t len);
void         jfn_cef_layer_reset(const JfnCefLayer*);
void         jfn_cef_layer_load_url(const JfnCefLayer*, const char* url_utf8, size_t len);
int          jfn_cef_layer_on_after_created(const JfnCefLayer*);
void         jfn_cef_layer_on_before_close_hook(const JfnCefLayer*);
char*        jfn_cef_layer_take_pending_url(const JfnCefLayer*);
bool         jfn_cef_layer_on_before_popup(const JfnCefLayer*, const char* url_utf8, size_t len);
void         jfn_cef_layer_free_string(char*);
}

// Callback invoked for IPC messages from the renderer process.
// Returns true if the message was handled.
using MessageHandler = std::function<bool(const std::string& name,
                                         CefRefPtr<CefListValue> args,
                                         CefRefPtr<CefBrowser> browser)>;

// Callback invoked after the browser is created (OnAfterCreated).
using CreatedCallback = std::function<void(CefRefPtr<CefBrowser>)>;

// Callback invoked just before the browser is destroyed (OnBeforeClose).
using BeforeCloseCallback = std::function<void()>;

// Callbacks for app-level context menu items. CefLayer is policy-free: it
// asks the app to append items and to dispatch unknown command IDs.
using ContextMenuBuilder = std::function<void(CefRefPtr<CefMenuModel>)>;
using ContextMenuDispatcher = std::function<bool(int command_id)>;

// Generic CEF browser client — pure rendering, lifecycle, context menu,
// keyboard. Business logic is injected via setMessageHandler / setCreatedCallback.
// CefLayer holds a generic PlatformSurface*; presents/resizes/visibility
// route through g_platform.surface_*.
class CefLayer : public CefClient, public CefRenderHandler,
                 public CefLifeSpanHandler, public CefLoadHandler,
                 public CefContextMenuHandler, public CefDisplayHandler,
                 public CefKeyboardHandler {
public:
    CefLayer(Browsers& browsers, PlatformSurface* surface);
    ~CefLayer() override;

    void setName(std::string name) {
        name_ = std::move(name);
        jfn_cef_layer_set_name(rs_, name_.c_str());
    }
    const std::string& name() const { return name_; }

    void setMessageHandler(MessageHandler handler) { message_handler_ = std::move(handler); }
    void setCreatedCallback(CreatedCallback cb) { on_after_created_ = std::move(cb); }
    void setBeforeCloseCallback(BeforeCloseCallback cb) { on_before_close_ = std::move(cb); }
    void setContextMenuBuilder(ContextMenuBuilder cb) { context_menu_builder_ = std::move(cb); }
    void setContextMenuDispatcher(ContextMenuDispatcher cb) { context_menu_dispatcher_ = std::move(cb); }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }

    bool OnPreKeyEvent(CefRefPtr<CefBrowser>, const CefKeyEvent&,
                       CefEventHandle, bool* is_keyboard_shortcut) override;

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override;
    void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override;
    void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&,
                 const void*, int w, int h) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                            const RectList&, const CefAcceleratedPaintInfo& info) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser>) override;
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int popup_id,
                       const CefString& target_url, const CefString& target_frame_name,
                       WindowOpenDisposition target_disposition, bool user_gesture,
                       const CefPopupFeatures&, CefWindowInfo&,
                       CefRefPtr<CefClient>&, CefBrowserSettings&,
                       CefRefPtr<CefDictionaryValue>&, bool* no_javascript_access) override;
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override;
    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                     ErrorCode, const CefString& errorText, const CefString& failedUrl) override;

    // CefDisplayHandler
    void OnFullscreenModeChange(CefRefPtr<CefBrowser>, bool fullscreen) override;
    bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                        cef_cursor_type_t type, const CefCursorInfo&) override;
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level,
                          const CefString& message, const CefString& source,
                          int line) override;

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                  CefProcessId, CefRefPtr<CefProcessMessage> message) override;

    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                             CefRefPtr<CefContextMenuParams>,
                             CefRefPtr<CefMenuModel> model) override;
    bool RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                        CefRefPtr<CefContextMenuParams>, CefRefPtr<CefMenuModel>,
                        CefRefPtr<CefRunContextMenuCallback>) override;

    void resize(int w, int h, int physical_w, int physical_h);
    // Mirror of the renderer-side rAF deadline loop (cef_app.cpp). Each
    // resize bumps a 5s sliding deadline; while live, periodic
    // Invalidate(PET_VIEW) on TID_UI keeps the host nudging the renderer
    // even when JS rAF wouldn't fire (e.g. when the page is static and
    // the renderer skipped a compositor frame).
    void kickInvalidateLoop();
    bool isClosed() const { return jfn_cef_layer_is_closed(rs_); }
    bool isLoaded() const { return jfn_cef_layer_is_loaded(rs_); }
    CefRefPtr<CefBrowser> browser() { return browser_; }
    void waitForClose() { jfn_cef_layer_wait_for_close(rs_); }
    void waitForLoad() { jfn_cef_layer_wait_for_load(rs_); }
    void execJs(const std::string& js);
    void setRefreshRate(double hz);
    void setVisible(bool visible);
    void fade(float fade_sec,
              std::function<void()> on_fade_start,
              std::function<void()> on_complete);

    PlatformSurface* surface() const { return surface_; }

    // Native-shim injection profile travels to the renderer's
    // OnBrowserCreated; carries jmpNative function list + script list.
    // Set once by the owning subclass; reused across reset() cycles.
    void setInjectionProfile(CefRefPtr<CefDictionaryValue> p) { extra_info_ = std::move(p); }

    // Create the underlying CEF browser. Builds CefWindowInfo and
    // CefBrowserSettings from the Browsers display state.
    void create(const std::string& url);

    // Tear down the current browser and recreate with no URL (blank).
    void reset();

    // Navigate the current browser to `url`.
    void loadUrl(const std::string& url);

    // Called by Browsers when this layer stops being the input target,
    // or just before its surface is freed. Tears down anything that
    // shouldn't outlive active status — currently the popup.
    void onDeactivated();

    // Browser-op create thunk — dispatches CefBrowserHost::CreateBrowser
    // with WindowInfo/Settings/extra_info synthesized from extra_info_ +
    // context_menu_builder_. Invoked from Rust via JfnCefBrowserOps.
    void doCreateBrowser(const std::string& url);

private:
    Browsers& browsers_;
    PlatformSurface* surface_ = nullptr;
    std::string name_;
    CefRefPtr<CefBrowser> browser_;
    JfnCefLayer* rs_ = nullptr;
    CefRefPtr<CefRunContextMenuCallback> pending_menu_callback_;
    MessageHandler message_handler_;
    CreatedCallback on_after_created_;
    BeforeCloseCallback on_before_close_;
    ContextMenuBuilder context_menu_builder_;
    ContextMenuDispatcher context_menu_dispatcher_;
    CefRefPtr<CefDictionaryValue> extra_info_;
    IMPLEMENT_REFCOUNTING(CefLayer);
};
