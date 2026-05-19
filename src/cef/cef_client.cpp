#include "cef_client.h"
#include "logging.h"
#include "../browser/browsers.h"
#include "../platform/platform.h"
#include "include/cef_browser.h"
// Raw C-struct pointers handed across the Rust→C++ boundary must be turned
// into proper C++ wrappers via the CToCpp translators (NOT static_cast); the
// C and C++ types are not layout-compatible.
#include "libcef_dll/ctocpp/browser_ctocpp.h"
#include "libcef_dll/ctocpp/list_value_ctocpp.h"
#include "libcef_dll/ctocpp/menu_model_ctocpp.h"

namespace {

struct VoidFnHolder { std::function<void()> fn; };
void void_fn_thunk(void* ctx) {
    auto* h = static_cast<VoidFnHolder*>(ctx);
    if (h->fn) h->fn();
}
void void_fn_dtor(void* ctx) {
    delete static_cast<VoidFnHolder*>(ctx);
}

struct MessageHandlerHolder { MessageHandler fn; };
bool message_handler_thunk(void* ctx, const char* name_utf8, size_t len,
                           void* args, void* browser) {
    auto* h = static_cast<MessageHandlerHolder*>(ctx);
    CefRefPtr<CefListValue> args_ref =
        CefListValueCToCpp::Wrap(static_cast<cef_list_value_t*>(args));
    CefRefPtr<CefBrowser> browser_ref =
        CefBrowserCToCpp::Wrap(static_cast<cef_browser_t*>(browser));
    return h->fn(std::string(name_utf8, len), args_ref, browser_ref);
}
void message_handler_dtor(void* ctx) {
    delete static_cast<MessageHandlerHolder*>(ctx);
}

struct CreatedCallbackHolder { CreatedCallback fn; };
void created_callback_thunk(void* ctx, void* /*browser_raw*/) {
    auto* h = static_cast<CreatedCallbackHolder*>(ctx);
    if (h->fn) h->fn();
}
void created_callback_dtor(void* ctx) {
    delete static_cast<CreatedCallbackHolder*>(ctx);
}

struct BeforeCloseHolder { BeforeCloseCallback fn; };
void before_close_thunk(void* ctx) {
    auto* h = static_cast<BeforeCloseHolder*>(ctx);
    h->fn();
}
void before_close_dtor(void* ctx) {
    delete static_cast<BeforeCloseHolder*>(ctx);
}

struct CtxMenuBuilderHolder { ContextMenuBuilder fn; };
void ctx_menu_builder_thunk(void* ctx, void* menu_model) {
    auto* h = static_cast<CtxMenuBuilderHolder*>(ctx);
    CefRefPtr<CefMenuModel> model_ref =
        CefMenuModelCToCpp::Wrap(static_cast<cef_menu_model_t*>(menu_model));
    h->fn(model_ref);
}
void ctx_menu_builder_dtor(void* ctx) {
    delete static_cast<CtxMenuBuilderHolder*>(ctx);
}

struct CtxMenuDispatcherHolder { ContextMenuDispatcher fn; };
bool ctx_menu_dispatcher_thunk(void* ctx, int command_id) {
    auto* h = static_cast<CtxMenuDispatcherHolder*>(ctx);
    return h->fn(command_id);
}
void ctx_menu_dispatcher_dtor(void* ctx) {
    delete static_cast<CtxMenuDispatcherHolder*>(ctx);
}

}  // namespace

CefLayer::CefLayer(Browsers& browsers, PlatformSurface* surface)
    : browsers_(browsers), surface_(surface), rs_(jfn_cef_layer_new()) {
    jfn_cef_layer_set_surface(rs_, surface);
}

CefLayer::~CefLayer() {
    jfn_cef_layer_free(rs_);
}

void CefLayer::resize(int w, int h, int physical_w, int physical_h) {
    LOG_TRACE(LOG_CEF, "CefLayer::resize name={} logical={}x{} physical={}x{}",
             name_.c_str(), w, h, physical_w, physical_h);
    jfn_cef_layer_resize(rs_, w, h, physical_w, physical_h);
}

void CefLayer::kickInvalidateLoop() { jfn_cef_layer_kick_invalidate_loop(rs_); }
void CefLayer::setVisible(bool visible) { jfn_cef_layer_set_visible(rs_, visible); }
void CefLayer::setRefreshRate(double hz) { jfn_cef_layer_set_refresh_rate(rs_, hz); }
void CefLayer::onDeactivated() { jfn_cef_layer_on_deactivated(rs_); }
void CefLayer::create(const std::string& url) {
    jfn_cef_layer_create(rs_, url.data(), url.size());
}
void CefLayer::reset() { jfn_cef_layer_reset(rs_); }
void CefLayer::loadUrl(const std::string& url) {
    jfn_cef_layer_load_url(rs_, url.data(), url.size());
}

void CefLayer::fade(float fade_sec,
                    std::function<void()> on_fade_start,
                    std::function<void()> on_complete) {
    auto* start = new VoidFnHolder{std::move(on_fade_start)};
    auto* done = new VoidFnHolder{std::move(on_complete)};
    jfn_cef_layer_fade(rs_, fade_sec,
                       &void_fn_thunk, start, &void_fn_dtor,
                       &void_fn_thunk, done,  &void_fn_dtor);
}

void CefLayer::execJs(const std::string& js) {
    jfn_cef_layer_exec_js(rs_, js.data(), js.size());
}

void CefLayer::setMessageHandler(MessageHandler handler) {
    auto* h = new MessageHandlerHolder{std::move(handler)};
    jfn_cef_layer_set_message_handler(rs_, (void*)&message_handler_thunk,
                                      h, &message_handler_dtor);
}
void CefLayer::setCreatedCallback(CreatedCallback cb) {
    auto* h = new CreatedCallbackHolder{std::move(cb)};
    jfn_cef_layer_set_created_callback(rs_, (void*)&created_callback_thunk,
                                       h, &created_callback_dtor);
}
void CefLayer::setBeforeCloseCallback(BeforeCloseCallback cb) {
    auto* h = new BeforeCloseHolder{std::move(cb)};
    jfn_cef_layer_set_before_close_callback(rs_, (void*)&before_close_thunk,
                                            h, &before_close_dtor);
}
void CefLayer::setContextMenuBuilder(ContextMenuBuilder cb) {
    auto* h = new CtxMenuBuilderHolder{std::move(cb)};
    jfn_cef_layer_set_context_menu_builder(rs_, (void*)&ctx_menu_builder_thunk,
                                           h, &ctx_menu_builder_dtor);
}
void CefLayer::setContextMenuDispatcher(ContextMenuDispatcher cb) {
    auto* h = new CtxMenuDispatcherHolder{std::move(cb)};
    jfn_cef_layer_set_context_menu_dispatcher(rs_, (void*)&ctx_menu_dispatcher_thunk,
                                              h, &ctx_menu_dispatcher_dtor);
}
